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
run_case stmtatomic
run_case nulls
run_case limits

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

# H1 regression: a commit that fails mid-WAL-write must not poison later
# reads or the next checkpoint (partial frames are truncated away)
python3 - "$T" <<'PY'
import os, resource, signal, subprocess, sys
td, = sys.argv[1:2]
binp = "build/quarry"
db = os.path.join(td, "rlimit.qry")
fat = "x" * 800
seed = "CREATE TABLE k (id INT PRIMARY KEY, v TEXT);\n" + "".join(
    "INSERT INTO k VALUES (%d, '%s');\n" % (i, fat) for i in range(1, 6))
r = subprocess.run([binp, db], input=seed, text=True, capture_output=True)
assert r.returncode == 0, r.stderr

def limited():
    resource.setrlimit(resource.RLIMIT_FSIZE, (80 * 1024, 80 * 1024))
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)

# grow the WAL past the limit inside one big transaction: COMMIT must fail
stmts = "BEGIN;\n" + "".join(
    "INSERT INTO k VALUES (%d, '%s');\n" % (i, fat) for i in range(6, 200)) + "COMMIT;\n"
r = subprocess.run([binp, db], input=stmts, text=True, capture_output=True, preexec_fn=limited)
assert r.returncode == 0, (r.returncode, r.stderr)
assert "WAL write failed" in r.stderr or "WAL fsync failed" in r.stderr, r.stderr

# the failed COMMIT left that process's txn open; on exit it rolled back.
# What matters is on-disk state now: only the 5 seeded rows, fully readable.
r = subprocess.run([binp, db, "-c", "SELECT COUNT(*) FROM k;"], text=True, capture_output=True)
assert r.returncode == 0, r.stderr
assert r.stdout == "count\n5\n(1 row)\n", r.stdout
r = subprocess.run([binp, db], input=".checkpoint\n", text=True, capture_output=True)
assert "checkpoint complete." in r.stdout, (r.stdout, r.stderr)
r = subprocess.run([binp, db, "-c", "INSERT INTO k VALUES (6, 'after');"], text=True, capture_output=True)
assert r.returncode == 0, r.stderr
r = subprocess.run([binp, db, "-c", "SELECT COUNT(*) FROM k;"], text=True, capture_output=True)
assert r.stdout == "count\n6\n(1 row)\n", r.stdout
print("ok: failed commit under RLIMIT_FSIZE leaves database fully usable")
PY
[ $? -eq 0 ] || fails=$((fails + 1))

# H4 regression: no-PK table keeps allocating keys after the tail leaf empties
python3 - "$T" <<'PY'
import subprocess, sys, os
td, = sys.argv[1:2]
binp = "build/quarry"
db = os.path.join(td, "tailleaf.qry")
sql = "CREATE TABLE t (v TEXT);\n" + "".join("INSERT INTO t VALUES ('r%d');\n" % i for i in range(200))
r = subprocess.run([binp, db], input=sql, text=True, capture_output=True)
assert r.returncode == 0, r.stderr
r = subprocess.run([binp, db, "-c", "SELECT COUNT(*) FROM t;"], text=True, capture_output=True)
assert r.stdout == "count\n200\n(1 row)\n", r.stdout
# delete the tail: keys 151..200 sit in the last leaves
ids = "\n".join("DELETE FROM t WHERE v = 'r%d';" % i for i in range(150, 200)) + "\n"
r = subprocess.run([binp, db], input=ids, text=True, capture_output=True)
assert r.returncode == 0, r.stderr
r = subprocess.run([binp, db, "-c", "INSERT INTO t VALUES ('new');"], text=True, capture_output=True)
assert r.returncode == 0, ("wedged after tail-leaf delete", r.stderr)
assert "1 row inserted." in r.stdout, r.stdout
r = subprocess.run([binp, db, "-c", "SELECT COUNT(*) FROM t;"], text=True, capture_output=True)
assert r.stdout == "count\n151\n(1 row)\n", r.stdout
r = subprocess.run([binp, db, "-c", "SELECT v FROM t WHERE v = 'new';"], text=True, capture_output=True)
assert "new" in r.stdout, r.stdout
print("ok: no-PK key allocation survives an emptied tail leaf")
PY
[ $? -eq 0 ] || fails=$((fails + 1))

# H3 regression: corrupted pages are rejected, not crashed on or looped over
python3 - "$T" <<'PY'
import os, shutil, subprocess, sys
td, = sys.argv[1:2]
binp = "build/quarry"

def makedb(name, rows=50):
    db = os.path.join(td, name)
    sql = "CREATE TABLE t (id INT PRIMARY KEY, v TEXT);\n" + "".join(
        "INSERT INTO t VALUES (%d, 'row%d');\n" % (i, i) for i in range(rows))
    r = subprocess.run([binp, db], input=sql, text=True, capture_output=True)
    assert r.returncode == 0, r.stderr
    return db

def poke(db, off, data):
    with open(db, "r+b") as f:
        f.seek(off); f.write(data)

def expect_graceful(db, tag, stmt="SELECT COUNT(*) FROM t;"):
    try:
        r = subprocess.run([binp, db, "-c", stmt], text=True, capture_output=True, timeout=10)
    except subprocess.TimeoutExpired:
        raise AssertionError(tag + ": hung")
    assert r.returncode in (0, 1), (tag, r.returncode)

# 1) leaf with ncells = 0xFFFF: must error, not overflow
db = makedb("corrupt1.qry")
poke(db, 2 * 4096 + 1, b"\xff\xff")
r = subprocess.run([binp, db, "-c", "SELECT COUNT(*) FROM t;"], text=True, capture_output=True, timeout=10)
assert r.returncode == 1 and "corrupt" in r.stderr, (r.returncode, r.stderr)

# 2) interior page pointing at itself: must error, not hang
db = makedb("corrupt2.qry", rows=2000)
root_off = None
with open(db, "rb") as f:
    data = f.read()
# find an interior page (type byte 2) to twist into a self-loop
for pg in range(1, len(data) // 4096):
    if data[pg * 4096] == 2:
        root_off = pg * 4096
        break
assert root_off, "no interior page in 2000-row tree"
poke(db, root_off + 16 + 8, (root_off // 4096).to_bytes(4, "little"))
r = subprocess.run([binp, db, "-c", "SELECT COUNT(*) FROM t;"], text=True, capture_output=True, timeout=10)
assert r.returncode == 1 and ("corrupt" in r.stderr or "out of range" in r.stderr), (r.returncode, r.stderr)

# 3) garbage page type
db = makedb("corrupt3.qry")
poke(db, 2 * 4096, b"\x7f")
expect_graceful(db, "bad page type")
r = subprocess.run([binp, db, "-c", "SELECT COUNT(*) FROM t;"], text=True, capture_output=True, timeout=10)
assert r.returncode == 1 and "corrupt" in r.stderr, (r.returncode, r.stderr)
print("ok: corrupted pages rejected (ncells overflow, self-loop, bad type)")
PY
[ $? -eq 0 ] || fails=$((fails + 1))

# fuzzing: garbage SQL + on-disk corruption (shipped harness, see README)
python3 tests/fuzz.py || fails=$((fails + 1))

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
