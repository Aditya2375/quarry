/* Catalog, SQL executor, and the db handle.
 *
 * The catalog is itself a B+tree (root stored in the database header):
 * one fixed-size record per table, keyed by a sequence number.
 * Statements outside an explicit BEGIN...COMMIT run in their own
 * auto-commit transaction. A failed statement inside an explicit
 * transaction leaves it open so the caller can ROLLBACK.
 */
#include "quarry.h"
#include "sql.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void default_out(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void (*q_out)(const char *fmt, ...) = default_out;

/* Success lines for mutating statements are staged here and printed only
   after the enclosing auto-commit succeeds, so a commit failure never
   follows a printed "ok". */
static char pending_ok[256];

#define MAX_TABLES 64
#define MAX_ROWS_COLLECT 100000

struct db {
    struct pager *pg;
    struct tabledef tabs[MAX_TABLES];
    int ntabs;
    bool explicit_txn;
};

/* ---------- catalog records (fixed 584 bytes) ---------- */
#define CAT_NAME 0    /* 32 bytes, NUL-padded */
#define CAT_ROOT 32   /* u32 */
#define CAT_NCOLS 36  /* u32 */
#define CAT_COLS 40   /* per col: name 32, type u8, pk u8 */
#define CAT_REC_LEN (40 + Q_MAX_COLS * 34)

static void p32(uint8_t *b, uint32_t v) {
    b[0]=v&0xFF; b[1]=(v>>8)&0xFF; b[2]=(v>>16)&0xFF; b[3]=(v>>24)&0xFF;
}
static uint32_t g32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
}

static int fail(int code, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(q_err, sizeof q_err, fmt, ap);
    va_end(ap);
    return code;
}

static void copy_name(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}
static void cat_encode(const struct tabledef *t, uint8_t *rec) {
    memset(rec, 0, CAT_REC_LEN);
    snprintf((char *)rec + CAT_NAME, 32, "%s", t->name);
    p32(rec + CAT_ROOT, t->root);
    p32(rec + CAT_NCOLS, (uint32_t)t->ncols);
    for (int i = 0; i < t->ncols; i++) {
        uint8_t *c = rec + CAT_COLS + i * 34;
        copy_name((char *)c, 32, t->cols[i].name);
        c[32] = (uint8_t)t->cols[i].type;
        c[33] = (uint8_t)t->cols[i].is_pk;
    }
}
static void cat_decode(const uint8_t *rec, struct tabledef *t) {
    memset(t, 0, sizeof *t);
    copy_name(t->name, sizeof t->name, (char *)rec + CAT_NAME);
    t->root = g32(rec + CAT_ROOT);
    t->ncols = (int)g32(rec + CAT_NCOLS);
    if (t->ncols > Q_MAX_COLS) t->ncols = Q_MAX_COLS;
    for (int i = 0; i < t->ncols; i++) {
        const uint8_t *c = rec + CAT_COLS + i * 34;
        copy_name(t->cols[i].name, sizeof t->cols[i].name, (char *)c);
        t->cols[i].type = c[32];
        t->cols[i].is_pk = c[33];
    }
}

static int load_catalog(struct db *d) {
    /* build into scratch and swap only on success: a failed resync keeps
       the old catalog, so later statements still reach the pager and get
       its real error instead of a misleading "no table named ..." from a
       half-loaded catalog */
    struct tabledef *tabs = calloc(MAX_TABLES, sizeof *tabs);
    if (!tabs) return fail(Q_ERR_NOMEM, "out of memory");
    int ntabs = 0;
    uint32_t root; uint64_t seq;
    int rc = pager_get_catalog(d->pg, &root, &seq);
    if (rc == Q_OK && root != 0) {
        struct cursor c;
        rc = cursor_first(d->pg, root, &c);
        while (rc == Q_OK && !c.done) {
            if (ntabs == MAX_TABLES) { rc = fail(Q_ERR_TOO_BIG, "too many tables (max 64)"); break; }
            int64_t key; uint8_t rec[CAT_REC_LEN]; uint32_t len = sizeof rec;
            if ((rc = cursor_get(&c, &key, rec, &len)) != Q_OK) break;
            cat_decode(rec, &tabs[ntabs]);
            tabs[ntabs].catkey = (uint64_t)key;
            ntabs++;
            rc = cursor_next(&c);
        }
    }
    if (rc == Q_OK) {
        memcpy(d->tabs, tabs, (size_t)ntabs * sizeof *tabs);
        d->ntabs = ntabs;
    }
    free(tabs);
    return rc;
}

