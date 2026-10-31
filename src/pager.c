/* Page-based storage with a write-ahead log.
 *
 * One data file (fixed 4 KB pages, page 0 is the database header) and one
 * WAL file. Committed pages are appended to the WAL and fsynced before a
 * commit reports success; a checkpoint copies them into the data file and
 * truncates the WAL. Uncommitted pages live only in memory, so ROLLBACK is
 * just dropping the dirty set, and a crash can never expose them.
 *
 * Deliberate limits (see README): the whole dirty set of a transaction is
 * buffered in memory (bounded by Q_TXN_MAX_PAGES), and there is no free
 * list - deleted space is reused inside pages but the file only grows
 * until a checkpoint compacts live pages.
 */
#include "quarry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>

#define DB_MAGIC   0x31525551u /* "QUR1" */
#define WAL_MAGIC  0x4C415752u /* "RWAL": v2 format, 24-byte self-describing frames */
#define FRAME_MAGIC 0x4D415246u /* "FRAM" */
/* frame header v2: magic, pgno, commit_pages, txn_frames, crc, reserved.
   txn_frames is set on the commit frame only and says how many frames the
   transaction has, so recovery folds exactly that run and never absorbs
   frames of a dead transaction sitting between commits. crc covers the
   pgno/commit_pages/txn_frames fields AND the page. */
#define FRAME_HDR  24

char q_err[256];

