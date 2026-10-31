/* Unit checks for the pager + B+tree against a simple in-memory model. */
#include "quarry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long checks;
#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } \
} while (0)

/* xorshift for reproducible randomness */
static uint64_t rng_state = 88172645463325252ull;
static uint64_t xr(void) {
    rng_state ^= rng_state << 13; rng_state >>= 7; rng_state ^= rng_state << 17;
    return rng_state;
}

#define NKEYS 5000

int main(void) {
    const char *path = "/tmp/quarry-unit.qry";
    unlink(path);
    unlink("/tmp/quarry-unit.qry.wal");

    struct pager *p;
    CHECK(pager_open(&p, path) == Q_OK);
    CHECK(pager_begin(p) == Q_OK);
    uint32_t root;
    CHECK(btree_create(p, &root) == Q_OK);

    /* model: present[i], value[i] */
    static int present[NKEYS];
    int64_t keys[NKEYS];
    /* unique random keys */
    for (int i = 0; i < NKEYS; i++) {
        int64_t k;
        int dup;
        do {
            dup = 0;
            k = (int64_t)(xr() % 1000000) - 500000;   /* exercise negatives */
            for (int j = 0; j < i; j++) if (keys[j] == k) { dup = 1; break; }
        } while (dup);
        keys[i] = k;
    }

    uint8_t rec[16];
    for (int i = 0; i < NKEYS; i++) {
        uint32_t old = root;
        memcpy(rec, &keys[i], 8);
        CHECK(btree_insert(p, &root, keys[i], rec, 8) == Q_OK);
        (void)old;
        present[i] = 1;
        if (i % 997 == 0) {  /* periodic commit+reopen cycles */
            CHECK(pager_commit(p) == Q_OK);
            CHECK(pager_close(p) == Q_OK);
            CHECK(pager_open(&p, path) == Q_OK);
            CHECK(pager_begin(p) == Q_OK);
        }
    }
    /* duplicates must be rejected */
    memcpy(rec, &keys[0], 8);
    CHECK(btree_insert(p, &root, keys[0], rec, 8) == Q_ERR_CONSTRAINT);
    /* oversized row must be rejected cleanly */
    static uint8_t big[3000];
    CHECK(btree_insert(p, &root, 2000000, big, sizeof big) == Q_ERR_TOO_BIG);

    /* every key findable with the right payload */
    for (int i = 0; i < NKEYS; i++) {
        uint8_t out[16]; uint32_t len = sizeof out;
        CHECK(btree_find(p, root, keys[i], out, &len) == Q_OK);
        CHECK(len == 8 && memcmp(out, &keys[i], 8) == 0);
    }
    /* a missing key reports not-found */
    { uint8_t o[16]; uint32_t l = sizeof o; CHECK(btree_find(p, root, 1500000, o, &l) == Q_ERR_SCHEMA); }

    /* ordered scan matches the sorted model */
    {
        int64_t *sorted = malloc(NKEYS * sizeof(int64_t));
        memcpy(sorted, keys, NKEYS * sizeof(int64_t));
        int cmp(const void *a, const void *b) {
            int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
            return x < y ? -1 : x > y;
        }
        qsort(sorted, NKEYS, sizeof(int64_t), cmp);
        struct cursor c;
        CHECK(cursor_first(p, root, &c) == Q_OK);
        int i = 0;
        while (!c.done) {
            int64_t k; uint8_t o[16]; uint32_t l = sizeof o;
            CHECK(cursor_get(&c, &k, o, &l) == Q_OK);
            CHECK(k == sorted[i]);
            i++;
            CHECK(cursor_next(&c) == Q_OK);
        }
        CHECK(i == NKEYS);
        free(sorted);
    }

    /* delete every third key; verify gone and the rest intact */
    for (int i = 0; i < NKEYS; i += 3) {
        CHECK(btree_delete(p, root, keys[i]) == Q_OK);
        present[i] = 0;
    }
    CHECK(btree_delete(p, root, 1500001) == Q_OK);  /* deleting absent key is a no-op */
    for (int i = 0; i < NKEYS; i++) {
        uint8_t o[16]; uint32_t l = sizeof o;
        int rc = btree_find(p, root, keys[i], o, &l);
        CHECK(rc == (present[i] ? Q_OK : Q_ERR_SCHEMA));
    }

    /* rollback discards uncommitted inserts */
    CHECK(pager_commit(p) == Q_OK);
    CHECK(pager_begin(p) == Q_OK);
    memcpy(rec, &keys[0], 8);
    CHECK(btree_insert(p, &root, 1600000, rec, 8) == Q_OK);
    { uint8_t o[16]; uint32_t l = sizeof o; CHECK(btree_find(p, root, 1600000, o, &l) == Q_OK); }
    CHECK(pager_rollback(p) == Q_OK);
    CHECK(pager_begin(p) == Q_OK);
    { uint8_t o[16]; uint32_t l = sizeof o; CHECK(btree_find(p, root, 1600000, o, &l) == Q_ERR_SCHEMA); }
    CHECK(pager_rollback(p) == Q_OK);

    /* committed data survives close + reopen */
    CHECK(pager_close(p) == Q_OK);
    CHECK(pager_open(&p, path) == Q_OK);
    for (int i = 0; i < NKEYS; i += 7) {
        uint8_t o[16]; uint32_t l = sizeof o;
        int rc = btree_find(p, root, keys[i], o, &l);
        CHECK(rc == (present[i] ? Q_OK : Q_ERR_SCHEMA));
        if (rc == Q_OK) CHECK(l == 8 && memcmp(o, &keys[i], 8) == 0);
    }
    /* abandoned (crashed) pager: committed state still there */
    CHECK(pager_begin(p) == Q_OK);
    memcpy(rec, &keys[0], 8);
    CHECK(btree_insert(p, &root, 1700000, rec, 8) == Q_OK);
    CHECK(pager_commit(p) == Q_OK);
    pager_abandon(p);   /* no checkpoint */
    CHECK(pager_open(&p, path) == Q_OK);
    { uint8_t o[16]; uint32_t l = sizeof o; CHECK(btree_find(p, root, 1700000, o, &l) == Q_OK); }
    for (int i = 0; i < NKEYS; i += 11) {
        uint8_t o[16]; uint32_t l = sizeof o;
        int rc = btree_find(p, root, keys[i], o, &l);
        CHECK(rc == (present[i] ? Q_OK : Q_ERR_SCHEMA));
    }
    CHECK(pager_close(p) == Q_OK);

    printf("%ld checks passed\n", checks);
    return 0;
}