/* rewrite a table's catalog record (root page changes on root splits) */
static int save_table(struct db *d, struct tabledef *t) {
    uint32_t catroot; uint64_t seq;
    int rc = pager_get_catalog(d->pg, &catroot, &seq);
    if (rc != Q_OK || catroot == 0) return rc;
    uint8_t rec[CAT_REC_LEN];
    cat_encode(t, rec);
    uint32_t newroot = catroot;
    if ((rc = btree_delete(d->pg, catroot, (int64_t)t->catkey)) != Q_OK) return rc;
    if ((rc = btree_insert(d->pg, &newroot, (int64_t)t->catkey, rec, CAT_REC_LEN)) != Q_OK) return rc;
    if (newroot != catroot) return pager_set_catalog(d->pg, newroot, seq);
    return Q_OK;
}

/* validate the catalog tree and every table tree (tests + fuzzer) */
int db_validate(struct db *d, char *err, size_t errlen) {
    uint32_t catroot; uint64_t seq;
    int rc = pager_get_catalog(d->pg, &catroot, &seq);
    if (rc != Q_OK) { snprintf(err, errlen, "catalog: %s", q_err); return rc; }
    if (catroot) {
        rc = btree_validate(d->pg, catroot, NULL, err, errlen);
        if (rc != Q_OK) return rc;
    }
    rc = load_catalog(d);
    if (rc != Q_OK) { snprintf(err, errlen, "catalog: %s", q_err); return rc; }
    for (int i = 0; i < d->ntabs; i++) {
        char tbuf[256];
        rc = btree_validate(d->pg, d->tabs[i].root, NULL, tbuf, sizeof tbuf);
        if (rc != Q_OK) {
            snprintf(err, errlen, "table %s: %s", d->tabs[i].name, tbuf);
            return rc;
        }
    }
    return Q_OK;
}

static struct tabledef *find_table(struct db *d, const char *name) {
    for (int i = 0; i < d->ntabs; i++)
        if (strcmp(d->tabs[i].name, name) == 0) return &d->tabs[i];
    return NULL;
}
static int col_index(struct tabledef *t, const char *name) {
    for (int i = 0; i < t->ncols; i++)
        if (strcmp(t->cols[i].name, name) == 0) return i;
    return -1;
}

int db_open(struct db **out, const char *path) {
    struct db *d = calloc(1, sizeof *d);
    if (!d) return fail(Q_ERR_NOMEM, "out of memory");
    int rc = pager_open(&d->pg, path);
    if (rc != Q_OK) { free(d); return rc; }
    /* a SQL database with allocated pages always has a catalog tree: a
       zeroed catalog root on a multi-page file is silent schema loss */
    {
        uint32_t catroot; uint64_t seq;
        if ((rc = pager_get_catalog(d->pg, &catroot, &seq)) != Q_OK) { pager_close(d->pg); free(d); return rc; }
        if (catroot == 0 && pager_page_count(d->pg) > 1) {
            pager_close(d->pg); free(d);
            return fail(Q_ERR_CORRUPT, "database header is corrupt (catalog root missing)");
        }
    }
    if ((rc = load_catalog(d)) != Q_OK) { pager_close(d->pg); free(d); return rc; }
    *out = d;
    return Q_OK;
}
int db_close(struct db *d) {
    int rc = pager_close(d->pg);
    free(d);
    return rc;
}
void db_abandon(struct db *d) {
    pager_abandon(d->pg);
    free(d);
}
int db_checkpoint(struct db *d) { return pager_checkpoint(d->pg); }
bool db_in_txn(struct db *d) { return d->explicit_txn; }

