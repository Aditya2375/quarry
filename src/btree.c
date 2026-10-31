/* B+tree over the pager: signed int64 keys, variable-length row blobs in
 * the leaves, sibling links for ordered scans.
 *
 * Page layout (all offsets bytes, little-endian):
 *   common header (16 B): type u8 (1 leaf / 2 interior), ncells u16,
 *                         used u32 (bytes of cell area in use), link u32
 *                         leaf -> right sibling; interior -> rightmost child
 *   leaf cell:     key i64 | len u16 | payload[len]
 *   interior cell: key i64 | child u32   (child holds keys < cell key)
 *
 * Splits propagate a separator key upward; a split root grows the tree one
 * level. Deletes compact the leaf but never merge or rebalance - the tree
 * stays correct and ordered, a page can just stay sparse (see README).
 */
#include "quarry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PT_LEAF 1
#define PT_INTERIOR 2

#define PH_TYPE   0
#define PH_NCELLS 1  /* u16 */
#define PH_USED   3  /* u32 */
#define PH_LINK   7  /* u32 */
#define PH_SIZE   16

#define LEAF_CELL_HDR 10  /* key 8 + len 2 */
#define INT_CELL_SIZE 12  /* key 8 + child 4 */

static void p16(uint8_t *b, uint16_t v) { b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; }
static uint16_t g16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static void p32(uint8_t *b, uint32_t v) {
    b[0]=v&0xFF; b[1]=(v>>8)&0xFF; b[2]=(v>>16)&0xFF; b[3]=(v>>24)&0xFF;
}
static uint32_t g32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
}
static void p64(uint8_t *b, int64_t v) { uint64_t u=(uint64_t)v; for (int i=0;i<8;i++){b[i]=u&0xFF;u>>=8;} }
static int64_t g64(const uint8_t *b) {
    uint64_t u=0; for (int i=7;i>=0;i--){u=(u<<8)|b[i];} return (int64_t)u;
}

static int oom(void) { snprintf(q_err, sizeof q_err, "out of memory"); return Q_ERR_NOMEM; }

/* ---------- corruption guards ---------- */
#define MAX_DEPTH 64   /* a real tree over 32-bit pages is far shallower */

static int corrupt(const char *what) {
    snprintf(q_err, sizeof q_err, "corrupt database: %s", what);
    return Q_ERR_CORRUPT;
}

/* header sanity + cell-area bounds, checked on every page read so a
   corrupted file errors out instead of walking out of bounds or looping */
static int page_check(const uint8_t *pg) {
    if (pg[PH_TYPE] != PT_LEAF && pg[PH_TYPE] != PT_INTERIOR)
        return corrupt("unknown page type");
    uint32_t n = g16(pg + PH_NCELLS);
    if (pg[PH_TYPE] == PT_INTERIOR) {
        if (n > (Q_PAGE_SIZE - PH_SIZE) / INT_CELL_SIZE)
            return corrupt("interior cell count out of range");
        int64_t prev = 0;
        for (uint32_t i = 0; i < n; i++) {
            int64_t k = g64(pg + PH_SIZE + (uint32_t)i * INT_CELL_SIZE);
            if (i > 0 && k <= prev)
                return corrupt("interior keys out of order");
            prev = k;
        }
    } else {
        uint32_t off = PH_SIZE;
        int64_t prev = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (off + LEAF_CELL_HDR > Q_PAGE_SIZE)
                return corrupt("leaf cell header out of bounds");
            int64_t k = g64(pg + off);
            if (i > 0 && k <= prev)
                return corrupt("leaf keys out of order");
            prev = k;
            off += LEAF_CELL_HDR + g16(pg + off + 8);
            if (off > Q_PAGE_SIZE)
                return corrupt("leaf cell payload out of bounds");
        }
        /* PH_USED must equal the packed cell bytes: insert/delete arithmetic
           trusts it, and a stale value memmoves past the page */
        if (g32(pg + PH_USED) != off - PH_SIZE)
            return corrupt("leaf used-bytes field inconsistent with cells");
    }
    return Q_OK;
}

