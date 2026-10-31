/* Browser entry point: a persistent in-memory database + one SQL
   statement per call. Output is captured into a returned buffer. */
#include "quarry.h"

#include <emscripten.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *outbuf;
static size_t outlen, outcap;

static void buf_out(const char *fmt, ...) {
    va_list ap, aq;
    va_start(ap, fmt);
    va_copy(aq, ap);
    int need = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (need < 0) { va_end(ap); return; }
    if (outlen + (size_t)need + 1 > outcap) {
        size_t nc = (outlen + (size_t)need + 1) * 2;
        char *nb = realloc(outbuf, nc);
        if (!nb) { va_end(ap); return; }
        outbuf = nb; outcap = nc;
    }
    vsnprintf(outbuf + outlen, outcap - outlen, fmt, ap);
    outlen += (size_t)need;
    va_end(ap);
}

static void discard_out(const char *fmt, ...) { (void)fmt; }

static struct db *g_db;

static const char *SEED =
    "CREATE TABLE employees (id INT PRIMARY KEY, name TEXT, role TEXT, salary INT);"
    "INSERT INTO employees VALUES (1, 'Ada Lovelace', 'engineer', 128);"
    "INSERT INTO employees VALUES (2, 'Grace Hopper', 'manager', 150);"
    "INSERT INTO employees VALUES (3, 'Linus Torvalds', 'engineer', 110);"
    "INSERT INTO employees VALUES (4, 'Margaret Hamilton', 'engineer', 141);"
    "INSERT INTO employees VALUES (5, 'Dennis Ritchie', 'researcher', 135);"
    "INSERT INTO employees VALUES (6, 'Barbara Liskov', 'researcher', 139);"
    "INSERT INTO employees VALUES (7, 'Edsger Dijkstra', 'researcher', 118);";

static int open_and_seed(void) {
    if (g_db) return 1;
    if (db_open(&g_db, "/demo.qry") != Q_OK) return 0;
    /* fresh MEMFS on every page load: seed the demo table quietly */
    void (*saved)(const char *, ...) = q_out;
    q_out = discard_out;
    char sql[256];
    const char *p = SEED;
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t n = semi ? (size_t)(semi - p) : strlen(p);
        snprintf(sql, sizeof sql, "%.*s", (int)n, p);
        db_exec(g_db, sql);
        p += n + (semi ? 1 : 0);
    }
    q_out = saved;
    return 1;
}

/* run one statement; returns the output text (never NULL) */
EMSCRIPTEN_KEEPALIVE
const char *quarry_exec(const char *input) {
    outlen = 0;
    if (!outbuf) { outbuf = malloc(4096); outcap = 4096; }
    outbuf[0] = 0;
    if (!open_and_seed()) return "error: could not open the in-memory database\n";
    q_out = buf_out;
    if (db_exec(g_db, input) != Q_OK)
        buf_out("error: %s\n", q_err);
    return outbuf;
}

EMSCRIPTEN_KEEPALIVE
const char *quarry_tables(void) {
    outlen = 0;
    if (!outbuf) { outbuf = malloc(4096); outcap = 4096; }
    outbuf[0] = 0;
    if (!open_and_seed()) return "error\n";
    q_out = buf_out;
    db_list_tables(g_db);
    return outbuf;
}

/* wipe the demo database and reseed */
EMSCRIPTEN_KEEPALIVE
const char *quarry_reset(void) {
    if (g_db) { db_close(g_db); g_db = NULL; }
    unlink("/demo.qry");
    unlink("/demo.qry.wal");
    open_and_seed();
    return "database reset to the demo dataset.\n";
}
