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

/* ---- round-11: randomized-order model test with structural validation ---- */
static void validate_or_die(struct pager *p, uint32_t root, int *depth_out) {
    char verr[256] = {0};
    int rc = btree_validate(p, root, depth_out, verr, sizeof verr);
    if (rc != Q_OK) fprintf(stderr, "VALIDATOR: %s\n", verr);
    CHECK(rc == Q_OK);
}

/* inserts nkeys shuffled keys with the given payload size, checking full
   scan order, find(), duplicate rejection and structural validity as it
   goes, then again after commit + close + reopen. */
static void randomized_model(uint64_t seed_base, int nseeds, int nkeys,
                             uint32_t payload_len, int64_t spread,
                             int check_every, int min_depth) {
    /* min_depth is the validator's leaf depth: 2 = three-level tree (what
       the critic called depth 3), 3 = four levels ("depth 4"). */
    for (int sd = 0; sd < nseeds; sd++) {
        rng_state = seed_base + (uint64_t)sd * 1000003ull;
        char path[64];
        snprintf(path, sizeof path, "/tmp/quarry-unit-rand-%d.qry", (int)(seed_base % 100000) + sd);
        unlink(path);
        char wpath[80]; snprintf(wpath, sizeof wpath, "%s.wal", path); unlink(wpath);
        struct pager *p;
        CHECK(pager_open(&p, path) == Q_OK);
        CHECK(pager_begin(p) == Q_OK);
        uint32_t root;
        CHECK(btree_create(p, &root) == Q_OK);

        int64_t *keys = malloc(sizeof(int64_t) * (size_t)nkeys);
        int64_t *model = malloc(sizeof(int64_t) * (size_t)nkeys);
        uint8_t *pl = malloc(payload_len);
        CHECK(keys && model && pl);
        for (int i = 0; i < nkeys; i++)
            keys[i] = (int64_t)i * spread - spread * (nkeys / 2);
        for (int i = nkeys - 1; i > 0; i--) {
            int j = (int)(xr() % (uint64_t)(i + 1));
            int64_t t = keys[i]; keys[i] = keys[j]; keys[j] = t;
        }
        int nm = 0;
        for (int i = 0; i < nkeys; i++) {
            for (uint32_t b = 0; b < payload_len; b++)
                pl[b] = (uint8_t)(keys[i] >> ((b % 8) * 8));
            CHECK(btree_insert(p, &root, keys[i], pl, payload_len) == Q_OK);
            int pos = nm;
            while (pos > 0 && model[pos - 1] > keys[i]) { model[pos] = model[pos - 1]; pos--; }
            model[pos] = keys[i]; nm++;
            if ((i + 1) % 200 == 0) { CHECK(pager_commit(p) == Q_OK); CHECK(pager_begin(p) == Q_OK); }
            if ((i + 1) % check_every == 0) {
                validate_or_die(p, root, NULL);
                struct cursor c;
                CHECK(cursor_first(p, root, &c) == Q_OK);
                int j = 0;
                while (!c.done) {
                    int64_t k; uint8_t o[1100]; uint32_t l = sizeof o;
                    CHECK(cursor_get(&c, &k, o, &l) == Q_OK);
                    CHECK(j < nm);
                    CHECK(k == model[j]);
                    CHECK(l == payload_len);
                    j++;
                    CHECK(cursor_next(&c) == Q_OK);
                }
                CHECK(j == nm);
                for (int j2 = 0; j2 < nm; j2 += 25) {
                    uint8_t o[1100]; uint32_t l = sizeof o;
                    CHECK(btree_find(p, root, model[j2], o, &l) == Q_OK);
                    CHECK(l == payload_len);
                }
                CHECK(btree_insert(p, &root, model[nm / 2], pl, payload_len) == Q_ERR_CONSTRAINT);
            }
        }
        /* reopen: validate, find EVERY key with payload, full ordered scan */
        CHECK(pager_commit(p) == Q_OK);
        CHECK(pager_close(p) == Q_OK);
        CHECK(pager_open(&p, path) == Q_OK);
        int depth = 0;
        validate_or_die(p, root, &depth);
        fprintf(stderr, "  seed %d: %d keys, depth %d\n", sd, nkeys, depth);
        CHECK(depth >= min_depth);
        for (int i = 0; i < nkeys; i++) {
            uint8_t o[1100]; uint32_t l = sizeof o;
            CHECK(btree_find(p, root, model[i], o, &l) == Q_OK);
            CHECK(l == payload_len);
            for (uint32_t b = 0; b < payload_len; b++)
                pl[b] = (uint8_t)(model[i] >> ((b % 8) * 8));
            CHECK(memcmp(o, pl, payload_len) == 0);
        }
        {
            struct cursor c;
            CHECK(cursor_first(p, root, &c) == Q_OK);
            int j = 0;
            while (!c.done) {
                int64_t k; uint8_t o[1100]; uint32_t l = sizeof o;
                CHECK(cursor_get(&c, &k, o, &l) == Q_OK);
                CHECK(j < nm);
                CHECK(k == model[j]);
                j++;
                CHECK(cursor_next(&c) == Q_OK);
            }
            CHECK(j == nm);
        }
        CHECK(pager_close(p) == Q_OK);
        unlink(path);
        unlink(wpath);
        free(keys); free(model); free(pl);
    }
}