static int get_checked(struct pager *p, uint32_t pgno, const uint8_t **pg) {
    int rc = pager_get(p, pgno, pg);
    if (rc != Q_OK) return rc;
    return page_check(*pg);
}
static int get_mut_checked(struct pager *p, uint32_t pgno, uint8_t **pg) {
    int rc = pager_get_mut(p, pgno, pg);
    if (rc != Q_OK) return rc;
    return page_check(*pg);
}

/* ---------- leaf helpers ---------- */
/* byte offset of cell i by walking the packed area */
static uint32_t leaf_cell_off(const uint8_t *pg, int i) {
    uint32_t off = PH_SIZE;
    for (int k = 0; k < i; k++) off += LEAF_CELL_HDR + g16(pg + off + 8);
    return off;
}
/* index of the first cell with key >= target; returns ncells if none */
static int leaf_lower_bound(const uint8_t *pg, int64_t key) {
    int n = g16(pg + PH_NCELLS);
    uint32_t off = PH_SIZE;
    for (int i = 0; i < n; i++) {
        int64_t k = g64(pg + off);
        if (k >= key) return i;
        off += LEAF_CELL_HDR + g16(pg + off + 8);
    }
    return n;
}

/* ---------- interior helpers ---------- */
static uint32_t int_cell_off(int i) { return PH_SIZE + (uint32_t)i * INT_CELL_SIZE; }
/* child to descend into for key */
static uint32_t int_child_for(const uint8_t *pg, int64_t key) {
    int n = g16(pg + PH_NCELLS);
    for (int i = 0; i < n; i++) {
        if (key < g64(pg + int_cell_off(i)))
            return g32(pg + int_cell_off(i) + 8);
    }
    return g32(pg + PH_LINK);
}
static int int_insert_pos(const uint8_t *pg, int64_t key) {
    int n = g16(pg + PH_NCELLS);
    for (int i = 0; i < n; i++)
        if (key < g64(pg + int_cell_off(i))) return i;
    return n;
}
/* insert cell (key, child) at sorted position */
static void int_insert_cell_at(uint8_t *pg, int pos, int64_t key, uint32_t child) {
    int n = g16(pg + PH_NCELLS);
    uint32_t at = int_cell_off(pos);
    memmove(pg + at + INT_CELL_SIZE, pg + at, (size_t)(n - pos) * INT_CELL_SIZE);
    p64(pg + at, key);
    p32(pg + at + 8, child);
    p16(pg + PH_NCELLS, (uint16_t)(n + 1));
}

/* ---------- public API ---------- */
int cursor_first(struct pager *p, uint32_t root, struct cursor *c);
int cursor_next(struct cursor *c);
int cursor_get(struct cursor *c, int64_t *key, uint8_t *buf, uint32_t *len_io);

int btree_create(struct pager *p, uint32_t *root_out) {
    uint32_t pgno; uint8_t *pg;
    int rc = pager_alloc(p, &pgno, &pg);
    if (rc != Q_OK) return rc;
    memset(pg, 0, Q_PAGE_SIZE);
    pg[PH_TYPE] = PT_LEAF;
    *root_out = pgno;
    return Q_OK;
}

static uint32_t find_leaf(struct pager *p, uint32_t root, int64_t key, int *rc) {
    uint32_t pgno = root;
    for (int depth = 0; depth < MAX_DEPTH; depth++) {
        const uint8_t *pg;
        *rc = get_checked(p, pgno, &pg);
        if (*rc != Q_OK) return 0;
        if (pg[PH_TYPE] == PT_LEAF) return pgno;
        uint32_t next = int_child_for(pg, key);
        if (next == pgno) { *rc = corrupt("interior page points at itself"); return 0; }
        pgno = next;
    }
    *rc = corrupt("tree deeper than 64 levels");
    return 0;
}