/* ---------- value checks ---------- */
static int check_value(struct coldef *cd, struct qval *v) {
    if (v->type == 0) {
        if (cd->is_pk) return fail(Q_ERR_CONSTRAINT, "primary key cannot be NULL");
        return Q_OK;
    }
    if (v->type != cd->type)
        return fail(Q_ERR_TYPE, "column %s is %s but value is %s",
                    cd->name, cd->type == Q_COL_INT ? "INT" : "TEXT",
                    v->type == Q_COL_INT ? "an integer" : "text");
    if (cd->type == Q_COL_TEXT && v->slen > 800)
        return fail(Q_ERR_TOO_BIG, "text value too long (max 800 bytes)");
    return Q_OK;
}

/* compare a stored value with a literal: <0, 0, >0; NULLs sort lowest */
static int val_cmp(const struct qval *a, const struct qval *b) {
    if (a->type == 0 && b->type == 0) return 0;
    if (a->type == 0) return -1;
    if (b->type == 0) return 1;
    if (a->type == Q_COL_INT) {
        if (b->type != Q_COL_INT) return -2; /* mismatch */
        return a->i < b->i ? -1 : a->i > b->i ? 1 : 0;
    }
    if (b->type != Q_COL_TEXT) return -2;
    uint32_t n = a->slen < b->slen ? a->slen : b->slen;
    int r = n ? memcmp(a->s, b->s, n) : 0;
    /* normalize: memcmp can return any magnitude (e.g. -2), which would
       collide with the -2 type-mismatch sentinel above */
    if (r) return r < 0 ? -1 : 1;
    return a->slen < b->slen ? -1 : a->slen > b->slen ? 1 : 0;
}

static int cond_match(struct tabledef *t, struct cond *cd, struct qval *vals, bool *match) {
    int ci = col_index(t, cd->col);
    if (ci < 0) return fail(Q_ERR_SCHEMA, "no column named %s", cd->col);
    /* three-valued logic: a comparison that touches NULL is unknown,
       and unknown is not true - the row does not match (SQL semantics) */
    if (vals[ci].type == 0 || cd->v.type == 0) { *match = false; return Q_OK; }
    int cmp = val_cmp(&vals[ci], &cd->v);
    if (cmp == -2) return fail(Q_ERR_TYPE, "type mismatch in WHERE on column %s", cd->col);
    bool m;
    switch (cd->op) {
        case OP_EQ: m = cmp == 0; break;
        case OP_NE: m = cmp != 0; break;
        case OP_LT: m = cmp < 0; break;
        case OP_LE: m = cmp <= 0; break;
        case OP_GT: m = cmp > 0; break;
        default:    m = cmp >= 0; break;
    }
    *match = m;
    return Q_OK;
}

/* ---------- statements ---------- */
static int exec_create(struct db *d, struct stmt *st) {
    if (find_table(d, st->table))
        return fail(Q_ERR_SCHEMA, "table %s already exists", st->table);
    if (d->ntabs == MAX_TABLES) return fail(Q_ERR_TOO_BIG, "too many tables (max 64)");
    for (int i = 0; i < st->ncols; i++)
        for (int j = i + 1; j < st->ncols; j++)
            if (strcmp(st->cols[i].name, st->cols[j].name) == 0)
                return fail(Q_ERR_SCHEMA, "duplicate column %s", st->cols[i].name);

    struct tabledef t;
    memset(&t, 0, sizeof t);
    snprintf(t.name, sizeof t.name, "%s", st->table);
    t.ncols = st->ncols;
    memcpy(t.cols, st->cols, sizeof st->cols);

    int rc = btree_create(d->pg, &t.root);
    if (rc != Q_OK) return rc;

    uint32_t catroot; uint64_t seq;
    if ((rc = pager_get_catalog(d->pg, &catroot, &seq)) != Q_OK) return rc;
    if (catroot == 0) {
        if ((rc = btree_create(d->pg, &catroot)) != Q_OK) return rc;
    }
    t.catkey = seq;
    uint8_t rec[CAT_REC_LEN];
    cat_encode(&t, rec);
    rc = btree_insert(d->pg, &catroot, (int64_t)seq, rec, CAT_REC_LEN);
    if (rc != Q_OK) return rc;
    if ((rc = pager_set_catalog(d->pg, catroot, seq + 1)) != Q_OK) return rc;
    if ((rc = load_catalog(d)) != Q_OK) return rc;
    snprintf(pending_ok, sizeof pending_ok, "table %s created.\n", t.name);
    return Q_OK;
}

