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
run_case strings
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


# round-2 H3a regression: a sibling-link cycle through NON-EMPTY leaves must
# error, not spin (the step counter used to reset on every cursor_next call,
# so COUNT(*) hung at 100% CPU on a cycle through leaves with cells)
python3 - "$T" <<'PY'
import subprocess, sys, os
td, = sys.argv[1:2]
binp = "build/quarry"
db = os.path.join(td, "linkcycle.qry")
sql = "CREATE TABLE t (id INT PRIMARY KEY, v TEXT);\n" + "".join(
    "INSERT INTO t VALUES (%d, 'row%d');\n" % (i, i) for i in range(1, 201))
r = subprocess.run([binp, db], input=sql, text=True, capture_output=True)
assert r.returncode == 0, r.stderr
with open(db, "rb") as f:
    data = f.read()
pg = None
for p in range(len(data) // 4096 - 1, 0, -1):
    if data[p * 4096] == 1 and int.from_bytes(data[p*4096+1:p*4096+3], "little") > 0:
        pg = p
        break
assert pg, "no non-empty leaf found"
with open(db, "r+b") as f:
    f.seek(pg * 4096 + 7)              # PH_LINK
    f.write(pg.to_bytes(4, "little"))  # self-link
try:
    r = subprocess.run([binp, db, "-c", "SELECT COUNT(*) FROM t;"],
                       text=True, capture_output=True, timeout=15)
except subprocess.TimeoutExpired:
    raise AssertionError("COUNT(*) hung on a sibling-link cycle")
assert r.returncode == 1 and "corrupt" in r.stderr, (r.returncode, r.stderr)
print("ok: non-empty leaf sibling cycle errors instead of hanging")
PY
[ $? -eq 0 ] || fails=$((fails + 1))

# round-2 H3b regressions: PH_USED must match the packed cells, leaf key
# order is enforced, and row field tags must match column types
python3 - "$T" <<'PY'
import subprocess, sys, os
td, = sys.argv[1:2]
binp = "build/quarry"

def makedb(name, rows):
    db = os.path.join(td, name)
    sql = "CREATE TABLE t (id INT PRIMARY KEY, v TEXT);\n" + "".join(
        "INSERT INTO t VALUES (%d, 'row%d');\n" % (i, i) for i in range(1, rows + 1))
    r = subprocess.run([binp, db], input=sql, text=True, capture_output=True)
    assert r.returncode == 0, r.stderr
    return db

def last_leaf(db, ncells_min=1):
    with open(db, "rb") as f:
        data = f.read()
    for p in range(len(data) // 4096 - 1, 0, -1):
        if data[p * 4096] == 1 and int.from_bytes(data[p*4096+1:p*4096+3], "little") >= ncells_min:
            return p
    raise AssertionError("no leaf found")

# used=0 on a leaf with cells: a 780-byte insert used to memmove past the page
db = makedb("usedzero.qry", 50)
pg = last_leaf(db)
with open(db, "r+b") as f:
    f.seek(pg * 4096 + 3); f.write(b"\x00\x00\x00\x00")
r = subprocess.run([binp, db, "-c", "INSERT INTO t VALUES (999999, '%s');" % ("x" * 780)],
                   text=True, capture_output=True, timeout=15)
assert r.returncode == 1 and "corrupt" in r.stderr, (r.returncode, r.stderr)

# out-of-order keys inside a leaf
db = makedb("keyorder.qry", 50)
pg = last_leaf(db, 2)
with open(db, "r+b") as f:
    f.seek(pg * 4096 + 16)                       # first cell's key
    f.write(b"\xff\xff\xff\xff\xff\xff\xff\x7f")  # huge key ahead of smaller ones
r = subprocess.run([binp, db, "-c", "SELECT COUNT(*) FROM t;"],
                   text=True, capture_output=True, timeout=15)
assert r.returncode == 1 and "corrupt" in r.stderr, (r.returncode, r.stderr)

# row field tag contradicts the column type (TEXT column, INT tag)
db = makedb("badtag.qry", 200)
pg = last_leaf(db)
with open(db, "r+b") as f:
    f.seek(pg * 4096 + 16 + 10 + 9)  # first cell: past key+len, past INT field
    f.write(b"\x01")
r = subprocess.run([binp, db, "-c", "SELECT * FROM t;"],
                   text=True, capture_output=True, timeout=15)
assert r.returncode == 1 and "corrupt" in r.stderr, (r.returncode, r.stderr)
print("ok: PH_USED / key-order / row-type corruption rejected")
PY
[ $? -eq 0 ] || fails=$((fails + 1))

# round-2 N1 regression: garbage appended to the WAL tail is truncated at
# open, so commits that follow it stay reachable after later crashes
rm -f "$T/waltail.qry" "$T/waltail.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY, v INT);\nINSERT INTO t VALUES (1, 1);\n.crash\n" | "$BIN" "$T/waltail.qry" > /dev/null 2>&1 || true
python3 -c "open('$T/waltail.qry.wal','ab').write(b'X'*100)"
printf "INSERT INTO t VALUES (2, 2);\n.crash\n" | "$BIN" "$T/waltail.qry" > /dev/null 2>&1 || true
out=$("$BIN" "$T/waltail.qry" -c "SELECT id FROM t ORDER BY id;")
if [ "$out" = "ID
1
2
(2 rows)" ]; then echo "ok: garbage WAL tail truncated at open"; else echo "FAIL: WAL tail: [$out]"; fails=$((fails + 1)); fi

# ---- round-3 regressions ----------------------------------------------

# F1a: a rolled-back CREATE TABLE must leave no in-memory trace: the same
# name can be created again right after the rollback
rm -f "$T/r3f1a.qry" "$T/r3f1a.qry.wal"
out=$(printf "BEGIN;\nCREATE TABLE x (id INT PRIMARY KEY);\nROLLBACK;\nCREATE TABLE x (id INT PRIMARY KEY);\nINSERT INTO x VALUES (7);\nSELECT COUNT(*) FROM x;\n" | "$BIN" "$T/r3f1a.qry")
case "$out" in
  *"rollback."*"table X created."*"1 row inserted."*"1"*) echo "ok: rollback of CREATE TABLE resyncs the catalog";;
  *) echo "FAIL: rollback CREATE: [$out]"; fails=$((fails + 1));;
esac

# F1b: rollback of a transaction that split table a's root must not leave a
# stale root; later writes to another table must not leak into a
rm -f "$T/r3f1b.qry" "$T/r3f1b.qry.wal"
{
  echo "CREATE TABLE a (id INT PRIMARY KEY, v INT);"
  echo "INSERT INTO a VALUES (1, 1);"; echo "INSERT INTO a VALUES (2, 2);"
  echo "CREATE TABLE b (id INT PRIMARY KEY, w INT);"
  echo "BEGIN;"
  i=1; while [ $i -le 200 ]; do echo "INSERT INTO a VALUES ($((i + 2)), $i);"; i=$((i + 1)); done
  echo "ROLLBACK;"
  i=1; while [ $i -le 399 ]; do echo "INSERT INTO b VALUES ($i, $i);"; i=$((i + 1)); done
  echo "SELECT COUNT(*) FROM a;"
  echo "INSERT INTO a VALUES (99999, 7);"
  echo "SELECT COUNT(*) FROM a;"
  echo "SELECT COUNT(*) FROM b;"
} | "$BIN" "$T/r3f1b.qry" > "$T/r3f1b.out" 2>&1
ca=$(sed -n 's/^count$/{n;p}' "$T/r3f1b.out" 2>/dev/null | head -1)
out=$(awk '/^count$/{getline; print}' "$T/r3f1b.out" | tr '\n' ' ')
if [ "$out" = "2 3 399 " ]; then echo "ok: rollback after root splits resyncs in-memory roots"; else echo "FAIL: rollback roots: [$out]"; fails=$((fails + 1)); fi

# F2: pages allocated inside an open transaction are readable by later
# statements in the same transaction (pending-count budget, not just
# committed page_count)
rm -f "$T/r3f2.qry" "$T/r3f2.qry.wal"
{
  echo "CREATE TABLE t (id INT PRIMARY KEY, v TEXT);"
  echo "INSERT INTO t VALUES (1, 'x');"
  echo "BEGIN;"
  i=2; while [ $i -le 41 ]; do echo "INSERT INTO t VALUES ($i, '$(printf 'y%.0s' $(seq 1 750))');"; i=$((i + 1)); done
  echo "SELECT COUNT(*) FROM t;"
  echo "COMMIT;"
} | "$BIN" "$T/r3f2.qry" > "$T/r3f2.out" 2>&1
if grep -q "leaf sibling chain loops" "$T/r3f2.out"; then
  echo "FAIL: in-txn page budget: sibling loop reported"; fails=$((fails + 1))
elif awk '/^count$/{getline; print}' "$T/r3f2.out" | grep -q '^41$'; then
  echo "ok: in-txn reads see pages allocated by the same transaction"
else
  echo "FAIL: in-txn page budget: [$(cat "$T/r3f2.out" | tr '\n' ' ' | tail -c 120)]"; fails=$((fails + 1))
fi

# F3: dead frames between two committed runs must not fold into the second
# run at recovery (crafted WAL: commitA + CRC-valid dead frame + commitB)
rm -f "$T/r3f3.qry" "$T/r3f3.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY, v INT);\n.crash\n" | "$BIN" "$T/r3f3.qry" > /dev/null 2>&1 || true
printf "INSERT INTO t VALUES (1, 1);\n.crash\n" | "$BIN" "$T/r3f3.qry" > /dev/null 2>&1 || true
python3 - "$T/r3f3.qry.wal" <<'EOF'
import struct, sys, zlib
wal = open(sys.argv[1], "rb").read()
frames = []
off = 16
while off + 24 <= len(wal):
    magic, pgno, cp, tf = struct.unpack_from("<IIII", wal, off)
    if magic != 0x4D415246: break
    frames.append((off, cp))
    off += 24 + 4096
ci = next(i for i, (_, cp) in enumerate(frames) if cp)
a_end = frames[ci][0] + 24 + 4096
txA, txB = wal[16:a_end], wal[a_end:]
hdr = bytearray(24)
struct.pack_into("<IIII", hdr, 0, 0x4D415246, 0, 0, 0)   # dead frame: page 0, no commit
struct.pack_into("<I", hdr, 16, zlib.crc32(hdr[4:16] + bytes(4096)))
open(sys.argv[1], "wb").write(wal[:16] + txA + bytes(hdr) + bytes(4096) + txB)
EOF
out=$(printf ".tables\nSELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r3f3.qry" 2>&1)
case "$out" in
  *"T"*"1"*) echo "ok: dead WAL frames between commits are ignored at recovery";;
  *) echo "FAIL: dead WAL frame folded: [$out]"; fails=$((fails + 1));;
