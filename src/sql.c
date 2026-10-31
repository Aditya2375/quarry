/* Tokenizer + parser for the SQL subset. Grammar (case-insensitive):
 *
 *   CREATE TABLE name (col TYPE [PRIMARY KEY], ...)
 *   INSERT INTO name [(cols)] VALUES (lit, ...)   (one row per statement)
 *   SELECT * | cols | COUNT(*) FROM name [WHERE cond [AND ...]]
 *        [ORDER BY col [ASC|DESC]] [LIMIT n]
 *   UPDATE name SET col = lit [, ...] WHERE cond [AND ...]
 *   DELETE FROM name WHERE cond [AND ...]
 *   BEGIN | COMMIT | ROLLBACK
 *
 *   cond: col op lit    op: = != < <= > >=
 *   lit:  integer | 'string' ('' escapes a quote) | NULL
 */
#include "quarry.h"
#include "sql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------- tokens ---------- */
enum { T_WORD, T_NUM, T_STR, T_OP, T_PUNCT, T_END };
struct tok {
    int kind;
    char word[64];     /* T_WORD: uppercased text */
    int64_t num;
    char *str;         /* T_STR: owned */
    char ch;           /* T_OP/T_PUNCT: first char; T_OP two-char in ch2 */
    char ch2;
};

struct parser {
    const char *cur;
    struct tok t;
    char *owned[Q_MAX_COLS + 8];  /* strings to free on error */
    int nowned;
    bool num_overflow;            /* last T_NUM did not fit in int64 */
};

static void tok_free(struct tok *t) {
    if (t->kind == T_STR && t->str) free(t->str);
    t->str = NULL;
}

static bool kw(struct parser *ps, const char *k) {
    return ps->t.kind == T_WORD && strcmp(ps->t.word, k) == 0;
}

static void next(struct parser *ps) {
    tok_free(&ps->t);
    const char *c = ps->cur;
    while (*c && isspace((unsigned char)*c)) c++;
    /* -- line comment */
    while (c[0] == '-' && c[1] == '-') {
        while (*c && *c != '\n') c++;
        while (*c && isspace((unsigned char)*c)) c++;
    }
    memset(&ps->t, 0, sizeof ps->t);
    if (!*c) { ps->t.kind = T_END; ps->cur = c; return; }
    if (isalpha((unsigned char)*c) || *c == '_') {
        size_t n = 0;
        while (isalnum((unsigned char)*c) || *c == '_') {
            if (n < sizeof ps->t.word - 1)
                ps->t.word[n++] = (char)toupper((unsigned char)*c);
            c++;
        }
        ps->t.word[n] = 0;
        ps->t.kind = T_WORD;
    } else if (isdigit((unsigned char)*c) ||
               (*c == '-' && isdigit((unsigned char)c[1]))) {
        ps->t.kind = T_NUM;
        bool neg = *c == '-';
        if (neg) c++;
        /* checked accumulation: no signed overflow on huge literals,
           and -9223372036854775808 (INT64_MIN) parses correctly */
        uint64_t lim = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
        uint64_t u = 0;
        ps->num_overflow = false;
        while (isdigit((unsigned char)*c)) {
            uint64_t dgt = (uint64_t)(*c - '0');
            if (u > (lim - dgt) / 10) ps->num_overflow = true;
            else u = u * 10 + dgt;
            c++;
        }
        ps->t.num = neg ? (u == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)u)
                        : (int64_t)u;
    } else if (*c == '\'') {
        c++;
        size_t cap = 32, n = 0;
        char *s = malloc(cap);
        if (!s) { ps->t.kind = T_END; ps->cur = c; return; }
        for (;;) {
            if (!*c) { free(s); ps->t.kind = T_END; ps->cur = c; return; }
            if (*c == '\'') {
                if (c[1] == '\'') { c += 2; }
                else { c++; break; }
            }
            if (n + 2 > cap) { cap *= 2; s = realloc(s, cap); if (!s) { ps->t.kind = T_END; ps->cur = c; return; } }
            s[n++] = *c++;
        }
        s[n] = 0;
        ps->t.kind = T_STR;
        ps->t.str = s;
    } else if (*c == '=' || *c == '<' || *c == '>' || *c == '!') {
        ps->t.kind = T_OP;
        ps->t.ch = *c;
        if ((*c == '<' || *c == '>' || *c == '!') && c[1] == '=') { ps->t.ch2 = '='; c++; }
        if (*c == '!' && !ps->t.ch2) ps->t.ch = '?'; /* bare ! = invalid op */
        c++;
    } else {
        ps->t.kind = T_PUNCT;
        ps->t.ch = *c++;
    }
    ps->cur = c;
}