static int exec_insert(struct db *d, struct stmt *st) {
    struct tabledef *t = find_table(d, st->table);
    if (!t) return fail(Q_ERR_SCHEMA, "no table named %s", st->table);
    struct qval vals[Q_MAX_COLS] = {{0}};
    bool seen[Q_MAX_COLS] = {false};
    if (st->ninsert_cols) {
        if (st->ninsert_cols != st->nvals)
            return fail(Q_ERR_SCHEMA, "column list and VALUES have different lengths");
        for (int i = 0; i < st->nvals; i++) {
            int ci = col_index(t, st->insert_cols[i]);
            if (ci < 0) return fail(Q_ERR_SCHEMA, "no column named %s", st->insert_cols[i]);
            if (seen[ci])
                return fail(Q_ERR_SCHEMA, "column %s listed twice", st->insert_cols[i]);
            seen[ci] = true;
            vals[ci] = st->vals[i];
        }
    } else {
        if (st->nvals != t->ncols)
            return fail(Q_ERR_SCHEMA, "table %s has %d columns, got %d values",
                        t->name, t->ncols, st->nvals);
        for (int i = 0; i < st->nvals; i++) vals[i] = st->vals[i];
    }
    for (int i = 0; i < t->ncols; i++) {
        int rc = check_value(&t->cols[i], &vals[i]);
        if (rc != Q_OK) return rc;
    }
    int64_t key;
    if (t->cols[0].is_pk) {
        key = vals[0].i;
    } else {
        if (btree_max(d->pg, t->root, &key) == Q_OK) {
            if (key == INT64_MAX)
                return fail(Q_ERR_TOO_BIG,
                            "cannot auto-assign a key: INT64_MAX already used");
            key++;
        } else key = 1;
    }
    uint32_t need = 0;
    for (int i = 0; i < t->ncols; i++)
        need += vals[i].type == 0 ? 1 : vals[i].type == Q_COL_INT ? 9 : 3 + vals[i].slen;
    if (need > Q_MAX_ROW_BYTES)
        return fail(Q_ERR_TOO_BIG, "row too large (%u bytes, limit %d)", need, Q_MAX_ROW_BYTES);
    uint8_t buf[Q_MAX_ROW_BYTES + 64];
    uint32_t len = row_encode(t, vals, buf);
    uint32_t old_root = t->root;
    int rc = btree_insert(d->pg, &t->root, key, buf, len);
    if (rc != Q_OK) {
        t->root = old_root;   /* statement failed: in-memory root must match the pages again */
        if (rc == Q_ERR_CONSTRAINT)
            return fail(rc, "duplicate primary key %lld", (long long)key);
        return rc;
    }
    if (t->root != old_root && (rc = save_table(d, t)) != Q_OK) { t->root = old_root; return rc; }
    return Q_OK;   /* the caller prints success only after the commit lands */
}