/* --- crc32 (IEEE, table-driven) --- */
static uint32_t crc_table[256];
static bool crc_ready;
static void crc_init(void) {
    if (crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = true;
}
/* crc over the frame's semantic header fields plus its page */
static uint32_t crc32_frame(const uint8_t *fh, const uint8_t *page) {
    crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 4; i < 16; i++) c = crc_table[(c ^ fh[i]) & 0xFF] ^ (c >> 8);
    for (size_t i = 0; i < Q_PAGE_SIZE; i++) c = crc_table[(c ^ page[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct dirty_page { uint32_t pgno; uint8_t bytes[Q_PAGE_SIZE]; };

/* committed-but-uncheckpointed pages: open-addressing hash, pgno -> frame offset.
   slot 0 keeps pgno+1 so 0 means empty */
struct walmap { uint32_t *keys; uint64_t *offs; size_t cap, n; };

struct pager {
    int      fd;
    int      wal_fd;
    char    *path;
    char    *wal_path;
    uint32_t page_count;    /* committed page count (main file + WAL view) */
    uint32_t pending_count; /* includes pages allocated in the open txn */
    bool     in_txn;

    struct dirty_page *dirty;
    size_t ndirty, dirty_cap;

    struct walmap wm;
    size_t frames_since_ckpt;

    bool     broken;         /* WAL map rebuild failed; only close is safe */
    size_t   walmap_fail_in; /* test hook: countdown to injected map failure */
    size_t   walmap_fail_period; /* original countdown value (repeats) */

    /* statement savepoint undo log: before-images of pages that were
       already dirty when the savepoint was marked. Truncating the dirty
       set alone cannot undo edits made in place to such pages. */
    bool     sp_active;
    size_t   sp_ndirty;
    uint32_t *undo_pgno;
    uint8_t (*undo_img)[Q_PAGE_SIZE];
    size_t   nundo, undo_cap;

    /* rotating read buffers: a B+tree descent may hold several levels
       at once, so one static buffer is not enough */
    uint8_t rbuf[32][Q_PAGE_SIZE];
    size_t  rnext;
};

/* header page 0 fields (byte offsets) */
#define H_MAGIC   0
#define H_PAGES   4
#define H_CATROOT 8
#define H_CATSEQ  12

static int set_err(int code, const char *msg) {
    snprintf(q_err, sizeof q_err, "%s", msg);
    return code;
}

static void put32(uint8_t *b, uint32_t v) {
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; b[2] = (v >> 16) & 0xFF; b[3] = (v >> 24) & 0xFF;
}
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t get64le(const uint8_t *b) {
    uint64_t u = 0; for (int i = 7; i >= 0; i--) u = (u << 8) | b[i]; return u;
}
static void put64le(uint8_t *b, uint64_t v) { for (int i = 0; i < 8; i++) { b[i] = v & 0xFF; v >>= 8; } }

static ssize_t read_full(int fd, void *buf, size_t n, uint64_t off) {
    uint8_t *b = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, b + got, n - got, (off_t)(off + got));
        if (r < 0) return -1;
        if (r == 0) break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}
static int write_full(int fd, const void *buf, size_t n, uint64_t off) {
    const uint8_t *b = buf;
    size_t put = 0;
    while (put < n) {
        ssize_t w = pwrite(fd, b + put, n - put, (off_t)(off + put));
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}

/* ---------- dirty set ---------- */
static int dirty_index(struct pager *p, uint32_t pgno) {
    for (size_t i = 0; i < p->ndirty; i++)
        if (p->dirty[i].pgno == pgno) return (int)i;
    return -1;
}
static uint8_t *dirty_add(struct pager *p, uint32_t pgno) {
    if (p->ndirty == Q_TXN_MAX_PAGES) return NULL;
    if (p->ndirty == p->dirty_cap) {
        size_t nc = p->dirty_cap ? p->dirty_cap * 2 : 64;
        struct dirty_page *nd = realloc(p->dirty, nc * sizeof *nd);
        if (!nd) return NULL;
        p->dirty = nd; p->dirty_cap = nc;
    }
    struct dirty_page *d = &p->dirty[p->ndirty++];
    d->pgno = pgno;
    memset(d->bytes, 0, Q_PAGE_SIZE);
    return d->bytes;
}

/* ---------- wal map ---------- */
static uint64_t wal_hash(uint32_t k) { return (uint64_t)k * 2654435761u; }

static int walmap_find(struct pager *p, uint32_t pgno, uint64_t *off) {
    if (!p->wm.cap) return 0;
    size_t m = p->wm.cap - 1;
    size_t i = (size_t)(wal_hash(pgno + 1) & m);
    while (p->wm.keys[i]) {
        if (p->wm.keys[i] == pgno + 1) { *off = p->wm.offs[i]; return 1; }
        i = (i + 1) & m;
    }
    return 0;
}
static int walmap_grow(struct pager *p, size_t ncap) {
    uint32_t *nk = calloc(ncap, sizeof(uint32_t));
    uint64_t *no = malloc(ncap * sizeof(uint64_t));
    if (!nk || !no) { free(nk); free(no); return set_err(Q_ERR_NOMEM, "out of memory"); }
    size_t m = ncap - 1;
    for (size_t i = 0; i < p->wm.cap; i++) {
        if (!p->wm.keys[i]) continue;
        size_t j = (size_t)(wal_hash(p->wm.keys[i]) & m);
        while (nk[j]) j = (j + 1) & m;
        nk[j] = p->wm.keys[i];
        no[j] = p->wm.offs[i];
    }
    free(p->wm.keys); free(p->wm.offs);
    p->wm.keys = nk; p->wm.offs = no; p->wm.cap = ncap;
    return Q_OK;
}
static int walmap_put(struct pager *p, uint32_t pgno, uint64_t off) {
    if (p->walmap_fail_in && --p->walmap_fail_in == 0) {
        p->walmap_fail_in = p->walmap_fail_period; /* fail periodically */
        return set_err(Q_ERR_NOMEM, "injected WAL map failure");
    }
    if ((p->wm.n + 1) * 10 >= p->wm.cap * 7) {   /* keep load < 0.7 */
        if (walmap_grow(p, p->wm.cap ? p->wm.cap * 2 : 1024) != Q_OK) return Q_ERR_NOMEM;
    }
    size_t m = p->wm.cap - 1;
    size_t i = (size_t)(wal_hash(pgno + 1) & m);
    while (p->wm.keys[i]) {
        if (p->wm.keys[i] == pgno + 1) { p->wm.offs[i] = off; return Q_OK; }
        i = (i + 1) & m;
    }
    p->wm.keys[i] = pgno + 1;
    p->wm.offs[i] = off;
    p->wm.n++;
    return Q_OK;
}

/* ---------- open / recover ---------- */
static int wal_recover(struct pager *p, int truncate_tail) {
    uint8_t hdr[16];
    ssize_t hr = read_full(p->wal_fd, hdr, sizeof hdr, 0);
    if (hr < 0) return set_err(Q_ERR_IO, "WAL header read failed");
    if (hr > 0 && hr < (ssize_t)sizeof hdr)
        return set_err(Q_ERR_CORRUPT, "WAL header is torn - not truncating it away");
    if (hr > 0 && get32(hdr) != WAL_MAGIC)
        /* a non-empty WAL we do not recognize may hold committed data
           (older format, torn header). Never erase it: fail the open. */
        return set_err(Q_ERR_CORRUPT, "WAL has an unknown format - not truncating it away");
    if (hr < (ssize_t)sizeof hdr) {
        /* a truly empty WAL is initialized; a short one already failed
           above with a corrupt error */
        uint8_t wh[16] = {0};
        put32(wh, WAL_MAGIC);
        if (write_full(p->wal_fd, wh, sizeof wh, 0) < 0)
            return set_err(Q_ERR_IO, "WAL header write failed");
        return Q_OK;
    }

    uint64_t off = 16;
    uint64_t last_commit_end = 16;
    for (;;) {
        uint8_t fh[FRAME_HDR];
        ssize_t r = read_full(p->wal_fd, fh, sizeof fh, off);
        if (r < 0) return set_err(Q_ERR_IO, "WAL frame read failed");
        if (r != (ssize_t)sizeof fh) break;               /* torn tail */
        if (get32(fh) != FRAME_MAGIC) break;              /* corrupt frame: stop */
        uint32_t commit_pages = get32(fh + 8);
        uint32_t txn_frames = get32(fh + 12);
        uint32_t crc = get32(fh + 16);
        uint8_t page[Q_PAGE_SIZE];
        ssize_t pr = read_full(p->wal_fd, page, Q_PAGE_SIZE, off + FRAME_HDR);
        if (pr < 0) return set_err(Q_ERR_IO, "WAL frame read failed");
        if (pr != (ssize_t)Q_PAGE_SIZE) break;
        if (crc32_frame(fh, page) != crc) break;          /* corrupt header or payload: stop */

        if (commit_pages) {
            /* Fold exactly this transaction's frames: the txn_frames frames
               ending at the commit frame. Frames of a dead transaction
               sitting between commits are never folded into a live one. */
            uint64_t span = (uint64_t)txn_frames * (FRAME_HDR + Q_PAGE_SIZE);
            if (txn_frames < 1 || span > off + FRAME_HDR + Q_PAGE_SIZE - 16) break;
            uint64_t f = off - (uint64_t)(txn_frames - 1) * (FRAME_HDR + Q_PAGE_SIZE);
            int bad = 0;
            for (uint32_t k = 0; k < txn_frames; k++) {
                uint8_t h2[FRAME_HDR];
                ssize_t h2r = read_full(p->wal_fd, h2, sizeof h2, f);
                if (h2r < 0) return set_err(Q_ERR_IO, "WAL frame read failed");
                if (h2r != (ssize_t)sizeof h2 ||
                    get32(h2) != FRAME_MAGIC) { bad = 1; break; }
                uint32_t pg2 = get32(h2 + 4);
                if (pg2 >= commit_pages) { bad = 1; break; }  /* page past the committed size */
                if (walmap_put(p, pg2, f) != Q_OK)
                    return set_err(Q_ERR_NOMEM, "out of memory");
                f += FRAME_HDR + Q_PAGE_SIZE;
            }
            if (bad) break;  /* corrupt commit run: drop it and the tail */
            p->page_count = commit_pages;
            last_commit_end = off + FRAME_HDR + Q_PAGE_SIZE;
        }
        off += FRAME_HDR + Q_PAGE_SIZE;
    }
    /* Anything past the last valid commit boundary - a torn tail, garbage
       appended while the file was offline, or frames of a dead transaction
       - must not survive open: future commits would append behind it and
       recovery would never reach them. This also removes a partial tail a
       failed commit could not truncate away itself. */
    if (truncate_tail && ftruncate(p->wal_fd, (off_t)last_commit_end) != 0)
        return set_err(Q_ERR_IO, "WAL tail truncate failed");
    return Q_OK;
}

static void fsync_dir(const char *path) {
    /* make a newly created file's directory entry durable */
    char *dup = strdup(path);
    if (!dup) return;
    char *slash = strrchr(dup, '/');
    if (slash) { *slash = 0; if (!dup[0]) strcpy(dup, "/"); }
    else strcpy(dup, ".");
    int dfd = open(dup, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    free(dup);
}

int pager_open(struct pager **out, const char *path) {
    int rc = Q_OK;
    struct pager *p = calloc(1, sizeof *p);
    if (!p) return set_err(Q_ERR_NOMEM, "out of memory");
    p->fd = p->wal_fd = -1;
    p->path = strdup(path);
    size_t n = strlen(path);
    p->wal_path = malloc(n + 5);
    if (!p->path || !p->wal_path) { rc = set_err(Q_ERR_NOMEM, "out of memory"); goto fail; }
    memcpy(p->wal_path, path, n);
    memcpy(p->wal_path + n, ".wal", 5);

    p->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (p->fd < 0) { rc = set_err(Q_ERR_IO, "cannot open database file"); goto fail; }
    p->wal_fd = open(p->wal_path, O_RDWR | O_CREAT, 0644);
    if (p->wal_fd < 0) { rc = set_err(Q_ERR_IO, "cannot open WAL file"); goto fail; }
#ifndef __EMSCRIPTEN__
    /* single-writer: the lock is per open-file-description, so it is
       released by the close() in pager_close/pager_abandon even on a
       crash. A second process gets an honest error instead of silently
       losing acknowledged commits. */
    if (flock(p->fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK)
            rc = set_err(Q_ERR_IO, "database is locked by another process");
        else {
            /* e.g. ENOLCK on a network filesystem: say what really failed */
            char msg[128];
            snprintf(msg, sizeof msg, "cannot lock database file: %s", strerror(errno));
            rc = set_err(Q_ERR_IO, msg);
        }
        goto fail;
    }
#endif
    fsync_dir(path);

    uint8_t hdr[Q_PAGE_SIZE] = {0};
    off_t sz = lseek(p->fd, 0, SEEK_END);
    int replayed = 0;
    if (sz == 0) {
        off_t wsz = lseek(p->wal_fd, 0, SEEK_END);
        uint8_t whdr[16] = {0};
        ssize_t hr = wsz == 16 ? read_full(p->wal_fd, whdr, sizeof whdr, 0) : 0;
        if (wsz == 0 || (hr == (ssize_t)sizeof whdr && get32(whdr) == WAL_MAGIC)) {
            /* fresh or cleanly checkpointed WAL: no frames to lose */
            put32(hdr + H_MAGIC, DB_MAGIC);
            put32(hdr + H_PAGES, 1);
            put32(hdr + H_CATROOT, 0);
            put64le(hdr + H_CATSEQ, 1);
            if (write_full(p->fd, hdr, Q_PAGE_SIZE, 0) < 0) {
                rc = set_err(Q_ERR_IO, "cannot initialize database file"); goto fail;
            }
            if (fsync(p->fd) != 0) {
                rc = set_err(Q_ERR_IO, "cannot sync new database file"); goto fail;
            }
            /* fresh file, nothing recoverable in the WAL: start it over */
            if (ftruncate(p->wal_fd, 0) != 0) {
                rc = set_err(Q_ERR_IO, "WAL truncate failed"); goto fail;
            }
            uint8_t wh[16] = {0};
            put32(wh, WAL_MAGIC);
            if (write_full(p->wal_fd, wh, sizeof wh, 0) < 0) {
                rc = set_err(Q_ERR_IO, "WAL header write failed"); goto fail;
            }
        } else {
            /* The main file is empty but the WAL holds bytes that are not a
               bare valid header: the file was lost or truncated outside
               quarry. Never discard that WAL unseen - it may hold the only
               copy of committed data. Recover the committed frames WITHOUT
               touching the WAL, so a refusal below leaves both files exactly
               as found; a replay works only when the WAL also carries the
               catalog page (page 0), otherwise refuse for manual recovery. */
            p->page_count = 1;
            int rrc = wal_recover(p, 0);
            if (rrc != Q_OK) { rc = rrc; goto fail; }
            uint64_t woff;
            if (p->wm.n == 0) {
                rc = set_err(Q_ERR_CORRUPT,
                    "database file is empty and the WAL holds no recoverable committed data - refusing to destroy it; restore a backup or remove the .wal file to start over");
                goto fail;
            }
            if (!walmap_find(p, 0, &woff)) {
                rc = set_err(Q_ERR_CORRUPT,
                    "database file is empty but the WAL holds committed data without its catalog page - refusing to destroy it; restore a backup or remove the .wal file to start over");
                goto fail;
            }
            put32(hdr + H_MAGIC, DB_MAGIC);
            put32(hdr + H_PAGES, 1);
            put32(hdr + H_CATROOT, 0);
            put64le(hdr + H_CATSEQ, 1);
            if (write_full(p->fd, hdr, Q_PAGE_SIZE, 0) < 0) {
                rc = set_err(Q_ERR_IO, "cannot initialize database file"); goto fail;
            }
            /* the checkpoint folds the real page 0 from the WAL over these
               placeholder H_* fields */
            int crc = pager_checkpoint(p);
            if (crc != Q_OK) { rc = crc; goto fail; }
            p->pending_count = p->page_count;
            replayed = 1;
        }
    } else if (sz < Q_PAGE_SIZE) {
        rc = set_err(Q_ERR_CORRUPT, "not a quarry database (bad header)"); goto fail;
    } else {
        ssize_t rr = read_full(p->fd, hdr, Q_PAGE_SIZE, 0);
        if (rr < 0) { rc = set_err(Q_ERR_IO, "database header read failed"); goto fail; }
        if (rr != Q_PAGE_SIZE || get32(hdr + H_MAGIC) != DB_MAGIC) {
            rc = set_err(Q_ERR_CORRUPT, "not a quarry database (bad header)"); goto fail;
        }
    }
    if (!replayed) {
        p->page_count = get32(hdr + H_PAGES);
        p->pending_count = p->page_count;
        int rrc = wal_recover(p, 1);
        if (rrc != Q_OK) { rc = rrc; goto fail; }  /* keep IO/NOMEM distinct from CORRUPT */

        p->pending_count = p->page_count;
    }
    /* test hook: fail the Nth walmap_put once (fault injection for the
       commit-time map-rebuild path) */
    const char *inj = getenv("QUARRY_WALMAP_FAIL_AFTER");
    if (inj) {
        p->walmap_fail_in = (size_t)strtoull(inj, NULL, 10);
        p->walmap_fail_period = p->walmap_fail_in;
    }
    *out = p;
    return Q_OK;
fail:
    if (p->fd >= 0) close(p->fd);
    if (p->wal_fd >= 0) close(p->wal_fd);
    free(p->wm.keys); free(p->wm.offs);  /* wal_recover may have run before the failure */
    free(p->path); free(p->wal_path); free(p);
    return rc;
}

/* ---------- transactions ---------- */
bool pager_in_txn(struct pager *p) { return p->in_txn; }

int pager_begin(struct pager *p) {
    if (p->broken) return set_err(Q_ERR_IO, "pager unusable after a WAL map failure - reopen the database");
    if (p->in_txn) return set_err(Q_ERR_TXN, "already inside a transaction");
    p->in_txn = true;
    p->ndirty = 0;
    p->pending_count = p->page_count;
    p->sp_active = false;
    p->nundo = 0;
    return Q_OK;
}

int pager_commit(struct pager *p) {
    if (p->broken) return set_err(Q_ERR_IO, "pager unusable after a WAL map failure - reopen the database");
    if (!p->in_txn) return set_err(Q_ERR_TXN, "no transaction to commit");
    if (lseek(p->wal_fd, 0, SEEK_END) < 0) return set_err(Q_ERR_IO, "WAL seek failed");
    uint64_t off = (uint64_t)lseek(p->wal_fd, 0, SEEK_CUR);
    if (off < 16) {
        /* the WAL header is missing or torn (e.g. a checkpoint whose header
           rewrite failed left the file truncated): rewrite it before
           staging frames, or recovery would read garbage at offset 0 and
           refuse the whole WAL, taking these acknowledged commits with it */
        uint8_t wh[16] = {0};
        put32(wh, WAL_MAGIC);
        if (write_full(p->wal_fd, wh, sizeof wh, 0) < 0) {
            if (ftruncate(p->wal_fd, (off_t)off) != 0) {
                p->broken = true;
                return set_err(Q_ERR_IO, "WAL header write failed and the partial header could not be removed - reopen the database");
            }
            return set_err(Q_ERR_IO, "WAL header write failed");
        }
        off = 16;
    }
    uint64_t start = off;
    /* Stage every frame first. The walmap and page_count move only after
       the whole batch is written and fsynced, so a failed commit (disk
       full, quota, EIO) leaves no committed-looking frames behind: the
       partial tail is truncated away and ROLLBACK still just drops the
       dirty set. */
    for (size_t i = 0; i < p->ndirty; i++) {
        uint8_t fh[FRAME_HDR] = {0};
        uint32_t last = (i + 1 == p->ndirty) ? p->pending_count : 0;
        put32(fh, FRAME_MAGIC);
        put32(fh + 4, p->dirty[i].pgno);
        put32(fh + 8, last);
        put32(fh + 12, last ? (uint32_t)p->ndirty : 0);  /* txn_frames on the commit frame */
        put32(fh + 16, crc32_frame(fh, p->dirty[i].bytes));
        if (write_full(p->wal_fd, fh, sizeof fh, off) < 0 ||
            write_full(p->wal_fd, p->dirty[i].bytes, Q_PAGE_SIZE, off + FRAME_HDR) < 0) {
            if (ftruncate(p->wal_fd, (off_t)start) != 0) {
                /* partial frames linger and recovery will not fold past
                   them, so later commits would report success but never
                   become durable. Refuse all further work instead. */
                p->broken = true;
                return set_err(Q_ERR_IO, "WAL write failed and the partial tail could not be removed - reopen the database");
            }
            return set_err(Q_ERR_IO, "WAL write failed");
        }
        off += FRAME_HDR + Q_PAGE_SIZE;
    }
    if (fsync(p->wal_fd) < 0) {
        if (ftruncate(p->wal_fd, (off_t)start) != 0) {
            p->broken = true;
            return set_err(Q_ERR_IO, "WAL fsync failed and the partial tail could not be removed - reopen the database");
        }
        return set_err(Q_ERR_IO, "WAL fsync failed");
    }
    off = start;
    for (size_t i = 0; i < p->ndirty; i++) {
        if (walmap_put(p, p->dirty[i].pgno, off) != Q_OK) {
            /* The frames are durable; the in-memory map is partial. Never
               checkpoint from a partial map - unmapped pages would be read
               back from the stale main file and the WAL truncated on top,
               destroying committed data while reporting success. Rebuild
               the whole map from the WAL instead. If even that fails, keep
               the WAL intact and mark the pager unusable; the next open
               recovers everything. */
            free(p->wm.keys); free(p->wm.offs);
            p->wm.keys = NULL; p->wm.offs = NULL; p->wm.cap = p->wm.n = 0;
            int rrc = wal_recover(p, 1);
            p->ndirty = 0;
            p->in_txn = false;
            p->sp_active = false;
            p->nundo = 0;
            if (rrc != Q_OK) {
                p->broken = true;
                return set_err(Q_ERR_NOMEM,
                    "WAL map rebuild failed; committed data is safe on disk - reopen the database");
            }
            return Q_OK;
        }
        off += FRAME_HDR + Q_PAGE_SIZE;
        p->frames_since_ckpt++;
    }
    p->page_count = p->pending_count;
    p->ndirty = 0;
    p->in_txn = false;
    p->sp_active = false;
    p->nundo = 0;
    /* auto-checkpoint is best-effort: the commit is already durable, so a
       checkpoint failure must not report the transaction as failed */
    if (p->frames_since_ckpt >= 2000) pager_checkpoint(p);
    return Q_OK;
}

void pager_sp_mark(struct pager *p, struct pager_savepoint *sp) {
    sp->ndirty = p->ndirty;
    sp->pending = p->pending_count;
    p->sp_active = true;
    p->sp_ndirty = p->ndirty;
    p->nundo = 0;
}

/* remember the pre-statement image of a page that was already dirty at the
   mark and is about to be mutated again */
static int undo_save(struct pager *p, int di) {
    uint32_t pgno = p->dirty[di].pgno;
    for (size_t i = 0; i < p->nundo; i++)
        if (p->undo_pgno[i] == pgno) return Q_OK;
    if (p->nundo == p->undo_cap) {
        size_t nc = p->undo_cap ? p->undo_cap * 2 : 32;
        uint32_t *np = realloc(p->undo_pgno, nc * sizeof *np);
        if (!np) return set_err(Q_ERR_NOMEM, "out of memory");
        p->undo_pgno = np;
        uint8_t (*ni)[Q_PAGE_SIZE] = realloc(p->undo_img, nc * sizeof *ni);
        if (!ni) return set_err(Q_ERR_NOMEM, "out of memory");
        p->undo_img = ni;
        p->undo_cap = nc;
    }
    p->undo_pgno[p->nundo] = pgno;
    memcpy(p->undo_img[p->nundo], p->dirty[di].bytes, Q_PAGE_SIZE);
    p->nundo++;
    return Q_OK;
}

void pager_sp_restore(struct pager *p, const struct pager_savepoint *sp) {
    /* first undo in-place edits to pages that were already dirty at the
       mark, then drop every page the statement dirtied afterwards */
    for (size_t i = 0; i < p->nundo; i++) {
        int di = dirty_index(p, p->undo_pgno[i]);
        if (di >= 0 && (size_t)di < sp->ndirty)
            memcpy(p->dirty[di].bytes, p->undo_img[i], Q_PAGE_SIZE);
    }
    p->ndirty = sp->ndirty;
    p->pending_count = sp->pending;
    p->sp_active = false;
    p->nundo = 0;
}

int pager_rollback(struct pager *p) {
    if (!p->in_txn) return set_err(Q_ERR_TXN, "no transaction to roll back");
    p->ndirty = 0;
    p->pending_count = p->page_count;
    p->in_txn = false;
    p->sp_active = false;
    p->nundo = 0;
    return Q_OK;
}

/* ---------- page access ---------- */
int pager_get(struct pager *p, uint32_t pgno, const uint8_t **page) {
    if (p->broken) return set_err(Q_ERR_IO, "pager unusable after a WAL map failure - reopen the database");
    if (pgno >= p->pending_count)
        return set_err(Q_ERR_CORRUPT, "page out of range");
    int di = dirty_index(p, pgno);
    if (di >= 0) { *page = p->dirty[di].bytes; return Q_OK; }

    uint8_t *buf = p->rbuf[p->rnext];
    p->rnext = (p->rnext + 1) % 32;
    uint64_t woff;
    if (walmap_find(p, pgno, &woff)) {
        if (read_full(p->wal_fd, buf, Q_PAGE_SIZE, woff + FRAME_HDR) != Q_PAGE_SIZE)
            return set_err(Q_ERR_IO, "WAL read failed");
    } else {
        if (read_full(p->fd, buf, Q_PAGE_SIZE, (uint64_t)pgno * Q_PAGE_SIZE) != Q_PAGE_SIZE)
            return set_err(Q_ERR_IO, "page read failed");
    }
    *page = buf;
    return Q_OK;
}

int pager_get_mut(struct pager *p, uint32_t pgno, uint8_t **page) {
    if (!p->in_txn) return set_err(Q_ERR_TXN, "write outside a transaction");
    int di = dirty_index(p, pgno);
    if (di < 0) {
        const uint8_t *src;
        int rc = pager_get(p, pgno, &src);
        if (rc != Q_OK) return rc;
        uint8_t *dst = dirty_add(p, pgno);
        if (!dst) return set_err(Q_ERR_TOO_BIG,
            "transaction touched too many pages (limit 2048); split it up");
        memcpy(dst, src, Q_PAGE_SIZE);
        di = (int)p->ndirty - 1;
    }
    /* a page dirtied before the active savepoint must keep its pre-statement
       image before it is mutated in place, or sp_restore cannot undo it */
    if (p->sp_active && (size_t)di < p->sp_ndirty) {
        int rc = undo_save(p, di);
        if (rc != Q_OK) return rc;
    }
    *page = p->dirty[di].bytes;
    return Q_OK;
}

int pager_alloc(struct pager *p, uint32_t *pgno, uint8_t **page) {
    if (p->broken) return set_err(Q_ERR_IO, "pager unusable after a WAL map failure - reopen the database");
    if (!p->in_txn) return set_err(Q_ERR_TXN, "allocate outside a transaction");
    if (p->ndirty >= Q_TXN_MAX_PAGES)
        return set_err(Q_ERR_TOO_BIG, "transaction touched too many pages (limit 2048)");
    uint32_t n = p->pending_count++;
    uint8_t *b = dirty_add(p, n);
    if (!b) return set_err(Q_ERR_NOMEM, "out of memory");
    *pgno = n;
    *page = b;
    return Q_OK;
}

/* ---------- checkpoint / close ---------- */
int pager_checkpoint(struct pager *p) {
    if (p->broken) return set_err(Q_ERR_IO, "pager unusable after a WAL map failure - reopen the database");
    if (p->in_txn) return set_err(Q_ERR_TXN, "cannot checkpoint inside a transaction");
    for (size_t i = 0; i < p->wm.cap; i++) {
        if (!p->wm.keys[i]) continue;
        uint32_t pgno = p->wm.keys[i] - 1;
        uint8_t page[Q_PAGE_SIZE];
        if (read_full(p->wal_fd, page, Q_PAGE_SIZE, p->wm.offs[i] + FRAME_HDR) != Q_PAGE_SIZE)
            return set_err(Q_ERR_IO, "WAL read during checkpoint failed");
        if (write_full(p->fd, page, Q_PAGE_SIZE, (uint64_t)pgno * Q_PAGE_SIZE) < 0)
            return set_err(Q_ERR_IO, "checkpoint write failed");
    }
    uint8_t hdr[Q_PAGE_SIZE] = {0};
    if (read_full(p->fd, hdr, Q_PAGE_SIZE, 0) != Q_PAGE_SIZE)
        return set_err(Q_ERR_IO, "header read failed");
    put32(hdr + H_PAGES, p->page_count);
    if (write_full(p->fd, hdr, Q_PAGE_SIZE, 0) < 0)
        return set_err(Q_ERR_IO, "header write failed");
    /* data must be durable in the main file before the WAL is truncated;
       on failure the WAL is untouched and recovery replays it */
    if (fsync(p->fd) != 0)
        return set_err(Q_ERR_IO, "checkpoint fsync failed - WAL left intact");
    if (ftruncate(p->wal_fd, 0) != 0) return set_err(Q_ERR_IO, "WAL truncate failed");
    /* Past the truncate the pages are durable in the main file, so the
       checkpoint has logically happened even if the WAL housekeeping below
       fails. Drop the in-memory map now: leaving stale frame offsets would
       send later reads chasing a WAL that no longer exists. A missing or
       torn WAL header is fine - the next open reinitializes an empty WAL
       or refuses a torn one, and no data depends on it either way. */
    free(p->wm.keys); free(p->wm.offs);
    memset(&p->wm, 0, sizeof p->wm);
    p->frames_since_ckpt = 0;
    uint8_t wh[16] = {0};
    put32(wh, WAL_MAGIC);
    if (write_full(p->wal_fd, wh, sizeof wh, 0) < 0)
        return set_err(Q_ERR_IO, "WAL header rewrite failed after checkpoint");
    if (fsync(p->wal_fd) != 0)
        return set_err(Q_ERR_IO, "WAL fsync failed after checkpoint");
    return Q_OK;
}

int pager_close(struct pager *p) {
    if (p->in_txn) pager_rollback(p);
    /* a broken pager keeps its WAL for the next open; checkpointing from
       its partial map would destroy committed data */
    int rc = p->broken ? Q_OK : pager_checkpoint(p);
    close(p->fd);
    close(p->wal_fd);
    free(p->path); free(p->wal_path);
    free(p->dirty); free(p->wm.keys); free(p->wm.offs);
    free(p->undo_pgno); free(p->undo_img);
    free(p);
    return rc;
}

void pager_abandon(struct pager *p) {
    /* crash simulation: WAL holds every committed page (each commit fsynced),
       uncommitted dirty pages die here. No checkpoint, no rollback. */
    close(p->fd);
    close(p->wal_fd);
    free(p->path); free(p->wal_path);
    free(p->dirty); free(p->wm.keys); free(p->wm.offs);
    free(p->undo_pgno); free(p->undo_img);
    free(p);
}

uint32_t pager_page_count(struct pager *p) { return p->page_count; }
uint32_t pager_pending_count(struct pager *p) { return p->pending_count; }

int pager_get_catalog(struct pager *p, uint32_t *root, uint64_t *seq) {
    const uint8_t *hdr;
    int rc = pager_get(p, 0, &hdr);
    if (rc != Q_OK) return rc;
    *root = get32(hdr + H_CATROOT);
    *seq = get64le(hdr + H_CATSEQ);
    return Q_OK;
}

int pager_set_catalog(struct pager *p, uint32_t root, uint64_t seq) {
    uint8_t *hdr;
    int rc = pager_get_mut(p, 0, &hdr);
    if (rc != Q_OK) return rc;
    put32(hdr + H_CATROOT, root);
    put64le(hdr + H_CATSEQ, seq);
    return Q_OK;
}
