#ifndef QUARRY_H
#define QUARRY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define Q_PAGE_SIZE     4096
#define Q_MAX_COLS      16
#define Q_MAX_NAME      31
#define Q_MAX_ROW_BYTES 1000   /* one row must fit in a leaf cell */
#define Q_MAX_CONDS     8      /* ANDed WHERE conditions per statement */
#define Q_TXN_MAX_PAGES 2048   /* one transaction may dirty at most this many pages (8 MB) */

enum { Q_COL_INT = 1, Q_COL_TEXT = 2 };

enum qerr {
    Q_OK = 0,
    Q_ERR_IO,          /* filesystem failure */
    Q_ERR_CORRUPT,     /* bad on-disk structure */
    Q_ERR_SYNTAX,      /* SQL parse error */
    Q_ERR_SCHEMA,      /* unknown table/column, duplicate table */
    Q_ERR_TYPE,        /* value does not match column type */
    Q_ERR_CONSTRAINT,  /* duplicate primary key */
    Q_ERR_TOO_BIG,     /* row / page / transaction limit exceeded */
    Q_ERR_TXN,         /* BEGIN/COMMIT/ROLLBACK misuse */
    Q_ERR_NOMEM
};

/* last error detail, set by any failing call */
extern char q_err[256];

/* a literal or decoded column value; text is NOT NUL-terminated */
struct qval {
    int type;          /* 0 = NULL, Q_COL_INT, Q_COL_TEXT */
    int64_t i;
    const char *s;
    uint32_t slen;
};

struct coldef {
    char name[Q_MAX_NAME + 1];
    int  type;         /* Q_COL_INT / Q_COL_TEXT */
    int  is_pk;
};

struct tabledef {
    char name[Q_MAX_NAME + 1];
    uint32_t root;
    uint64_t catkey;     /* key of this table's catalog record */
    int ncols;
    struct coldef cols[Q_MAX_COLS];
};

/* ---------- pager (pages + WAL) ---------- */
struct pager;

int  pager_open(struct pager **out, const char *path);
int  pager_close(struct pager *p);        /* checkpoints, then closes */
void pager_abandon(struct pager *p);      /* close fds without checkpoint (simulated crash) */

int  pager_begin(struct pager *p);
int  pager_commit(struct pager *p);
int  pager_rollback(struct pager *p);
bool pager_in_txn(struct pager *p);

/* statement-level savepoint inside an open transaction */
struct pager_savepoint { size_t ndirty; uint32_t pending; };
void pager_sp_mark(struct pager *p, struct pager_savepoint *sp);
void pager_sp_restore(struct pager *p, const struct pager_savepoint *sp);

int  pager_get(struct pager *p, uint32_t pgno, const uint8_t **page);
int  pager_get_mut(struct pager *p, uint32_t pgno, uint8_t **page);
int  pager_alloc(struct pager *p, uint32_t *pgno, uint8_t **page);
int  pager_checkpoint(struct pager *p);
uint32_t pager_page_count(struct pager *p);
uint32_t pager_pending_count(struct pager *p);
int  pager_get_catalog(struct pager *p, uint32_t *root, uint64_t *seq);
int  pager_set_catalog(struct pager *p, uint32_t root, uint64_t seq);

/* ---------- B+tree (int64 keys -> row blobs) ---------- */
int btree_create(struct pager *p, uint32_t *root_out);
int btree_insert(struct pager *p, uint32_t *root_io, int64_t key, const uint8_t *data, uint32_t len);
int btree_delete(struct pager *p, uint32_t root, int64_t key);  /* missing key = Q_OK */
int btree_find(struct pager *p, uint32_t root, int64_t key,
               uint8_t *buf, uint32_t *len_io);                 /* Q_ERR_SCHEMA if absent */
int btree_max(struct pager *p, uint32_t root, int64_t *key_out); /* Q_ERR_SCHEMA if empty */

struct cursor {
    struct pager *p;
    uint32_t leaf;
    int idx;
    int done;
    /* sibling-link hop budget for the WHOLE scan, not one cursor_next call:
       a cycle through non-empty leaves must trip it too */
    uint32_t steps;
    uint32_t limit;
};
int cursor_first(struct pager *p, uint32_t root, struct cursor *c);
int cursor_next(struct cursor *c);
int cursor_get(struct cursor *c, int64_t *key, uint8_t *buf, uint32_t *len_io);

/* ---------- db (catalog + SQL executor) ---------- */
struct db;

/* row output goes through this hook (printf by default) */
extern void (*q_out)(const char *fmt, ...);

int  db_open(struct db **out, const char *path);
int  db_close(struct db *d);
void db_abandon(struct db *d);
int  db_exec(struct db *d, const char *sql);        /* one statement */
bool db_in_txn(struct db *d);                       /* explicit BEGIN open */
int  db_checkpoint(struct db *d);
int  db_list_tables(struct db *d);                  /* prints via q_out */
int  db_show_schema(struct db *d, const char *name);

/* ---------- row encoding ---------- */
uint32_t row_encode(const struct tabledef *t, const struct qval *vals, uint8_t *buf);
int      row_decode(const struct tabledef *t, const uint8_t *buf, uint32_t len, struct qval *vals);

#endif