esac

# F4: the frame CRC covers the header fields, so flipping a committed
# frame's pgno invalidates the frame and recovery stops there
rm -f "$T/r3f4.qry" "$T/r3f4.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY, v INT);\nINSERT INTO t VALUES (1, 1);\n.crash\n" | "$BIN" "$T/r3f4.qry" > /dev/null 2>&1 || true
python3 - "$T/r3f4.qry.wal" <<'EOF'
import struct, sys
p = sys.argv[1]
wal = bytearray(open(p, "rb").read())
off = 16; last = -1
while off + 24 <= len(wal):
    magic, pgno, cp = struct.unpack_from("<III", wal, off)
    if magic != 0x4D415246: break
    last = off
    off += 24 + 4096
struct.pack_into("<I", wal, last + 4, (struct.unpack_from("<I", wal, last + 4)[0] + 1) & 0xFFFFFFFF)
open(p, "wb").write(bytes(wal))
EOF
out=$(printf ".tables\n" | "$BIN" "$T/r3f4.qry" 2>&1)
if [ "$out" = "T" ]; then echo "ok: header fields are CRC-covered (flipped pgno rejected)"; else echo "FAIL: pgno flip accepted: [$out]"; fails=$((fails + 1)); fi

# F5: a walmap failure during commit must not lose committed data
rm -f "$T/r3f5.qry" "$T/r3f5.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY, v INT);\nINSERT INTO t VALUES (1, 1);\n" | "$BIN" "$T/r3f5.qry" > /dev/null
# transient failure: commit retries via WAL rebuild and succeeds
out=$(QUARRY_WALMAP_FAIL_AFTER=1000000 sh -c "printf 'INSERT INTO t VALUES (2, 2);\n' | '$BIN' '$T/r3f5.qry'" 2>&1)
case "$out" in
  *"1 row inserted."*) : ;;
  *) echo "FAIL: transient walmap failure: [$out]"; fails=$((fails + 1));;
