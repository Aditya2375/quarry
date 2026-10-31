#ifndef QUARRY_SQL_H
#define QUARRY_SQL_H
#include "quarry.h"

enum { S_CREATE, S_INSERT, S_SELECT, S_UPDATE, S_DELETE, S_BEGIN, S_COMMIT, S_ROLLBACK };
enum { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };

struct cond {
    char col[Q_MAX_NAME + 1];
    int op;
    struct qval v;
    char *owned;
};
struct assign {
    char col[Q_MAX_NAME + 1];
    struct qval v;
    char *owned;
};

struct stmt {
    int kind;
    char table[Q_MAX_NAME + 1];
    /* create */
    struct coldef cols[Q_MAX_COLS];
    int ncols;
    /* insert */
    char insert_cols[Q_MAX_COLS][Q_MAX_NAME + 1];
    int ninsert_cols;
    struct qval vals[Q_MAX_COLS];
    char *val_strs[Q_MAX_COLS];
    int nvals;
    /* select */
    bool sel_star, sel_count;
    char sel_cols[Q_MAX_COLS][Q_MAX_NAME + 1];
    int nsel;
    /* where */
    struct cond conds[Q_MAX_CONDS];
    int nconds;
    /* update */
    struct assign sets[Q_MAX_COLS];
    int nsets;
    /* order/limit */
    char order_col[Q_MAX_NAME + 1];
    bool has_order, order_desc, has_limit;
    int64_t limit;
};

int  sql_parse(const char *text, struct stmt *st);
void stmt_free(struct stmt *st);

#endif