struct rowvec { int64_t *keys; uint8_t **rows; uint32_t *lens; size_t n, cap; };
static int rowvec_push(struct rowvec *rv, int64_t key, const uint8_t *row, uint32_t len) {
    if (rv->n == MAX_ROWS_COLLECT)
        return fail(Q_ERR_TOO_BIG, "too many matching rows (limit 100000)");
    if (rv->n == rv->cap) {
        size_t nc = rv->cap ? rv->cap * 2 : 64;
        /* install each realloc only when it succeeds: a failed one leaves
           the original block owned by rv, so nothing is freed twice */
        int64_t *nk = realloc(rv->keys, nc * sizeof *nk);
        uint8_t **nr = realloc(rv->rows, nc * sizeof *nr);
        uint32_t *nl = realloc(rv->lens, nc * sizeof *nl);
        if (nk) rv->keys = nk;
        if (nr) rv->rows = nr;
        if (nl) rv->lens = nl;
        if (!nk || !nr || !nl) return fail(Q_ERR_NOMEM, "out of memory");
        rv->cap = nc;
    }
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) return fail(Q_ERR_NOMEM, "out of memory");
    memcpy(copy, row, len);
    rv->keys[rv->n] = key; rv->rows[rv->n] = copy; rv->lens[rv->n] = len; rv->n++;
    return Q_OK;
}
static void rowvec_free(struct rowvec *rv) {
    for (size_t i = 0; i < rv->n; i++) free(rv->rows[i]);
    free(rv->keys); free(rv->rows); free(rv->lens);
    memset(rv, 0, sizeof *rv);
}

/* scan + filter into rv */
static int collect(struct db *d, struct tabledef *t, struct stmt *st, struct rowvec *rv,
                   int64_t *count_out) {
    struct cursor c;
    int rc = cursor_first(d->pg, t->root, &c);
    if (rc != Q_OK) return rc;
    uint8_t buf[Q_MAX_ROW_BYTES + 64];
    int64_t count = 0;
    while (!c.done) {
        int64_t key; uint32_t len = sizeof buf;
        if ((rc = cursor_get(&c, &key, buf, &len)) != Q_OK) return rc;
        struct qval vals[Q_MAX_COLS];
        if ((rc = row_decode(t, buf, len, vals)) != Q_OK) return rc;
        bool ok = true;
        for (int i = 0; i < st->nconds && ok; i++)
            if ((rc = cond_match(t, &st->conds[i], vals, &ok)) != Q_OK) return rc;
        if (ok) {
            count++;
            if (rv && (rc = rowvec_push(rv, key, buf, len)) != Q_OK) return rc;
        }
        if ((rc = cursor_next(&c)) != Q_OK) return rc;
    }
    if (count_out) *count_out = count;
    return Q_OK;
}

/* order-by context for qsort (single-threaded) */
static struct tabledef *g_sort_t;
static int g_sort_col;
static int g_sort_desc;
static struct rowvec *g_sort_rv;
static int row_cmp(const void *pa, const void *pb) {
    size_t a = *(const size_t *)pa, b = *(const size_t *)pb;
    struct qval va[Q_MAX_COLS], vb[Q_MAX_COLS];
    row_decode(g_sort_t, g_sort_rv->rows[a], g_sort_rv->lens[a], va);
    row_decode(g_sort_t, g_sort_rv->rows[b], g_sort_rv->lens[b], vb);
    int c = val_cmp(&va[g_sort_col], &vb[g_sort_col]);
    if (g_sort_desc) c = -c;
    return c;
}

static void print_val(const struct qval *v, int width, bool last) {
    if (last) {  /* no padding on the final column: no trailing whitespace */
        if (v->type == 0) { q_out("NULL"); return; }
        if (v->type == Q_COL_INT) { q_out("%lld", (long long)v->i); return; }
        q_out("%.*s", (int)v->slen, v->s);
        return;
    }
    if (v->type == 0) { q_out("%-*s", width, "NULL"); return; }
    if (v->type == Q_COL_INT) { q_out("%-*lld", width, (long long)v->i); return; }
    q_out("%-*.*s", width, (int)v->slen, v->s);
}
static int val_width(const struct qval *v) {
    if (v->type == 0) return 4;
    if (v->type == Q_COL_INT) { char b[24]; return (int)snprintf(b, sizeof b, "%lld", (long long)v->i); }
    return (int)v->slen;
}