esac
out=$("$BIN" "$T/r3f5.qry" -c "SELECT COUNT(*) FROM t;")
case "$out" in
  *"2"*) echo "ok: transient walmap failure rebuilt from WAL, data durable";;
  *) echo "FAIL: transient walmap durability: [$out]"; fails=$((fails + 1));;
esac
# persistent failure: honest error, no false success, WAL left alone
out=$(QUARRY_WALMAP_FAIL_AFTER=1 sh -c "printf 'INSERT INTO t VALUES (3, 3);\n' | '$BIN' '$T/r3f5.qry'" 2>&1)
case "$out" in
  *"1 row inserted."*) echo "FAIL: persistent walmap failure printed success: [$out]"; fails=$((fails + 1));;
  *"WAL map rebuild failed"*) : ;;
  *) echo "FAIL: persistent walmap failure not reported: [$out]"; fails=$((fails + 1));;
esac
out=$("$BIN" "$T/r3f5.qry" -c "SELECT id FROM t ORDER BY id;")
if [ "$out" = "ID
1
2
3
(3 rows)" ]; then echo "ok: persistent walmap failure leaves WAL intact, reopen recovers"; else echo "FAIL: persistent walmap recovery: [$out]"; fails=$((fails + 1)); fi

# ---- round-4 regressions ----------------------------------------------

# N1: a non-empty WAL with an unknown magic must fail the open, never be
# truncated away
rm -f "$T/r4n1.qry" "$T/r4n1.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY, v INT);\nINSERT INTO t VALUES (1, 1);\n.crash\n" | "$BIN" "$T/r4n1.qry" > /dev/null 2>&1 || true
python3 -c "
import sys
p = '$T/r4n1.qry.wal'
d = open(p, 'r+b'); d.write(b'XXXX'); d.close()"
sum_before=$(md5sum "$T/r4n1.qry.wal" | cut -d' ' -f1)
out=$(printf ".tables\n" | "$BIN" "$T/r4n1.qry" 2>&1) && rc=0 || rc=$?
sum_after=$(md5sum "$T/r4n1.qry.wal" | cut -d' ' -f1)
if [ $rc -ne 0 ] && [ "$sum_before" = "$sum_after" ] && echo "$out" | grep -q "unknown format"; then
  echo "ok: foreign WAL rejected, never truncated"
else
  echo "FAIL: foreign WAL handling (rc=$rc out=[$out])"; fails=$((fails + 1))
fi

# N3: ORDER BY / LIMIT are rejected on non-SELECT statements instead of
# being silently ignored
rm -f "$T/r4n3.qry" "$T/r4n3.qry.wal"
out=$(printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (1);\nINSERT INTO t VALUES (2);\nDELETE FROM t WHERE id > 0 LIMIT 1;\nUPDATE t SET id = 9 ORDER BY id;\nINSERT INTO t VALUES (3) LIMIT 1;\nSELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r4n3.qry" 2>&1)
n_err=$(echo "$out" | grep -c "only supported on SELECT")
n_rows=$(echo "$out" | awk '/^count$/{getline; print}')
if [ "$n_err" = "3" ] && [ "$n_rows" = "2" ]; then
  echo "ok: ORDER BY/LIMIT rejected off SELECT"
else
  echo "FAIL: LIMIT/ORDER rejection (errs=$n_err rows=$n_rows)"; fails=$((fails + 1))
fi

# N4: a broken pager refuses reads and allocations, not just writes
rm -f "$T/r4n4.qry" "$T/r4n4.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\n" | "$BIN" "$T/r4n4.qry" > /dev/null
out=$(QUARRY_WALMAP_FAIL_AFTER=1 sh -c "printf 'INSERT INTO t VALUES (1);\nSELECT COUNT(*) FROM t;\nCREATE TABLE u (id INT PRIMARY KEY);\n' | '$BIN' '$T/r4n4.qry'" 2>&1)
n_unusable=$(echo "$out" | grep -c "pager unusable")
if [ "$n_unusable" -ge 2 ] && ! echo "$out" | grep -q "row inserted\|table U created"; then
  echo "ok: broken pager refuses reads and allocations"
else
  echo "FAIL: broken pager served work: [$out]"; fails=$((fails + 1))
fi

# N5: a ';' inside a '--' comment is not a statement boundary
rm -f "$T/r4n5.qry" "$T/r4n5.qry.wal"
out=$(printf -- "-- drop; everything\nCREATE TABLE c1 (id INT PRIMARY KEY);\nSELECT COUNT(*) FROM c1;\n" | "$BIN" "$T/r4n5.qry" 2>&1)
case "$out" in
  *"table C1 created."*"0"*) echo "ok: semicolon inside comment is not a boundary";;
  *) echo "FAIL: comment semicolon: [$out]"; fails=$((fails + 1));;
