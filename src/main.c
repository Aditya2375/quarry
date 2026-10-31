/* quarry: a small relational database. REPL and batch runner.
 *
 *   quarry [file.qry]          open (or create) and read statements
 *   quarry file.qry -c "SQL"   run one statement and exit
 *
 * Statements end with ';' and may span lines. Dot commands:
 *   .tables  .schema NAME  .open FILE  .checkpoint  .crash  .exit  .help
 * (.crash kills the process without checkpointing, to prove WAL recovery.
 *  It simulates an OS/process crash: commits that were fsynced must survive.
 *  It does not simulate machine power loss, which can also lose OS buffers.)
 */
#include "quarry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

static void usage(FILE *f) {
    fputs("usage: quarry [file.qry] [-c \"SQL\"] [-validate]\n"
          "  .tables         list tables\n"
          "  .schema NAME    show a table's CREATE statement\n"
          "  .open FILE      switch database file\n"
          "  .checkpoint     flush the WAL into the data file\n"
          "  .crash          simulate an OS/process crash (no checkpoint;\n"
          "                  fsynced commits survive - not full power loss)\n"
          "  .exit           checkpoint and quit\n", f);
}

/* first unquoted ';' in buf, or -1 */
static long find_stmt_end(const char *buf) {
    int in = 0;
    for (long i = 0; buf[i]; i++) {
        if (buf[i] == '\'') in = !in;
        else if (!in && buf[i] == '-' && buf[i + 1] == '-') {
            /* a '--' comment runs to the end of the line; a ';' inside it
               is comment text, not a statement boundary */
            while (buf[i] && buf[i] != '\n') i++;
            if (!buf[i]) return -1;
        }
        else if (buf[i] == ';' && !in) return i;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    const char *one = NULL;
    int do_validate = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) {
            if (i + 1 >= argc) { usage(stderr); return 2; }
            one = argv[++i];
        }
        else if (!strcmp(argv[i], "-validate")) { do_validate = 1; }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        else if (argv[i][0] == '-') { usage(stderr); return 2; }  /* never create a file named "-c" */
        else {
            if (path) { fprintf(stderr, "quarry: extra argument '%s' (one database file only)\n", argv[i]); usage(stderr); return 2; }
            path = argv[i];
        }
    }

    if (!path) path = "quarry.db";
    struct db *d = NULL;
    if (db_open(&d, path) != Q_OK) { fprintf(stderr, "error: %s\n", q_err); return 1; }

    if (do_validate) {
        char verr[256] = {0};
        if (db_validate(d, verr, sizeof verr) != Q_OK) {
            fprintf(stderr, "validate: %s\n", verr); db_close(d); return 1;
        }
        puts("validate: ok");
        if (db_close(d) != Q_OK) { fprintf(stderr, "error: %s\n", q_err); return 1; }
        return 0;
    }

    if (one) {
        if (db_exec(d, one) != Q_OK) { fprintf(stderr, "error: %s\n", q_err); db_close(d); return 1; }
        if (db_close(d) != Q_OK) { fprintf(stderr, "error: %s\n", q_err); return 1; }
        return 0;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);  /* keep stdout/stderr interleaving deterministic */
    int interactive = isatty(fileno(stdin));
    char buf[65536];
    size_t blen = 0;
    char line[4096];
    for (;;) {
        if (interactive) { fputs(blen ? "   ...> " : "quarry> ", stdout); fflush(stdout); }
        if (!fgets(line, sizeof line, stdin)) break;
        size_t ln = strlen(line);
        if (ln && blen + ln >= sizeof buf - 1) { fprintf(stderr, "error: statement too long\n"); blen = 0; continue; }
        memcpy(buf + blen, line, ln + 1);
        blen += ln;

        for (;;) {
            /* dot command: first non-space char is '.' */
            size_t lead = 0;
            while (lead < blen && (buf[lead] == ' ' || buf[lead] == '\t' || buf[lead] == '\n' || buf[lead] == '\r')) lead++;
            if (blen && buf[lead] == '.') {
                if (lead) { memmove(buf, buf + lead, blen - lead + 1); blen -= lead; }
                char *nl = strchr(buf, '\n');
                if (!nl) break;
                *nl = 0;
                char cmd[4000];
                size_t cn = strlen(buf);
                if (cn >= sizeof cmd) cn = sizeof cmd - 1;
                memcpy(cmd, buf, cn);
                cmd[cn] = 0;
                memmove(buf, nl + 1, strlen(nl + 1) + 1);
                blen = strlen(buf);
                char *arg = strchr(cmd, ' ');
                if (arg) { *arg = 0; arg++; while (*arg == ' ') arg++; }
                if (!strcmp(cmd, ".exit") || !strcmp(cmd, ".quit")) goto done;
                else if (!strcmp(cmd, ".crash")) { db_abandon(d); _exit(2); }
                else if (!strcmp(cmd, ".tables")) db_list_tables(d);
                else if (!strcmp(cmd, ".schema")) {
                    if (!arg) fprintf(stderr, "error: .schema needs a table name\n");
                    else {
                        arg[strcspn(arg, "\r\n")] = 0;
                        for (char *u = arg; *u; u++) *u = (char)toupper((unsigned char)*u);
                        if (db_show_schema(d, arg) != Q_OK) fprintf(stderr, "error: %s\n", q_err);
                    }
                }
                else if (!strcmp(cmd, ".checkpoint")) {
                    if (db_checkpoint(d) != Q_OK) fprintf(stderr, "error: %s\n", q_err);
                    else q_out("checkpoint complete.\n");
                }
                else if (!strcmp(cmd, ".open")) {
                    if (db_in_txn(d)) fprintf(stderr, "error: COMMIT or ROLLBACK before .open (refusing to silently roll back)\n");
                    else if (!arg) fprintf(stderr, "error: .open needs a path\n");
                    else {
                        arg[strcspn(arg, "\r\n")] = 0;
                        if (db_close(d) != Q_OK) { fprintf(stderr, "error: %s\n", q_err); return 1; }
                        if (db_open(&d, arg) != Q_OK) { fprintf(stderr, "error: %s\n", q_err); return 1; }
                    }
                }
                else if (!strcmp(cmd, ".help")) usage(stdout);
                else fprintf(stderr, "error: unknown command %s\n", cmd);
                continue;
            }
            long end = find_stmt_end(buf);
            if (end < 0) break;
            char stmt[65536];
            memcpy(stmt, buf, (size_t)end);
            stmt[end] = 0;
            memmove(buf, buf + end + 1, blen - (size_t)end);
            blen -= (size_t)end + 1;
            /* skip pure-whitespace statements */
            int blank = 1;
            for (long i = 0; stmt[i]; i++) if (stmt[i] != ' ' && stmt[i] != '\t' && stmt[i] != '\n' && stmt[i] != '\r') { blank = 0; break; }
            if (blank) continue;
            if (db_exec(d, stmt) != Q_OK)
                fprintf(stderr, "error: %s\n", q_err);
        }
    }
    /* EOF with an unterminated tail: run it like sqlite's CLI. Dropping it
       silently would lose an INSERT or a COMMIT the user believes happened
       (an open transaction then rolls back on close without a word). */
    {
        size_t lead = 0;
        for (;;) {
            while (lead < blen && (buf[lead] == ' ' || buf[lead] == '\t' || buf[lead] == '\n' || buf[lead] == '\r')) lead++;
            if (lead + 1 < blen && buf[lead] == '-' && buf[lead + 1] == '-') {
                while (lead < blen && buf[lead] != '\n') lead++;
                continue;
            }
            break;
        }
        if (lead < blen && buf[lead] != '.') {
            if (db_exec(d, buf + lead) != Q_OK)
                fprintf(stderr, "error: %s\n", q_err);
        } else if (lead < blen) {
            fprintf(stderr, "error: dot-command at end of input needs a newline\n");
        }
    }
done:
    if (db_close(d) != Q_OK) { fprintf(stderr, "error: %s\n", q_err); return 1; }
    return 0;
}