int btree_find(struct pager *p, uint32_t root, int64_t key, uint8_t *buf, uint32_t *len_io) {
    int rc;
    uint32_t leaf = find_leaf(p, root, key, &rc);
    if (rc != Q_OK) return rc;
    const uint8_t *pg;
    if ((rc = get_checked(p, leaf, &pg)) != Q_OK) return rc;
    int n = g16(pg + PH_NCELLS);
    int i = leaf_lower_bound(pg, key);
    if (i < n) {
        uint32_t off = leaf_cell_off(pg, i);
        if (g64(pg + off) == key) {
            uint16_t len = g16(pg + off + 8);
            if (buf) {
                if (*len_io < len) { snprintf(q_err, sizeof q_err, "row buffer too small"); return Q_ERR_TOO_BIG; }
                memcpy(buf, pg + off + LEAF_CELL_HDR, len);
            }
            *len_io = len;
            return Q_OK;
        }
    }
    snprintf(q_err, sizeof q_err, "key not found");
    return Q_ERR_SCHEMA;
}

int btree_max(struct pager *p, uint32_t root, int64_t *key_out) {
    uint32_t pgno = root;
    for (int depth = 0; depth < MAX_DEPTH; depth++) {
        const uint8_t *pg;
        int rc = get_checked(p, pgno, &pg);
        if (rc != Q_OK) return rc;
        if (pg[PH_TYPE] == PT_INTERIOR) {
            uint32_t next = g32(pg + PH_LINK);
            if (next == pgno) return corrupt("interior page points at itself");
            pgno = next;
            continue;
        }
        int n = g16(pg + PH_NCELLS);
        if (n > 0) {
            *key_out = g64(pg + leaf_cell_off(pg, n - 1));
            return Q_OK;
        }
        /* The rightmost leaf is empty (every row in it was deleted; leaves
           are never merged). There is no right sibling to follow, so scan
           the tree for the last remaining key instead of giving up. */
        struct cursor c;
        rc = cursor_first(p, root, &c);
        if (rc != Q_OK) return rc;
        if (c.done) { snprintf(q_err, sizeof q_err, "empty tree"); return Q_ERR_SCHEMA; }
        int64_t mx = 0;
        while (!c.done) {
            uint32_t l = 0;
            if ((rc = cursor_get(&c, &mx, NULL, &l)) != Q_OK) return rc;
            if ((rc = cursor_next(&c)) != Q_OK) return rc;
        }
        *key_out = mx;
        return Q_OK;
    }
    return corrupt("tree deeper than 64 levels");
}

struct promo { int valid; int64_t key; uint32_t child; };