esac

# R8 D2: a trailing statement without ';' at EOF must still run (sqlite CLI
# behavior) - a silently dropped INSERT/COMMIT loses acknowledged work
rm -f "$T/r8d2.qry" "$T/r8d2.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (1)" | "$BIN" "$T/r8d2.qry" >/dev/null 2>&1
n=$(printf "SELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r8d2.qry" 2>/dev/null | grep -x "1")
[ "$n" = "1" ] && echo "ok: trailing INSERT without semicolon executes" || { echo "FAIL: R8 D2 insert n=$n"; fails=$((fails + 1)); }
rm -f "$T/r8d2b.qry" "$T/r8d2b.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\nBEGIN;\nINSERT INTO t VALUES (1);\nCOMMIT" | "$BIN" "$T/r8d2b.qry" >/dev/null 2>&1
n=$(printf "SELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r8d2b.qry" 2>/dev/null | grep -x "1")
[ "$n" = "1" ] && echo "ok: trailing COMMIT without semicolon commits" || { echo "FAIL: R8 D2 commit n=$n"; fails=$((fails + 1)); }
out=$(printf "CREATE TABLE t (id INT PRIMARY KEY);\n-- just a comment" | "$BIN" "$T/r8d2c.qry" 2>&1)
case "$out" in *error:*) echo "FAIL: R8 D2 comment tail noisy [$out]"; fails=$((fails + 1));; *) echo "ok: comment-only tail stays silent";; esac
rm -f "$T/r8d2c.qry" "$T/r8d2c.qry.wal"
out=$(printf "CREATE TABLE t (id INT PRIMARY KEY);\n.tables" | "$BIN" "$T/r8d2d.qry" 2>&1)
case "$out" in *"needs a newline"*) echo "ok: trailing dot-command errors honestly";; *) echo "FAIL: R8 D2 dot tail [$out]"; fails=$((fails + 1));; esac
rm -f "$T/r8d2d.qry" "$T/r8d2d.qry.wal"

# R8 D4: pager_open refuse-after-recovery path frees the WAL map (leak
# caught by the sanitize job's leak checker; here we pin the behavior)
rm -f "$T/r8d4.qry" "$T/r8d4.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\n.checkpoint\nINSERT INTO t VALUES (1);\n.crash\n" | "$BIN" "$T/r8d4.qry" >/dev/null 2>&1 || true
: > "$T/r8d4.qry"   # main file lost; WAL holds frames but no catalog page
out=$(printf "SELECT 1;\n" | "$BIN" "$T/r8d4.qry" 2>&1) && rc=0 || rc=$?
case "$out" in
  *"holds only part of the database"*) echo "ok: empty main + catalogless WAL refuses as before";;
  *) echo "FAIL: R8 D4 [$out]"; fails=$((fails + 1));;
esac

# N2 + N6: syscall fault injection via LD_PRELOAD shim (skipped where the
# shim cannot be built, e.g. non-Linux CI without -ldl)
# under ASan the binary must be first in LD_PRELOAD, so the shim cannot be
# preloaded into it; the shim cases run in the normal test job instead
if ldd "$BIN" 2>/dev/null | grep -qi 'asan'; then
  echo "skip: fault-injection shim incompatible with the ASan runtime"