static int exec_select(struct db *d, struct stmt *st) {
    struct tabledef *t = find_table(d, st->table);
    if (!t) return fail(Q_ERR_SCHEMA, "no table named %s", st->table);

    if (st->sel_count) {
        if (st->has_order || st->has_limit)
            return fail(Q_ERR_SYNTAX, "ORDER BY and LIMIT are not supported with COUNT(*)");
        int64_t n;
        int rc = collect(d, t, st, NULL, &n);
        if (rc != Q_OK) return rc;
        q_out("count\n%lld\n(1 row)\n", (long long)n);
        return Q_OK;
    }

    int sel[Q_MAX_COLS], nsel;
    if (st->sel_star) { nsel = t->ncols; for (int i = 0; i < nsel; i++) sel[i] = i; }
    else {
        nsel = st->nsel;
        for (int i = 0; i < nsel; i++) {
            sel[i] = col_index(t, st->sel_cols[i]);
            if (sel[i] < 0) return fail(Q_ERR_SCHEMA, "no column named %s", st->sel_cols[i]);
        }
    }
    int order_col = -1;
    if (st->has_order) {
        order_col = col_index(t, st->order_col);
        if (order_col < 0) return fail(Q_ERR_SCHEMA, "no column named %s", st->order_col);
    }

    struct rowvec rv = {0};
    int rc = collect(d, t, st, &rv, NULL);
    if (rc != Q_OK) { rowvec_free(&rv); return rc; }

    /* sort indices if needed */
    size_t *ord = malloc(sizeof(size_t) * (rv.n ? rv.n : 1));
    if (!ord) { rowvec_free(&rv); return fail(Q_ERR_NOMEM, "out of memory"); }
    for (size_t i = 0; i < rv.n; i++) ord[i] = i;
    if (st->has_order && rv.n > 1) {
        g_sort_t = t; g_sort_col = order_col; g_sort_desc = st->order_desc; g_sort_rv = &rv;
        qsort(ord, rv.n, sizeof(size_t), row_cmp);
    }
    size_t show = rv.n;
    if (st->has_limit && (size_t)st->limit < show) show = (size_t)st->limit;

    /* column widths over the rows we will print */
    int widths[Q_MAX_COLS] = {0};
    for (int i = 0; i < nsel; i++) widths[i] = (int)strlen(t->cols[sel[i]].name);
    struct qval vals[Q_MAX_COLS];
    for (size_t r = 0; r < show; r++) {
        row_decode(t, rv.rows[ord[r]], rv.lens[ord[r]], vals);
        for (int i = 0; i < nsel; i++) {
            int w = val_width(&vals[sel[i]]);
            if (w > widths[i]) widths[i] = w;
        }
    }
    for (int i = 0; i < nsel; i++) {
        if (i) q_out(" | ");
        if (i + 1 == nsel) q_out("%s", t->cols[sel[i]].name);
        else q_out("%-*s", widths[i], t->cols[sel[i]].name);
    }
    q_out("\n");
    for (size_t r = 0; r < show; r++) {
        row_decode(t, rv.rows[ord[r]], rv.lens[ord[r]], vals);
        for (int i = 0; i < nsel; i++) {
            if (i) q_out(" | ");
            print_val(&vals[sel[i]], widths[i], i + 1 == nsel);
        }
        q_out("\n");
    }
    q_out("(%zu row%s)\n", show, show == 1 ? "" : "s");
    free(ord);
    rowvec_free(&rv);
    return Q_OK;
}

