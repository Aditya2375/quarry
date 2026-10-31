#!/bin/sh
# quarry test suite: deterministic cases, crash recovery, differential model.
set -e
cd "$(dirname "$0")/.."
BIN=build/quarry
T=/tmp/quarry-tests
rm -rf "$T" && mkdir -p "$T"
fails=0

run_case() {
    name=$1
    rm -f "$T/$name.qry" "$T/$name.qry.wal"
    # stdout is line-buffered so stdout/stderr interleaving is deterministic
    "$BIN" "$T/$name.qry" < "tests/cases/$name.sql" 2>&1 \
        | sed 's/^error: /ERROR: /' > "$T/$name.got" || true
    if ! diff -u "tests/cases/$name.out" "$T/$name.got" > "$T/$name.diff"; then
        echo "FAIL: $name (see $T/$name.diff)"
        fails=$((fails + 1))
    else
        echo "ok: $name"
    fi
}

run_case crud
run_case errors
run_case txn

# persistence: close cleanly, reopen, data intact
rm -f "$T/persist.qry" "$T/persist.qry.wal"
printf "CREATE TABLE k (id INT PRIMARY KEY, v TEXT);\nINSERT INTO k VALUES (1, 'persisted');\n" | "$BIN" "$T/persist.qry" > /dev/null
out=$("$BIN" "$T/persist.qry" -c "SELECT v FROM k WHERE id = 1;")
if [ "$out" = "V
persisted
(1 row)" ]; then echo "ok: persistence"; else echo "FAIL: persistence: [$out]"; fails=$((fails + 1)); fi

# crash recovery: committed rows survive .crash, uncommitted are gone
rm -f "$T/crash.qry" "$T/crash.qry.wal"
rc=0
printf "CREATE TABLE k (id INT PRIMARY KEY, v TEXT);\nINSERT INTO k VALUES (1, 'a');\nBEGIN;\nINSERT INTO k VALUES (2, 'b');\n.crash\n" | "$BIN" "$T/crash.qry" > /dev/null 2>&1 || rc=$?
[ $rc -eq 2 ] || { echo "FAIL: .crash exit code $rc"; fails=$((fails + 1)); }
out=$("$BIN" "$T/crash.qry" -c "SELECT COUNT(*) FROM k;")
if [ "$out" = "count
1
(1 row)" ]; then echo "ok: crash recovery"; else echo "FAIL: crash recovery: [$out]"; fails=$((fails + 1)); fi

# crash right after a checkpoint + later commits
rc=0
printf "INSERT INTO k VALUES (3, 'c');\n.checkpoint\nINSERT INTO k VALUES (4, 'd');\n.crash\n" | "$BIN" "$T/crash.qry" > /dev/null 2>&1 || rc=$?
[ $rc -eq 2 ] || { echo "FAIL: .crash exit code $rc (after checkpoint)"; fails=$((fails + 1)); }
out=$("$BIN" "$T/crash.qry" -c "SELECT COUNT(*) FROM k;")
# rows 1 (pre-crash), 3 (pre-checkpoint), 4 (post-checkpoint): all committed
if [ "$out" = "count
3
(1 row)" ]; then echo "ok: crash after checkpoint"; else echo "FAIL: crash after checkpoint: [$out]"; fails=$((fails + 1)); fi

# btree stress: 20k fat rows force interior splits; crash; verify on reopen
rm -f "$T/stress.qry" "$T/stress.qry.wal"
python3 - "$T" <<'PY'
import random, subprocess, sys, os
td, = sys.argv[1:2]
binp = "build/quarry"
db = os.path.join(td, "stress.qry")
rng = random.Random(11)
keys = rng.sample(range(1, 1000000), 20000)
sql = "CREATE TABLE kv (id INT PRIMARY KEY, v TEXT);\n" + \
      "".join("INSERT INTO kv VALUES (%d, '%s');\n" % (k, str(k) * 18) for k in keys) + ".crash\n"
r = subprocess.run([binp, db], input=sql, text=True, capture_output=True)
assert r.returncode == 2, r.returncode
r = subprocess.run([binp, db, "-c", "SELECT id FROM kv ORDER BY id;"], text=True, capture_output=True)
got = [int(l.strip()) for l in r.stdout.splitlines()[1:] if not l.startswith("(")]
assert got == sorted(keys), "stress mismatch"
print("ok: btree stress (20000 rows, interior splits, crash recovery)")
PY
[ $? -eq 0 ] || fails=$((fails + 1))

# btree/pager unit checks
cc -std=gnu11 -O2 -g -Wall -Wextra -Werror -Isrc \
    tests/unit/test_btree.c src/pager.c src/btree.c -o "$T/test_btree"
"$T/test_btree" || fails=$((fails + 1))

# differential model against sqlite3 (skip gracefully without python3+sqlite3)
if python3 -c 'import sqlite3' 2>/dev/null; then
    python3 tests/model_test.py || fails=$((fails + 1))
fi

if [ "$fails" -gt 0 ]; then
    echo "SUITE FAILED: $fails failing group(s)"
    exit 1
fi
echo "ALL TESTS PASSED"