static int fail(struct parser *ps, const char *msg) {
    (void)ps;
    snprintf(q_err, sizeof q_err, "syntax error: %s", msg);
    return Q_ERR_SYNTAX;
}
static int expect_kw(struct parser *ps, const char *k) {
    if (!kw(ps, k)) { char m[64]; snprintf(m, sizeof m, "expected %s", k); return fail(ps, m); }
    next(ps); return Q_OK;
}
static int expect_punct(struct parser *ps, char ch) {
    if (ps->t.kind != T_PUNCT || ps->t.ch != ch) {
        char m[64]; snprintf(m, sizeof m, "expected '%c'", ch); return fail(ps, m);
    }
    next(ps); return Q_OK;
}
static int ident(struct parser *ps, char *out) {
    if (ps->t.kind != T_WORD) return fail(ps, "expected a name");
    size_t n = strlen(ps->t.word);
    if (n > Q_MAX_NAME) return fail(ps, "name too long (max 31 chars)");
    memcpy(out, ps->t.word, n + 1);
    next(ps); return Q_OK;
}
static void own(struct parser *ps, char *s) {
    if (ps->nowned < Q_MAX_COLS + 8) ps->owned[ps->nowned++] = s;
}

static int literal(struct parser *ps, struct qval *v, char **owned) {
    if (ps->t.kind == T_NUM) {
        if (ps->num_overflow) return fail(ps, "integer literal out of range");
        v->type = Q_COL_INT; v->i = ps->t.num;
        *owned = NULL;
        next(ps); return Q_OK;
    }
    if (ps->t.kind == T_STR) {
        v->type = Q_COL_TEXT; v->s = ps->t.str; v->slen = (uint32_t)strlen(ps->t.str);
        *owned = ps->t.str;
        own(ps, ps->t.str);
        ps->t.str = NULL;
        next(ps); return Q_OK;
    }
    if (kw(ps, "NULL")) {
        v->type = 0; *owned = NULL;
        next(ps); return Q_OK;
    }
    return fail(ps, "expected a number, string, or NULL");
}

/* ---------- statement parsing ---------- */
static int parse_create(struct parser *ps, struct stmt *st) {
    st->kind = S_CREATE;
    int rc;
    if ((rc = expect_kw(ps, "TABLE")) != Q_OK) return rc;
    if ((rc = ident(ps, st->table)) != Q_OK) return rc;
    if ((rc = expect_punct(ps, '(')) != Q_OK) return rc;
    st->ncols = 0;
    int npk = 0;
    for (;;) {
        if (st->ncols == Q_MAX_COLS) return fail(ps, "too many columns (max 16)");
        struct coldef *cd = &st->cols[st->ncols];
        if ((rc = ident(ps, cd->name)) != Q_OK) return rc;
        if (kw(ps, "INT")) cd->type = Q_COL_INT;
        else if (kw(ps, "TEXT")) cd->type = Q_COL_TEXT;
        else return fail(ps, "column type must be INT or TEXT");
        next(ps);
        cd->is_pk = 0;
        if (kw(ps, "PRIMARY")) {
            next(ps);
            if ((rc = expect_kw(ps, "KEY")) != Q_OK) return rc;
            cd->is_pk = 1;
            npk++;
        }
        st->ncols++;
        if (ps->t.kind == T_PUNCT && ps->t.ch == ',') { next(ps); continue; }
        break;
    }
    if (npk > 1) return fail(ps, "at most one PRIMARY KEY");
    if (npk && !st->cols[0].is_pk) return fail(ps, "PRIMARY KEY must be the first column");
    if (npk && st->cols[0].type != Q_COL_INT) return fail(ps, "PRIMARY KEY must be INT");
    return expect_punct(ps, ')');
}