#ifndef QUARRY_TEST_TINY_PAGES
static void model_and_corruption(void) {
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

    /* --- corruption guards --- */
    const char *cpath = "/tmp/quarry-unit-corrupt.qry";
    unlink(cpath);
    unlink("/tmp/quarry-unit-corrupt.qry.wal");
    CHECK(pager_open(&p, cpath) == Q_OK);
    CHECK(pager_begin(p) == Q_OK);
    CHECK(btree_create(p, &root) == Q_OK);
    memcpy(rec, &keys[0], 8);
    for (int i = 0; i < 5; i++) CHECK(btree_insert(p, &root, keys[i], rec, 8) == Q_OK);
    CHECK(pager_commit(p) == Q_OK);

    /* a leaf with ncells = 0xFFFF must be rejected, not walked */
    uint8_t *mp;
    CHECK(pager_begin(p) == Q_OK);
    CHECK(pager_get_mut(p, root, &mp) == Q_OK);
    mp[0] = 1;                    /* leaf */
    mp[1] = 0xFF; mp[2] = 0xFF;   /* ncells */
    CHECK(pager_commit(p) == Q_OK);
    { uint8_t o[16]; uint32_t l = sizeof o;
      CHECK(btree_find(p, root, keys[0], o, &l) == Q_ERR_CORRUPT); }
    { int64_t mx; CHECK(btree_max(p, root, &mx) == Q_ERR_CORRUPT); }
    { struct cursor c; CHECK(cursor_first(p, root, &c) == Q_ERR_CORRUPT); }
    CHECK(pager_begin(p) == Q_OK);
    { int64_t k = 424242; memcpy(rec, &k, 8);
      uint32_t r2 = root;
      CHECK(btree_insert(p, &r2, k, rec, 8) == Q_ERR_CORRUPT); }
    CHECK(pager_rollback(p) == Q_OK);

    /* an unknown page type must be rejected */
    CHECK(pager_begin(p) == Q_OK);
    CHECK(pager_get_mut(p, root, &mp) == Q_OK);
    memset(mp, 0, 128);
    mp[0] = 0x7F;
    CHECK(pager_commit(p) == Q_OK);
    { uint8_t o[16]; uint32_t l = sizeof o;
      CHECK(btree_find(p, root, keys[0], o, &l) == Q_ERR_CORRUPT); }

    /* an interior page pointing at itself must error, not loop */
    CHECK(pager_begin(p) == Q_OK);
    CHECK(pager_get_mut(p, root, &mp) == Q_OK);
    memset(mp, 0, 64);
    mp[0] = 2;                             /* interior */
    mp[1] = 1; mp[2] = 0;                  /* one cell */
    memcpy(mp + 16 + 8, &root, 4);         /* child = itself */
    CHECK(pager_commit(p) == Q_OK);
    { uint8_t o[16]; uint32_t l = sizeof o;
      CHECK(btree_find(p, root, keys[0], o, &l) == Q_ERR_CORRUPT); }
    { struct cursor c; CHECK(cursor_first(p, root, &c) == Q_ERR_CORRUPT); }
    { int64_t mx; CHECK(btree_max(p, root, &mx) == Q_ERR_CORRUPT); }

    /* interior cell count beyond the page must be rejected */
    CHECK(pager_begin(p) == Q_OK);
    CHECK(pager_get_mut(p, root, &mp) == Q_OK);
    memset(mp, 0, 64);
    mp[0] = 2;
    mp[1] = 0xFF; mp[2] = 0xFF;
    CHECK(pager_commit(p) == Q_OK);
    { struct cursor c; CHECK(cursor_first(p, root, &c) == Q_ERR_CORRUPT); }

    CHECK(pager_close(p) == Q_OK);
    unlink(cpath);
    unlink("/tmp/quarry-unit-corrupt.qry.wal");

}

#endif

int main(void) {
#ifdef QUARRY_TEST_TINY_PAGES
    /* shrunken fanouts: depth-4 trees with a few thousand rows */
    randomized_model(4242, 2, 4000, 8, 3, 500, 4);
    printf("%ld tiny-page checks passed (depth-4 randomized model)\n", checks);
#else
    model_and_corruption();
    /* random-order inserts at depth 3 with fat payloads: the shape that
       used to orphan leaves from the interior level */
    randomized_model(42, 3, 12000, 200, 13, 500, 2);
    printf("%ld checks passed\n", checks);
#endif
    return 0;
}