static int insert_rec(struct pager *p, uint32_t pgno, int64_t key,
                      const uint8_t *data, uint32_t len, struct promo *pr,
                      int depth) {
    /* find_leaf/btree_max/cursor_first bound their descent at MAX_DEPTH;
       this recursion must too, or a cyclic interior page is a stack
       overflow instead of a corruption error */
    if (depth >= MAX_DEPTH) return corrupt("tree deeper than 64 levels");
    uint8_t *pg;
    int rc = get_mut_checked(p, pgno, &pg);
    if (rc != Q_OK) return rc;
    pr->valid = 0;

    if (pg[PH_TYPE] == PT_LEAF) {
        uint32_t need = LEAF_CELL_HDR + len;
        int n = g16(pg + PH_NCELLS);
        int pos = leaf_lower_bound(pg, key);
        if (pos < n) {
            uint32_t off = leaf_cell_off(pg, pos);
            if (g64(pg + off) == key) {
                snprintf(q_err, sizeof q_err, "duplicate key %lld", (long long)key);
                return Q_ERR_CONSTRAINT;
            }
        }
        if (len > Q_MAX_ROW_BYTES) {
            snprintf(q_err, sizeof q_err, "row too large (%u bytes, limit %d)",
                     len, Q_MAX_ROW_BYTES);
            return Q_ERR_TOO_BIG;
        }
        if (need > Q_PAGE_SIZE - PH_SIZE) {
            snprintf(q_err, sizeof q_err, "row too large (%u bytes, limit %d)",
                     len, Q_MAX_ROW_BYTES);
            return Q_ERR_TOO_BIG;
        }
        uint32_t used = g32(pg + PH_USED);
        if (used + need <= Q_PAGE_SIZE - PH_SIZE) {
            uint32_t at = leaf_cell_off(pg, pos);
            uint32_t tail_at = leaf_cell_off(pg, n);
            memmove(pg + at + need, pg + at, tail_at - at);
            p64(pg + at, key);
            p16(pg + at + 8, (uint16_t)len);
            memcpy(pg + at + LEAF_CELL_HDR, data, len);
            p16(pg + PH_NCELLS, (uint16_t)(n + 1));
            p32(pg + PH_USED, used + need);
            return Q_OK;
        }
        /* split: build the two leaves in scratch, then write back.
           Capture everything from `pg` BEFORE pager_alloc: allocating a
           page can realloc the dirty array and move `pg`. */
        uint32_t old_next = g32(pg + PH_LINK);
        uint8_t *all = malloc(used + need + LEAF_CELL_HDR + len);
        if (!all) return oom();
        /* materialize old cells + new cell in key order into `all` */
        uint32_t w = 0;
        uint32_t off = PH_SIZE;
        bool placed = false;
        for (int i = 0; i < n; i++) {
            uint16_t cl = g16(pg + off + 8);
            if (!placed && key < g64(pg + off)) {
                p64(all + w, key); p16(all + w + 8, (uint16_t)len);
                memcpy(all + w + LEAF_CELL_HDR, data, len); w += need;
                placed = true;
            }
            memcpy(all + w, pg + off, LEAF_CELL_HDR + cl);
            w += LEAF_CELL_HDR + cl;
            off += LEAF_CELL_HDR + cl;
        }
        if (!placed) {
            p64(all + w, key); p16(all + w + 8, (uint16_t)len);
            memcpy(all + w + LEAF_CELL_HDR, data, len); w += need;
        }
        /* choose split point: first cell that pushes the left half past half full */
        int total = n + 1;
        uint32_t half = w / 2, acc = 0, loff = 0;
        int split = 0;
        for (int i = 0; i < total; i++) {
            uint32_t cs = LEAF_CELL_HDR + g16(all + loff + 8);
            if (acc >= half) { split = i; break; }
            acc += cs; loff += cs;
            split = i + 1;
        }
        if (split == 0) split = 1;
        if (split == total) split = total - 1;
        uint32_t left_bytes = 0, lo = 0;
        for (int i = 0; i < split; i++) { left_bytes += LEAF_CELL_HDR + g16(all + lo + 8); lo += LEAF_CELL_HDR + g16(all + lo + 8); }
        int64_t sep_key = g64(all + left_bytes);

        uint32_t rpgno; uint8_t *rpg;
        rc = pager_alloc(p, &rpgno, &rpg);
        if (rc != Q_OK) { free(all); return rc; }
        /* right page */
        memset(rpg, 0, Q_PAGE_SIZE);
        rpg[PH_TYPE] = PT_LEAF;
        p16(rpg + PH_NCELLS, (uint16_t)(total - split));
        p32(rpg + PH_USED, w - left_bytes);
        memcpy(rpg + PH_SIZE, all + left_bytes, w - left_bytes);
        p32(rpg + PH_LINK, old_next);
        /* re-fetch the left page: pager_alloc may have moved the dirty array */
        rc = pager_get_mut(p, pgno, &pg);
        if (rc != Q_OK) { free(all); return rc; }
        memset(pg + PH_SIZE, 0, Q_PAGE_SIZE - PH_SIZE);
        pg[PH_TYPE] = PT_LEAF;
        p16(pg + PH_NCELLS, (uint16_t)split);
        p32(pg + PH_USED, left_bytes);
        memcpy(pg + PH_SIZE, all, left_bytes);
        p32(pg + PH_LINK, rpgno);
        free(all);
        pr->valid = 1;
        pr->key = sep_key;
        pr->child = rpgno;
        return Q_OK;
    }

    /* interior: descend, then absorb a promotion if the child split.
       The recursion can realloc the dirty array, so re-fetch our page. */
    uint32_t child = int_child_for(pg, key);
    if (child == pgno) return corrupt("interior page points at itself");
    struct promo sub;
    rc = insert_rec(p, child, key, data, len, &sub, depth + 1);
    if (rc != Q_OK || !sub.valid) return rc;
    rc = pager_get_mut(p, pgno, &pg);
    if (rc != Q_OK) return rc;

    int n = g16(pg + PH_NCELLS);
    if ((size_t)(n + 1) * INT_CELL_SIZE <= Q_PAGE_SIZE - PH_SIZE - 16) {
        /* child page split into [child] (keys < sep) and [sub.child] (keys
           >= sep): new cell is (sep, child); the cell that used to point
           past it (or the rightmost pointer) now leads to sub.child. */
        int pos = int_insert_pos(pg, sub.key);
        int_insert_cell_at(pg, pos, sub.key, child);
        if (pos == n) p32(pg + PH_LINK, sub.child);
        else p32(pg + int_cell_off(pos + 1) + 8, sub.child);
        return Q_OK;
    }
    /* interior split: middle key goes up */
    int mid = n / 2;
    int total = n + 1;
    /* materialize cells with the new one included */
    int64_t *keys = malloc(sizeof(int64_t) * total);
    uint32_t *kids = malloc(sizeof(uint32_t) * (total + 1));
    if (!keys || !kids) { free(keys); free(kids); return oom(); }
    int pos = int_insert_pos(pg, sub.key);
    int ti = 0;
    for (int i = 0; i < n; i++) {
        if (i == pos) ti++;
        keys[ti] = g64(pg + int_cell_off(i));
        kids[ti] = g32(pg + int_cell_off(i) + 8);
        ti++;
    }
    /* new cell: (sep, old child); the slot after it leads to the new page
       (overwrites the shifted child pointer, or the rightmost pointer) */
    kids[total] = g32(pg + PH_LINK);
    keys[pos] = sub.key;
    kids[pos] = child;
    kids[pos + 1] = sub.child;  /* == kids[total] when the rightmost split */

    uint32_t rpgno; uint8_t *rpg;
    rc = pager_alloc(p, &rpgno, &rpg);
    if (rc != Q_OK) { free(keys); free(kids); return rc; }
    memset(rpg, 0, Q_PAGE_SIZE);
    rpg[PH_TYPE] = PT_INTERIOR;
    int right_n = total - mid - 1;
    p16(rpg + PH_NCELLS, (uint16_t)right_n);
    for (int i = 0; i < right_n; i++) {
        p64(rpg + int_cell_off(i), keys[mid + 1 + i]);
        p32(rpg + int_cell_off(i) + 8, kids[mid + 1 + i]);
    }
    p32(rpg + PH_LINK, kids[total]);

    /* re-fetch: pager_alloc may have moved the dirty array */
    rc = pager_get_mut(p, pgno, &pg);
    if (rc != Q_OK) { free(keys); free(kids); return rc; }
    pg[PH_TYPE] = PT_INTERIOR;
    p16(pg + PH_NCELLS, (uint16_t)mid);
    for (int i = mid; i < n; i++) { /* clear tail (optional hygiene) */
        memset(pg + int_cell_off(i), 0, INT_CELL_SIZE);
    }
    p32(pg + PH_LINK, kids[mid]);

    pr->valid = 1;
    pr->key = keys[mid];
    pr->child = rpgno;
    free(keys); free(kids);
    return Q_OK;
}