static int parse_insert(struct parser *ps, struct stmt *st) {
    st->kind = S_INSERT;
    int rc;
    if ((rc = expect_kw(ps, "INTO")) != Q_OK) return rc;
    if ((rc = ident(ps, st->table)) != Q_OK) return rc;
    st->ninsert_cols = 0;
    if (ps->t.kind == T_PUNCT && ps->t.ch == '(') {
        next(ps);
        for (;;) {
            if (st->ninsert_cols == Q_MAX_COLS) return fail(ps, "too many columns");
            if ((rc = ident(ps, st->insert_cols[st->ninsert_cols++])) != Q_OK) return rc;
            if (ps->t.kind == T_PUNCT && ps->t.ch == ',') { next(ps); continue; }
            break;
        }
        if ((rc = expect_punct(ps, ')')) != Q_OK) return rc;
    }
    if ((rc = expect_kw(ps, "VALUES")) != Q_OK) return rc;
    if ((rc = expect_punct(ps, '(')) != Q_OK) return rc;
    st->nvals = 0;
    for (;;) {
        if (st->nvals == Q_MAX_COLS) return fail(ps, "too many values (max 16)");
        char *o;
        if ((rc = literal(ps, &st->vals[st->nvals], &o)) != Q_OK) return rc;
        st->val_strs[st->nvals] = o;
        st->nvals++;
        if (ps->t.kind == T_PUNCT && ps->t.ch == ',') { next(ps); continue; }
        break;
    }
    return expect_punct(ps, ')');
}

static int parse_cond(struct parser *ps, struct cond *cd) {
    int rc;
    if ((rc = ident(ps, cd->col)) != Q_OK) return rc;
    if (ps->t.kind != T_OP) return fail(ps, "expected a comparison operator");
    char a = ps->t.ch, b = ps->t.ch2;
    if (a == '=' ) cd->op = OP_EQ;
    else if (a == '!' && b == '=') cd->op = OP_NE;
    else if (a == '<' && !b) cd->op = OP_LT;
    else if (a == '<' && b == '=') cd->op = OP_LE;
    else if (a == '>' && !b) cd->op = OP_GT;
    else if (a == '>' && b == '=') cd->op = OP_GE;
    else return fail(ps, "bad operator");
    next(ps);
    return literal(ps, &cd->v, &cd->owned);
}

static int parse_select(struct parser *ps, struct stmt *st) {
    st->kind = S_SELECT;
    int rc;
    next(ps); /* consume SELECT */
    st->sel_star = false; st->sel_count = false; st->nsel = 0;
    if (ps->t.kind == T_PUNCT && ps->t.ch == '*') {
        st->sel_star = true;
        next(ps);
    } else if (kw(ps, "COUNT")) {
        next(ps);
        if ((rc = expect_punct(ps, '(')) != Q_OK) return rc;
        if ((rc = expect_punct(ps, '*')) != Q_OK) return rc;
        if ((rc = expect_punct(ps, ')')) != Q_OK) return rc;
        st->sel_count = true;
    } else {
        for (;;) {
            if (st->nsel == Q_MAX_COLS) return fail(ps, "too many columns");
            if ((rc = ident(ps, st->sel_cols[st->nsel++])) != Q_OK) return rc;
            if (ps->t.kind == T_PUNCT && ps->t.ch == ',') { next(ps); continue; }
            break;
        }
    }
    if ((rc = expect_kw(ps, "FROM")) != Q_OK) return rc;
    if ((rc = ident(ps, st->table)) != Q_OK) return rc;
    return Q_OK;
}

static int parse_update(struct parser *ps, struct stmt *st) {
    st->kind = S_UPDATE;
    int rc;
    next(ps);
    if ((rc = ident(ps, st->table)) != Q_OK) return rc;
    if ((rc = expect_kw(ps, "SET")) != Q_OK) return rc;
    st->nsets = 0;
    for (;;) {
        if (st->nsets == Q_MAX_COLS) return fail(ps, "too many SET assignments");
        struct assign *a = &st->sets[st->nsets];
        if ((rc = ident(ps, a->col)) != Q_OK) return rc;
        if (ps->t.kind != T_OP || ps->t.ch != '=') return fail(ps, "expected '='");
        next(ps);
        if ((rc = literal(ps, &a->v, &a->owned)) != Q_OK) return rc;
        st->nsets++;
        if (ps->t.kind == T_PUNCT && ps->t.ch == ',') { next(ps); continue; }
        break;
    }
    return Q_OK;
}