elif cc -shared -fPIC -O2 tests/shim.c -o "$T/shim.so" -ldl 2>/dev/null; then

  # R8 D1: a checkpoint whose WAL header rewrite fails leaves a 0-byte WAL;
  # the next commit must rewrite the header before staging frames, or the
  # acknowledged commit becomes unreadable ("WAL has an unknown format")
  rm -f "$T/r8d1.qry" "$T/r8d1.qry.wal" "$T/r8d1.mark" "$T/r8d1.fifo"
  mkfifo "$T/r8d1.fifo"
  QSHIM=1 QSHIM_MARKER="$T/r8d1.mark" QSHIM_FAIL_WRITE=1 \
    LD_PRELOAD="$T/shim.so" "$BIN" "$T/r8d1.qry" < "$T/r8d1.fifo" > "$T/r8d1.out" 2> "$T/r8d1.err" &
  shim_pid=$!
  { printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (1);\n"; sleep 1
    touch "$T/r8d1.mark"; printf ".checkpoint\n"; sleep 1
    rm "$T/r8d1.mark"; printf "INSERT INTO t VALUES (2);\n.crash\n"; sleep 1; } > "$T/r8d1.fifo"
  wait $shim_pid && rc=0 || rc=$?
  ok=1
  grep -q "WAL header rewrite failed after checkpoint" "$T/r8d1.err" || ok=0
  [ "$(grep -c '1 row inserted.' "$T/r8d1.out")" -eq 2 ] || ok=0
  printf "SELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r8d1.qry" 2>/dev/null | grep -qx "2" || ok=0
  if [ $ok -eq 1 ]; then
    echo "ok: commit after failed checkpoint rewrites WAL header, both rows survive .crash"
  else
    echo "FAIL: R8 D1 (see $T/r8d1.out and $T/r8d1.err)"; fails=$((fails + 1))
  fi

  # R8 D3: a WAL read error at open is reported as IO, not masked as
  # corruption, and the WAL is left byte-identical
  rm -f "$T/r8d3.qry" "$T/r8d3.qry.wal"
  printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (1);\n.crash\n" | "$BIN" "$T/r8d3.qry" >/dev/null 2>&1 || true
  cp "$T/r8d3.qry.wal" "$T/r8d3.wal.before"
  out=$(QSHIM=1 QSHIM_FAIL_READ=1 LD_PRELOAD="$T/shim.so" "$BIN" "$T/r8d3.qry" -c "SELECT 1;" 2>&1) && rc=0 || rc=$?
  if [ $rc -ne 0 ] \
     && printf '%s' "$out" | grep -q "WAL header read failed" \
     && ! printf '%s' "$out" | grep -qi "unknown format\|corrupt" \
     && cmp -s "$T/r8d3.wal.before" "$T/r8d3.qry.wal"; then
    echo "ok: WAL read error at open is IO, WAL untouched"
  else
    echo "FAIL: R8 D3 rc=$rc [$out]"; fails=$((fails + 1))
  fi
  # and it opens fine once the fault clears
  out=$(printf "SELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r8d3.qry" 2>&1)
  case "$out" in *"1"*) echo "ok: WAL replays after the read fault clears";; *) echo "FAIL: R8 D3 recovery [$out]"; fails=$((fails + 1));; esac

  # R9 auto-checkpoint failure: a WAL truncate error at the 2000-frame
  # auto-checkpoint is swallowed (the commit is durable), every later
  # commit retries, and once the fault clears a checkpoint lands. A crash
  # afterwards loses nothing.
  rm -f "$T/r9ack.qry" "$T/r9ack.qry.wal" "$T/r9ack.mark" "$T/r9ack.fifo"
  mkfifo "$T/r9ack.fifo"
  QSHIM=1 QSHIM_MARKER="$T/r9ack.mark" QSHIM_FAIL_TRUNCATE=1 \
    LD_PRELOAD="$T/shim.so" "$BIN" "$T/r9ack.qry" < "$T/r9ack.fifo" > "$T/r9ack.out" 2> "$T/r9ack.err" &
  shim_pid=$!
  { printf "CREATE TABLE t (id INT PRIMARY KEY);\n"
    i=1; while [ $i -le 1990 ]; do printf "INSERT INTO t VALUES ($i);\n"; i=$((i+1)); done
    sleep 1; touch "$T/r9ack.mark"
    while [ $i -le 2290 ]; do printf "INSERT INTO t VALUES ($i);\n"; i=$((i+1)); done
    sleep 1; rm "$T/r9ack.mark"
    while [ $i -le 2340 ]; do printf "INSERT INTO t VALUES ($i);\n"; i=$((i+1)); done
    printf ".crash\n"; sleep 1; } > "$T/r9ack.fifo"
  wait $shim_pid && rc=0 || rc=$?
  nerr=$(grep -c "error:" "$T/r9ack.err" || true)
  nins=$(grep -c "1 row inserted." "$T/r9ack.out" || true)
  n=$(printf "SELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r9ack.qry" 2>/dev/null | grep -x "2340")
  if [ "$nins" = "2340" ] && [ "$nerr" = "0" ] && [ -n "$n" ]; then
    echo "ok: failed auto-checkpoint swallowed, retried, no loss across .crash"
  else
    echo "FAIL: R9 auto-checkpoint (ins=$nins err=$nerr recovered=[$n])"; fails=$((fails + 1))
  fi
  # N2: WAL write fails AND the tail truncate fails: the commit reports an
  # honest error, no success line prints, and later statements in the same
  # session are refused instead of being silently lost
  rm -f "$T/r4n2.qry" "$T/r4n2.qry.wal" "$T/r4n2.mark" "$T/r4n2.fifo"
  mkfifo "$T/r4n2.fifo"
  QSHIM=1 QSHIM_MARKER="$T/r4n2.mark" QSHIM_FAIL_WRITE=1 QSHIM_FAIL_TRUNCATE=1 \
    LD_PRELOAD="$T/shim.so" "$BIN" "$T/r4n2.qry" < "$T/r4n2.fifo" > "$T/r4n2.out" 2> "$T/r4n2.err" &
  shim_pid=$!
  { sleep 1; touch "$T/r4n2.mark"; printf "CREATE TABLE w (id INT PRIMARY KEY);\nINSERT INTO w VALUES (1);\n"; sleep 1; } > "$T/r4n2.fifo"
  wait $shim_pid
  if grep -q "could not be removed" "$T/r4n2.err" \
     && grep -q "pager unusable" "$T/r4n2.err" \
     && ! grep -q "table W created\|row inserted" "$T/r4n2.out"; then
    echo "ok: write+truncate failure breaks the pager, no silent loss"
  else
    echo "FAIL: N2 (see $T/r4n2.out and $T/r4n2.err)"; fails=$((fails + 1))
  fi
  out=$(printf ".tables\nCREATE TABLE q (id INT PRIMARY KEY);\nINSERT INTO q VALUES (1);\nSELECT COUNT(*) FROM q;\n" | "$BIN" "$T/r4n2.qry" 2>&1)
  case "$out" in
    *"(no tables)"*"table Q created."*"1"*) echo "ok: reopen after broken commit recovers cleanly";;
    *) echo "FAIL: reopen after broken commit: [$out]"; fails=$((fails + 1));;
  esac

  # N6: a db_close failure exits 1 and says why on stderr
  rm -f "$T/r4n6.qry" "$T/r4n6.qry.wal" "$T/r4n6.mark" "$T/r4n6.fifo"
  mkfifo "$T/r4n6.fifo"
  QSHIM=1 QSHIM_MARKER="$T/r4n6.mark" QSHIM_FAIL_TRUNCATE=1 \
    LD_PRELOAD="$T/shim.so" "$BIN" "$T/r4n6.qry" < "$T/r4n6.fifo" > "$T/r4n6.out" 2> "$T/r4n6.err" &
  shim_pid=$!
  { printf "CREATE TABLE z (id INT PRIMARY KEY);\n"; sleep 1; touch "$T/r4n6.mark"; } > "$T/r4n6.fifo"
  wait $shim_pid && rc=0 || rc=$?
  if [ $rc -eq 1 ] && grep -q "error:" "$T/r4n6.err"; then
    echo "ok: db_close failure exits 1 with a stderr message"
  else
    echo "FAIL: db_close failure silent (rc=$rc, see $T/r4n6.err)"; fails=$((fails + 1))
  fi

  # R5 F-B: a failed auto-commit (WAL write error) must not wedge the
  # session: the uncommitted row disappears, the pager leaves its txn, and
  # later statements work once the fault clears
  rm -f "$T/r5fb.qry" "$T/r5fb.qry.wal" "$T/r5fb.mark" "$T/r5fb.fifo"
  mkfifo "$T/r5fb.fifo"
  QSHIM=1 QSHIM_MARKER="$T/r5fb.mark" QSHIM_FAIL_WRITE=1 \
    LD_PRELOAD="$T/shim.so" "$BIN" "$T/r5fb.qry" < "$T/r5fb.fifo" > "$T/r5fb.out" 2> "$T/r5fb.err" &
  shim_pid=$!
  { printf "CREATE TABLE t (id INT PRIMARY KEY);\n"; sleep 1
    touch "$T/r5fb.mark"
    printf "INSERT INTO t VALUES (1);\n"; sleep 1
    rm "$T/r5fb.mark"
    printf "SELECT COUNT(*) FROM t;\nINSERT INTO t VALUES (2);\nSELECT COUNT(*) FROM t;\n"; sleep 1; } > "$T/r5fb.fifo"
  wait $shim_pid && rc=0 || rc=$?
  if grep -q "error: WAL write failed" "$T/r5fb.err" \
     && [ "$(grep -c 'error:' "$T/r5fb.err")" -eq 1 ] \
     && grep -q "1 row inserted." "$T/r5fb.out" \
     && [ "$(grep -A1 '^count' "$T/r5fb.out" | grep -c '^0$')" -eq 1 ] \
     && [ "$(grep -A1 '^count' "$T/r5fb.out" | grep -c '^1$')" -eq 1 ]; then
    echo "ok: failed auto-commit rolls back, session stays usable"
  else
    echo "FAIL: R5 F-B (see $T/r5fb.out and $T/r5fb.err)"; fails=$((fails + 1))
  fi

  # R5 F-C: checkpoint checks data fsync and the WAL header rewrite + fsync.
  # Each fault must produce an honest error (no "checkpoint complete."),
  # leave the database readable, and let a later checkpoint succeed.
  for mode in MAIN_FSYNC WRITE FSYNC; do
    rm -f "$T/r5fc.qry" "$T/r5fc.qry.wal" "$T/r5fc.mark" "$T/r5fc.fifo"
    mkfifo "$T/r5fc.fifo"
    env QSHIM=1 QSHIM_MARKER="$T/r5fc.mark" "QSHIM_FAIL_$mode=1" \
      LD_PRELOAD="$T/shim.so" "$BIN" "$T/r5fc.qry" < "$T/r5fc.fifo" > "$T/r5fc.out" 2> "$T/r5fc.err" &
    shim_pid=$!
    { printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (1);\n"; sleep 1
      touch "$T/r5fc.mark"
      printf ".checkpoint\n"; sleep 1
      rm "$T/r5fc.mark"
      printf "SELECT COUNT(*) FROM t;\n.checkpoint\n"; sleep 1; } > "$T/r5fc.fifo"
    wait $shim_pid && rc=0 || rc=$?
    if grep -q "error: " "$T/r5fc.err" \
       && [ "$(grep -c 'checkpoint complete.' "$T/r5fc.out")" -eq 1 ] \
       && grep -A1 '^count' "$T/r5fc.out" | grep -q '^1$'; then
      echo "ok: checkpoint failure ($mode) is reported, no silent success"
    else
      echo "FAIL: R5 F-C $mode (see $T/r5fc.out and $T/r5fc.err)"; fails=$((fails + 1))
    fi
    out=$(printf "SELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r5fc.qry" 2>&1)
    case "$out" in
      *"1"*) echo "ok: data survives a failed checkpoint ($mode)";;
      *) echo "FAIL: data lost after failed checkpoint $mode: [$out]"; fails=$((fails + 1));;
    esac
  done