int btree_insert(struct pager *p, uint32_t *root_io, int64_t key,
                 const uint8_t *data, uint32_t len) {
    struct promo pr;
    int rc = insert_rec(p, *root_io, key, data, len, &pr, 0);
    if (rc != Q_OK) return rc;
    if (pr.valid) {
        uint32_t nr; uint8_t *npg;
        rc = pager_alloc(p, &nr, &npg);
        if (rc != Q_OK) return rc;
        memset(npg, 0, Q_PAGE_SIZE);
        npg[PH_TYPE] = PT_INTERIOR;
        p16(npg + PH_NCELLS, 1);
        p64(npg + int_cell_off(0), pr.key);
        p32(npg + int_cell_off(0) + 8, *root_io);
        p32(npg + PH_LINK, pr.child);
        *root_io = nr;
    }
    return Q_OK;
}

int btree_delete(struct pager *p, uint32_t root, int64_t key) {
    int rc;
    uint32_t leaf = find_leaf(p, root, key, &rc);
    if (rc != Q_OK) return rc;
    uint8_t *pg;
    if ((rc = get_mut_checked(p, leaf, &pg)) != Q_OK) return rc;
    int n = g16(pg + PH_NCELLS);
    int i = leaf_lower_bound(pg, key);
    if (i >= n) return Q_OK;             /* absent: nothing to do */
    uint32_t off = leaf_cell_off(pg, i);
    if (g64(pg + off) != key) return Q_OK;
    uint32_t cs = LEAF_CELL_HDR + g16(pg + off + 8);
    uint32_t end = leaf_cell_off(pg, n);
    memmove(pg + off, pg + off + cs, end - (off + cs));
    p16(pg + PH_NCELLS, (uint16_t)(n - 1));
    p32(pg + PH_USED, g32(pg + PH_USED) - cs);
    return Q_OK;
}