static int parse_delete(struct parser *ps, struct stmt *st) {
    st->kind = S_DELETE;
    int rc;
    next(ps);
    if ((rc = expect_kw(ps, "FROM")) != Q_OK) return rc;
    return ident(ps, st->table);
}

/* shared tail: WHERE / ORDER BY / LIMIT */
static int parse_tail(struct parser *ps, struct stmt *st) {
    int rc;
    st->nconds = 0;
    st->has_order = false; st->order_desc = false;
    st->has_limit = false;
    if (kw(ps, "WHERE")) {
        next(ps);
        for (;;) {
            if (st->nconds == Q_MAX_CONDS) return fail(ps, "too many WHERE conditions (max 8)");
            if ((rc = parse_cond(ps, &st->conds[st->nconds++])) != Q_OK) return rc;
            if (kw(ps, "AND")) { next(ps); continue; }
            break;
        }
    }
    if (kw(ps, "ORDER")) {
        next(ps);
        if ((rc = expect_kw(ps, "BY")) != Q_OK) return rc;
        if ((rc = ident(ps, st->order_col)) != Q_OK) return rc;
        st->has_order = true;
        if (kw(ps, "DESC")) { st->order_desc = true; next(ps); }
        else if (kw(ps, "ASC")) next(ps);
    }
    if (kw(ps, "LIMIT")) {
        next(ps);
        if (ps->t.kind != T_NUM || ps->num_overflow || ps->t.num < 0)
            return fail(ps, "LIMIT needs a non-negative integer");
        st->limit = ps->t.num;
        st->has_limit = true;
        next(ps);
    }
    if (st->kind != S_SELECT && (st->has_order || st->has_limit))
        return fail(ps, "ORDER BY and LIMIT are only supported on SELECT");
    if (ps->t.kind == T_PUNCT && ps->t.ch == ';') next(ps);  /* optional trailing ';' */
    if (ps->t.kind != T_END) return fail(ps, "unexpected trailing input");
    return Q_OK;
}

void stmt_free(struct stmt *st) {
    for (int i = 0; i < Q_MAX_COLS; i++) { free(st->val_strs[i]); st->val_strs[i] = NULL; }
    for (int i = 0; i < st->nconds; i++) { free(st->conds[i].owned); st->conds[i].owned = NULL; }
    for (int i = 0; i < st->nsets; i++) { free(st->sets[i].owned); st->sets[i].owned = NULL; }
}

int sql_parse(const char *text, struct stmt *st) {
    struct parser ps = { .cur = text };
    memset(st, 0, sizeof *st);
    ps.t.str = NULL;
    ps.t.kind = T_END;
    next(&ps);
    int rc;
    if (kw(&ps, "CREATE")) { next(&ps); rc = parse_create(&ps, st); }
    else if (kw(&ps, "INSERT")) { next(&ps); rc = parse_insert(&ps, st); }
    else if (kw(&ps, "SELECT")) rc = parse_select(&ps, st);
    else if (kw(&ps, "UPDATE")) rc = parse_update(&ps, st);
    else if (kw(&ps, "DELETE")) rc = parse_delete(&ps, st);
    else if (kw(&ps, "BEGIN")) { st->kind = S_BEGIN; next(&ps); rc = Q_OK; }
    else if (kw(&ps, "COMMIT")) { st->kind = S_COMMIT; next(&ps); rc = Q_OK; }
    else if (kw(&ps, "ROLLBACK")) { st->kind = S_ROLLBACK; next(&ps); rc = Q_OK; }
    else rc = fail(&ps, "expected CREATE, INSERT, SELECT, UPDATE, DELETE, BEGIN, COMMIT or ROLLBACK");
    if (rc == Q_OK) {
        if (st->kind == S_BEGIN || st->kind == S_COMMIT || st->kind == S_ROLLBACK) {
            if (ps.t.kind == T_PUNCT && ps.t.ch == ';') next(&ps);
            if (ps.t.kind != T_END) rc = fail(&ps, "unexpected input after transaction statement");
        } else {
            rc = parse_tail(&ps, st);
        }
    }
    if (rc != Q_OK) stmt_free(st);
    tok_free(&ps.t);
    return rc;
}