else
  echo "skip: fault-injection shim not buildable here"
fi

# R6 F1: empty main file + non-empty WAL is never silently truncated.
# When the WAL carries the catalog page, open replays it and recovers.
rm -f "$T/r6f1.qry" "$T/r6f1.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (42);\n.crash\n" | "$BIN" "$T/r6f1.qry" >/dev/null 2>&1 || true
truncate -s 0 "$T/r6f1.qry"
out=$(printf "SELECT id FROM t;\n" | "$BIN" "$T/r6f1.qry" 2>&1)
case "$out" in
  *"42"*) echo "ok: empty main file + WAL replays committed data";;
  *) echo "FAIL: R6 F1 replay: [$out]"; fails=$((fails + 1));;
esac

# When the WAL's committed frames lack the catalog page, open refuses and
# leaves the WAL untouched; removing the WAL by hand starts fresh.
rm -f "$T/r6f1b.qry" "$T/r6f1b.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (1);\n.checkpoint\nINSERT INTO t VALUES (2);\n.crash\n" | "$BIN" "$T/r6f1b.qry" >/dev/null 2>&1 || true
truncate -s 0 "$T/r6f1b.qry"
walbefore=$(stat -c %s "$T/r6f1b.qry.wal")
out=$(printf "SELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r6f1b.qry" 2>&1 || true)
walafter=$(stat -c %s "$T/r6f1b.qry.wal")
case "$out" in
  *"refusing to destroy"*) : ;;
  *) echo "FAIL: R6 F1 refuse: [$out]"; fails=$((fails + 1));;