static int exec_update(struct db *d, struct stmt *st) {
    struct tabledef *t = find_table(d, st->table);
    if (!t) return fail(Q_ERR_SCHEMA, "no table named %s", st->table);
    int setidx[Q_MAX_COLS];
    for (int i = 0; i < st->nsets; i++) {
        setidx[i] = col_index(t, st->sets[i].col);
        if (setidx[i] < 0) return fail(Q_ERR_SCHEMA, "no column named %s", st->sets[i].col);
        int rc = check_value(&t->cols[setidx[i]], &st->sets[i].v);
        if (rc != Q_OK) return rc;
    }
    struct rowvec rv = {0};
    int rc = collect(d, t, st, &rv, NULL);
    if (rc != Q_OK) { rowvec_free(&rv); return rc; }
    uint8_t buf[Q_MAX_ROW_BYTES + 64];
    uint32_t root0 = t->root;
    for (size_t r = 0; r < rv.n; r++) {
        struct qval vals[Q_MAX_COLS];
        if ((rc = row_decode(t, rv.rows[r], rv.lens[r], vals)) != Q_OK) break;
        for (int i = 0; i < st->nsets; i++) vals[setidx[i]] = st->sets[i].v;
        uint32_t need = 0;
        for (int i = 0; i < t->ncols; i++)
            need += vals[i].type == 0 ? 1 : vals[i].type == Q_COL_INT ? 9 : 3 + vals[i].slen;
        if (need > Q_MAX_ROW_BYTES) { rc = fail(Q_ERR_TOO_BIG, "row too large after update (%u bytes, limit %d)", need, Q_MAX_ROW_BYTES); break; }
        uint32_t len = row_encode(t, vals, buf);
        int64_t newkey = t->cols[0].is_pk ? vals[0].i : rv.keys[r];
        if ((rc = btree_delete(d->pg, t->root, rv.keys[r])) != Q_OK) break;
        uint32_t old_root = t->root;
        if ((rc = btree_insert(d->pg, &t->root, newkey, buf, len)) != Q_OK) break;
        if (t->root != old_root && (rc = save_table(d, t)) != Q_OK) break;
    }
    if (rc != Q_OK) { t->root = root0; rowvec_free(&rv); return rc; }
    snprintf(pending_ok, sizeof pending_ok, "%zu row%s updated.\n", rv.n, rv.n == 1 ? "" : "s");
    rowvec_free(&rv);
    return Q_OK;
}

static int exec_delete(struct db *d, struct stmt *st) {
    struct tabledef *t = find_table(d, st->table);
    if (!t) return fail(Q_ERR_SCHEMA, "no table named %s", st->table);
    struct rowvec rv = {0};
    int rc = collect(d, t, st, &rv, NULL);
    if (rc != Q_OK) { rowvec_free(&rv); return rc; }
    for (size_t r = 0; r < rv.n; r++) {
        if ((rc = btree_delete(d->pg, t->root, rv.keys[r])) != Q_OK) break;
    }
    if (rc != Q_OK) { rowvec_free(&rv); return rc; }
    snprintf(pending_ok, sizeof pending_ok, "%zu row%s deleted.\n", rv.n, rv.n == 1 ? "" : "s");
    rowvec_free(&rv);
    return Q_OK;
}

/* ---------- entry point ---------- */
static bool needs_txn(int kind) {
    return kind == S_CREATE || kind == S_INSERT || kind == S_UPDATE || kind == S_DELETE;
}