/* ---------- ordered scan ---------- */
int cursor_first(struct pager *p, uint32_t root, struct cursor *c) {
    c->p = p; c->done = 0; c->idx = 0;
    /* pending, not committed: in-transaction scans may traverse pages
       allocated earlier in the same transaction */
    c->steps = 0; c->limit = pager_pending_count(p) + 1;
    uint32_t pgno = root;
    for (int depth = 0; depth < MAX_DEPTH; depth++) {
        const uint8_t *pg;
        int rc = get_checked(p, pgno, &pg);
        if (rc != Q_OK) return rc;
        if (pg[PH_TYPE] == PT_LEAF) break;
        int n = g16(pg + PH_NCELLS);
        pgno = (n > 0) ? g32(pg + int_cell_off(0) + 8) : g32(pg + PH_LINK);
        if (depth + 1 == MAX_DEPTH) return corrupt("tree deeper than 64 levels");
    }
    c->leaf = pgno;
    /* skip leading empty leaves; a corrupt sibling cycle must not loop */
    for (;;) {
        const uint8_t *pg;
        int rc = get_checked(p, c->leaf, &pg);
        if (rc != Q_OK) return rc;
        if (g16(pg + PH_NCELLS) > 0) return Q_OK;
        uint32_t next = g32(pg + PH_LINK);
        if (next == 0) { c->done = 1; return Q_OK; }
        if (++c->steps > c->limit) return corrupt("leaf sibling chain loops");
        c->leaf = next;
    }
}

int cursor_next(struct cursor *c) {
    if (c->done) return Q_OK;
    const uint8_t *pg;
    int rc = get_checked(c->p, c->leaf, &pg);
    if (rc != Q_OK) return rc;
    c->idx++;
    for (;;) {
        if (c->idx < g16(pg + PH_NCELLS)) return Q_OK;
        uint32_t next = g32(pg + PH_LINK);
        if (next == 0) { c->done = 1; return Q_OK; }
        c->leaf = next;
        c->idx = 0;
        if (++c->steps > c->limit) return corrupt("leaf sibling chain loops");
        rc = get_checked(c->p, c->leaf, &pg);
        if (rc != Q_OK) return rc;
    }
}

int cursor_get(struct cursor *c, int64_t *key, uint8_t *buf, uint32_t *len_io) {
    if (c->done) { snprintf(q_err, sizeof q_err, "cursor exhausted"); return Q_ERR_SCHEMA; }
    const uint8_t *pg;
    int rc = get_checked(c->p, c->leaf, &pg);
    if (rc != Q_OK) return rc;
    uint32_t off = leaf_cell_off(pg, c->idx);
    uint16_t len = g16(pg + off + 8);
    *key = g64(pg + off);
    if (buf) {
        if (*len_io < len) { snprintf(q_err, sizeof q_err, "row buffer too small"); return Q_ERR_TOO_BIG; }
        memcpy(buf, pg + off + LEAF_CELL_HDR, len);
    }
    *len_io = len;
    return Q_OK;
}