esac
if [ "$walbefore" != "$walafter" ]; then
  echo "FAIL: R6 F1 WAL modified on refused open ($walbefore -> $walafter)"; fails=$((fails + 1))
fi
rm "$T/r6f1b.qry.wal"
out=$(printf "SELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r6f1b.qry" 2>&1 || true)
case "$out" in
  *"no table named T"*) echo "ok: refused open preserves the WAL, manual reset works";;
  *) echo "FAIL: R6 F1 manual reset: [$out]"; fails=$((fails + 1));;
esac

# R7: empty main + WAL whose committed frames are all corrupt (bit-rot):
# refuse, and leave the WAL byte-identical - never reinitialize over it
rm -f "$T/r7f1.qry" "$T/r7f1.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (42);\n.crash\n" | "$BIN" "$T/r7f1.qry" >/dev/null 2>&1 || true
truncate -s 0 "$T/r7f1.qry"
python3 -c "
d = open('$T/r7f1.qry.wal', 'r+b'); d.seek(16 + 4); b = d.read(1)[0]
d.seek(16 + 4); d.write(bytes([b ^ 0xFF])); d.close()"
m1=$(md5sum "$T/r7f1.qry.wal" | cut -d' ' -f1)
out=$(printf ".tables\n" | "$BIN" "$T/r7f1.qry" 2>&1 || true)
m2=$(md5sum "$T/r7f1.qry.wal" | cut -d' ' -f1)
case "$out" in
  *"no recoverable committed data"*) : ;;
  *) echo "FAIL: R7 bit-rot: [$out]"; fails=$((fails + 1));;
esac
[ "$m1" = "$m2" ] || { echo "FAIL: R7 bit-rot WAL modified"; fails=$((fails + 1)); }
[ "$m1" = "$m2" ] && echo "ok: bit-rotted WAL refused, bytes preserved"

# R7/R9: a sub-16-byte WAL provably holds no frame (frames start at offset
# 16 and are 4120 bytes), so a torn header is reinitialized, never a
# refusal: empty main starts fresh, intact main just opens
rm -f "$T/r7torn.qry" "$T/r7torn.qry.wal"
touch "$T/r7torn.qry"; printf 'ABCDEFGH' > "$T/r7torn.qry.wal"
out=$(printf ".tables\n" | "$BIN" "$T/r7torn.qry" 2>&1 || true)
case "$out" in
  *"(no tables)"*) echo "ok: torn WAL header reinitialized, empty main starts fresh";;
  *) echo "FAIL: R9 torn header: [$out]"; fails=$((fails + 1));;
esac
[ "$(stat -c %s "$T/r7torn.qry.wal")" = "16" ] || { echo "FAIL: R9 torn WAL not reinitialized to a bare header"; fails=$((fails + 1)); }
rm -f "$T/r9torn2.qry" "$T/r9torn2.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (1);\n.checkpoint\n" | "$BIN" "$T/r9torn2.qry" >/dev/null 2>&1
truncate -s 8 "$T/r9torn2.qry.wal"
n=$(printf "SELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r9torn2.qry" 2>/dev/null | grep -cx "1")
[ "$n" = "1" ] && echo "ok: torn WAL header with intact main opens with data" || { echo "FAIL: R9 torn header + intact main"; fails=$((fails + 1)); }

# R9 D9-2: a partial main file (1-4095 bytes) attempts WAL replay exactly
# like an empty one
rm -f "$T/r9part.qry" "$T/r9part.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (7);\n.crash\n" | "$BIN" "$T/r9part.qry" >/dev/null 2>&1 || true
truncate -s 100 "$T/r9part.qry"
n=$(printf "SELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r9part.qry" 2>/dev/null | grep -cx "1")
[ "$n" = "1" ] && echo "ok: partial main replays the WAL" || { echo "FAIL: R9 partial main"; fails=$((fails + 1)); }