int db_exec(struct db *d, const char *sql) {
    pending_ok[0] = 0; /* stage fresh success line per statement */
    struct stmt st;
    int rc = sql_parse(sql, &st);
    if (rc != Q_OK) return rc;

    if (st.kind == S_BEGIN) {
        stmt_free(&st);
        if (d->explicit_txn) return fail(Q_ERR_TXN, "already inside a transaction");
        if ((rc = pager_begin(d->pg)) != Q_OK) return rc;
        d->explicit_txn = true;
        q_out("begin.\n");
        return Q_OK;
    }
    if (st.kind == S_COMMIT) {
        stmt_free(&st);
        if (!d->explicit_txn) return fail(Q_ERR_TXN, "no transaction to commit");
        if ((rc = pager_commit(d->pg)) != Q_OK) return rc;
        d->explicit_txn = false;
        q_out("commit.\n");
        return Q_OK;
    }
    if (st.kind == S_ROLLBACK) {
        stmt_free(&st);
        if (!d->explicit_txn) return fail(Q_ERR_TXN, "no transaction to roll back");
        if ((rc = pager_rollback(d->pg)) != Q_OK) return rc;
        d->explicit_txn = false;
        /* the rollback reverted pages, so in-memory table roots and the
           table list are stale: resync from the catalog on disk */
        if ((rc = load_catalog(d)) != Q_OK) return rc;
        q_out("rollback.\n");
        return Q_OK;
    }

    bool own_txn = false;
    struct pager_savepoint sp;
    bool have_sp = false;
    if (needs_txn(st.kind)) {
        if (!d->explicit_txn) {
            if ((rc = pager_begin(d->pg)) != Q_OK) { stmt_free(&st); return rc; }
            own_txn = true;
        } else {
            /* statement atomicity inside an explicit transaction: a failed
               statement undoes only its own pages, the txn stays usable */
            pager_sp_mark(d->pg, &sp);
            have_sp = true;
        }
    }
    switch (st.kind) {
        case S_CREATE: rc = exec_create(d, &st); break;
        case S_INSERT: rc = exec_insert(d, &st); break;
        case S_SELECT: rc = exec_select(d, &st); break;
        case S_UPDATE: rc = exec_update(d, &st); break;
        case S_DELETE: rc = exec_delete(d, &st); break;
        default: rc = Q_ERR_SYNTAX; break;
    }
    int kind = st.kind;
    stmt_free(&st);
    if (own_txn) {
        if (rc != Q_OK) {
            pager_rollback(d->pg);
            /* pages reverted; resync in-memory roots with the catalog */
            int crc2 = load_catalog(d);
            if (crc2 != Q_OK) rc = crc2;
        } else {
            rc = pager_commit(d->pg);
            if (rc != Q_OK) {
                /* a failed commit leaves the pager mid-transaction; roll it
                   back or the session wedges: later statements would see the
                   uncommitted pages and every INSERT would report "already
                   inside a transaction". pager_rollback is a harmless no-op
                   error on the commit paths that already cleared the txn
                   state, so its result is ignored here. The catalog resync
                   is best-effort too: when it fails (e.g. the pager broke),
                   the original commit error says more, so rc and q_err are
                   both kept. */
                char saved_err[sizeof q_err];
                memcpy(saved_err, q_err, sizeof saved_err);
                pager_rollback(d->pg);
                load_catalog(d);
                memcpy(q_err, saved_err, sizeof q_err);
            }
        }
    } else if (have_sp && rc != Q_OK) {
        pager_sp_restore(d->pg, &sp);
        /* resync in-memory roots with the restored pages; if that fails the
           in-memory catalog cannot be trusted, so report it over the
           original statement error */
        int crc = load_catalog(d);
        if (crc != Q_OK) rc = crc;
    }
    if (rc == Q_OK && kind == S_INSERT) snprintf(pending_ok, sizeof pending_ok, "1 row inserted.\n");
    if (rc == Q_OK && pending_ok[0]) { q_out("%s", pending_ok); pending_ok[0] = 0; }
    return rc;
}

int db_list_tables(struct db *d) {
    if (d->ntabs == 0) { q_out("(no tables)\n"); return Q_OK; }
    for (int i = 0; i < d->ntabs; i++) q_out("%s\n", d->tabs[i].name);
    return Q_OK;
}
int db_show_schema(struct db *d, const char *name) {
    struct tabledef *t = find_table(d, name);
    if (!t) return fail(Q_ERR_SCHEMA, "no table named %s", name);
    q_out("CREATE TABLE %s (\n", t->name);
    for (int i = 0; i < t->ncols; i++)
        q_out("  %s %s%s%s\n", t->cols[i].name,
              t->cols[i].type == Q_COL_INT ? "INT" : "TEXT",
              t->cols[i].is_pk ? " PRIMARY KEY" : "",
              i + 1 < t->ncols ? "," : "");
    q_out(");\n");
    return Q_OK;
}