# R9 D9-3: replay requires EVERY page in the WAL map; a sparse WAL refuses
# with the file byte-identical
rm -f "$T/r9sparse.qry" "$T/r9sparse.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (1);\n.checkpoint\nCREATE TABLE u (id INT PRIMARY KEY);\nINSERT INTO u VALUES (2);\n.crash\n" | "$BIN" "$T/r9sparse.qry" >/dev/null 2>&1 || true
rm "$T/r9sparse.qry"
m1=$(md5sum "$T/r9sparse.qry.wal" | cut -d' ' -f1)
out=$(printf ".tables\n" | "$BIN" "$T/r9sparse.qry" 2>&1 || true)
m2=$(md5sum "$T/r9sparse.qry.wal" | cut -d' ' -f1)
case "$out" in
  *"holds only part of the database"*) [ "$m1" = "$m2" ] && echo "ok: sparse WAL refuses, evidence preserved" || { echo "FAIL: R9 sparse WAL modified"; fails=$((fails + 1)); };;
  *) echo "FAIL: R9 sparse replay: [$out]"; fails=$((fails + 1));;
esac

# R9: the unknown-format refusal now says how to recover
rm -f "$T/r9unk.qry" "$T/r9unk.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (1);\n.checkpoint\n" | "$BIN" "$T/r9unk.qry" >/dev/null 2>&1
printf 'NOTQUARRYFORMAT!' | dd of="$T/r9unk.qry.wal" bs=1 count=16 conv=notrunc 2>/dev/null
out=$(printf ".tables\n" | "$BIN" "$T/r9unk.qry" 2>&1 || true)
case "$out" in
  *"unknown format"*"remove the .wal file"*) echo "ok: unknown-format refusal carries recovery guidance";;
  *) echo "FAIL: R9 unknown-format guidance: [$out]"; fails=$((fails + 1));;
esac


# R7: a refused open does not even trim garbage trailing the last commit
rm -f "$T/r7tail.qry" "$T/r7tail.qry.wal"
printf "CREATE TABLE t (id INT PRIMARY KEY);\nINSERT INTO t VALUES (1);\n.checkpoint\nINSERT INTO t VALUES (2);\n.crash\n" | "$BIN" "$T/r7tail.qry" >/dev/null 2>&1 || true
truncate -s 0 "$T/r7tail.qry"
printf 'GARBAGEGARBAGE' >> "$T/r7tail.qry.wal"
m1=$(md5sum "$T/r7tail.qry.wal" | cut -d' ' -f1)
out=$(printf ".tables\n" | "$BIN" "$T/r7tail.qry" 2>&1 || true)
m2=$(md5sum "$T/r7tail.qry.wal" | cut -d' ' -f1)
case "$out" in
  *"refusing to destroy"*) : ;;
  *) echo "FAIL: R7 garbage tail: [$out]"; fails=$((fails + 1));;
esac
if [ "$m1" = "$m2" ]; then echo "ok: refused open leaves garbage tail intact"
else echo "FAIL: R7 garbage tail trimmed on refused open"; fails=$((fails + 1)); fi

# R7: a flock failure that is NOT contention (e.g. ENOLCK on a network
# filesystem) says what really failed instead of claiming contention
rm -f "$T/r7lock.qry" "$T/r7lock.qry.wal" "$T/r7lock.mark"
if ldd "$BIN" 2>/dev/null | grep -qi 'asan'; then
  echo "skip: fault-injection shim incompatible with the ASan runtime"
elif cc -shared -fPIC -O2 tests/shim.c -o "$T/shim.so" -ldl 2>/dev/null; then
  out=$(printf ".tables\n" | QSHIM=1 QSHIM_FAIL_FLOCK=1 LD_PRELOAD="$T/shim.so" "$BIN" "$T/r7lock.qry" 2>&1 || true)
  case "$out" in
    *"cannot lock database file:"*"No locks available"*) echo "ok: non-contention lock failure is honest";;
    *"cannot lock database file:"*) echo "ok: non-contention lock failure is honest";;
    *) echo "FAIL: R7 flock errno: [$out]"; fails=$((fails + 1));;
  esac
else
  echo "skip: fault-injection shim not buildable here"
fi

# R6 F2: a second process is refused with an honest lock error
rm -f "$T/r6f2.qry" "$T/r6f2.qry.wal" "$T/r6f2.fifo"
mkfifo "$T/r6f2.fifo"
"$BIN" "$T/r6f2.qry" < "$T/r6f2.fifo" > "$T/r6f2.out" 2>&1 &
lock_pid=$!
{ printf "CREATE TABLE t (id INT PRIMARY KEY);\n"; sleep 2; } > "$T/r6f2.fifo" &
sleep 1
out=$(printf "SELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r6f2.qry" 2>&1 || true)
case "$out" in
  *"database is locked"*) echo "ok: second opener refused with a lock error";;
  *) echo "FAIL: R6 F2: [$out]"; fails=$((fails + 1));;
esac
wait $lock_pid 2>/dev/null || true
out=$(printf "INSERT INTO t VALUES (1);\nSELECT COUNT(*) FROM t;\n" | "$BIN" "$T/r6f2.qry" 2>&1)
case "$out" in
  *"1 row inserted."*"1"*) echo "ok: lock released at close";;
  *) echo "FAIL: R6 F2 release: [$out]"; fails=$((fails + 1));;
esac

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
