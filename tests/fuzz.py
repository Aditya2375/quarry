#!/usr/bin/env python3
"""quarry fuzzer: garbage SQL text and on-disk corruption.

Every round must end gracefully: exit 0 (ok) or 1 (handled error).
A signal, a timeout (hang), sanitizer output, or an unexpected exit
code is a bug.

Rounds are tunable for CI vs. local runs:
  QUARRY_FUZZ_SQL   garbage-SQL rounds   (default 300)
  QUARRY_FUZZ_DISK  corruption rounds    (default 200)
  QUARRY_FUZZ_WAL   WAL-mutation rounds  (default 150)
  QUARRY_FUZZ_SEED  base seed            (default 1234)
"""
import os
import random
import string
import subprocess
import sys

BIN = "build/quarry"
SQL_ROUNDS = int(os.environ.get("QUARRY_FUZZ_SQL", "300"))
DISK_ROUNDS = int(os.environ.get("QUARRY_FUZZ_DISK", "200"))
WAL_ROUNDS = int(os.environ.get("QUARRY_FUZZ_WAL", "150"))
TIMEOUT = 10

# sanitizer stacks do not change the exit code reliably, so scan output
SANITIZER_MARKS = ("AddressSanitizer", "LeakSanitizer",
                   "UndefinedBehaviorSanitizer", "runtime error:",
                   "MemorySanitizer", "SUMMARY: Sanitizer")


def run(db, stdin, timeout=TIMEOUT):
    try:
        # errors="replace": mutated files can make the engine echo bytes that
        # are not valid UTF-8; that must not kill the harness itself
        return subprocess.run([BIN, db], input=stdin, text=True,
                              errors="replace", capture_output=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        raise AssertionError("hung on input: %r" % stdin[:120])


def check(r, tag):
    blob = (r.stderr or "") + "\n" + (r.stdout or "")
    for m in SANITIZER_MARKS:
        assert m not in blob, "%s: sanitizer output: %r" % (tag, blob[:400])
    assert r.returncode in (0, 1), "%s: exit %r, stderr %r" % (
        tag, r.returncode, r.stderr[:200])


WORDS = ["SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "CREATE",
         "TABLE", "UPDATE", "DELETE", "BEGIN", "COMMIT", "ROLLBACK", "ORDER",
         "BY", "LIMIT", "COUNT", "AND", "NULL", "INT", "TEXT", "PRIMARY",
         "KEY", "SET", "t", "x", "id", "v", "1", "42", "-7", "'a'", "''",
         "(", ")", ",", ";", "=", "!=", "<", ">=", "*", "9223372036854775808"]


def garbage_sql(rng):
    lines = []
    for _ in range(rng.randint(1, 12)):
        n = rng.randint(0, 30)
        if rng.random() < 0.5:
            line = " ".join(rng.choice(WORDS) for _ in range(n))
        else:
            # raw byte-ish noise; never start a line with '.' (dot commands)
            line = "".join(rng.choice(string.printable.replace(".", "x"))
                           for _ in range(n))
        if line.lstrip().startswith("."):
            line = "x" + line
        lines.append(line)
    return "\n".join(lines) + "\n"


def validate(db, tag, expect_ok):
    # structural validator over every B+tree in the database; must never
    # crash or hang, and a healthy database must always validate clean
    try:
        r = subprocess.run([BIN, db, "-validate"], text=True, errors="replace",
                           capture_output=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        raise AssertionError("%s: validator hung" % tag)
    check(r, tag)
    if expect_ok:
        assert r.returncode == 0 and "validate: ok" in r.stdout, \
            "%s: validator rejected a healthy database: %r" % (
                tag, ((r.stdout or "") + (r.stderr or ""))[:300])

def fuzz_sql(rng, tmp):
    for i in range(SQL_ROUNDS):
        db = os.path.join(tmp, "fuzzsql.qry")
        if i % 3 == 0:  # sometimes pre-seed a table to bounce off real state
            subprocess.run([BIN, db], input="CREATE TABLE t (id INT PRIMARY KEY, v TEXT);\n",
                           text=True, capture_output=True)
        check(run(db, garbage_sql(rng)), "sql round %d" % i)
        if i % 10 == 0:
            validate(db, "sql round %d" % i, True)
    print("ok: fuzz: %d rounds of garbage SQL, no crash or hang" % SQL_ROUNDS)


def corrupt_targeted(rng, db, size):
    """Aim at the fields the engine trusts most: page type, cell count,
    used-bytes, sibling link. Uniform random bytes almost never produce
    interesting values in exactly these spots."""
    npages = size // 4096
    if npages < 2:
        return
    with open(db, "r+b") as f:
        for _ in range(rng.randint(1, 6)):
            pgno = rng.randrange(1, npages)
            base = pgno * 4096
            field = rng.choice(["type", "ncells", "used", "link"])
            if field == "type":
                f.seek(base)
                f.write(bytes([rng.choice([0, 3, 9, 255])]))
            elif field == "ncells":
                f.seek(base + 1)
                f.write(rng.choice([b"\xff\xff", b"\x00\x00", b"\x01\x00",
                                    b"\x00\x80"]))
            elif field == "used":
                f.seek(base + 3)
                f.write(rng.choice([b"\x00\x00\x00\x00", b"\xff\xff\xff\xff",
                                    (16).to_bytes(4, "little"),
                                    (4096).to_bytes(4, "little")]))
            else:  # link: self-cycle, huge, zero, another leaf, an
                # interior page, or a page whose type was scribbled
                target = rng.choice([pgno, 0xFFFFFFFF, 1,
                                     rng.randrange(1, npages),
                                     rng.randrange(1, npages)])
                f.seek(base + 7)
                f.write(target.to_bytes(4, "little"))


def fuzz_disk(rng, tmp):
    db = os.path.join(tmp, "fuzzdisk.qry")
    for i in range(DISK_ROUNDS):
        for f in (db, db + ".wal"):
            if os.path.exists(f):
                os.unlink(f)
        # most rounds stay small (single leaf); the rest grow past several
        # levels of interior pages so corruption can hit interior cells.
        # A few rounds go deep: wide rows force 300+ leaves, which splits
        # interior pages and reaches depth 3.
        roll = rng.random()
        if roll < 0.6:
            nrows, wide = rng.randint(1, 120), False
        elif roll < 0.95:
            nrows, wide = rng.randint(121, 1500), False
        else:
            nrows, wide = 13000, True
        pk = rng.random() < 0.7
        if pk:
            seed = "CREATE TABLE t (id INT PRIMARY KEY, v TEXT);\n" + "".join(
                "INSERT INTO t VALUES (%d, 'r%d');\n" % (k, k)
                for k in range(nrows))
        else:  # auto-keyed table: exercises the non-PK insert path
            v = "w" * 100 if wide else "r"
            seed = "CREATE TABLE t (v TEXT);\n" + "".join(
                "INSERT INTO t VALUES ('%s%d');\n" % (v, k)
                for k in range(nrows))
        r = subprocess.run([BIN, db], input=seed, text=True,
                           errors="replace", capture_output=True)
        assert r.returncode == 0, r.stderr
        if i % 5 == 0:  # the freshly seeded tree must always validate
            validate(db, "disk round %d seeded" % i, True)
        size = os.path.getsize(db)
        mode = rng.random()
        if mode < 0.45:  # flip random bytes
            with open(db, "r+b") as f:
                for _ in range(rng.randint(1, 24)):
                    f.seek(rng.randrange(size))
                    f.write(bytes([rng.randrange(256)]))
        elif mode < 0.75:  # targeted page-header corruption
            corrupt_targeted(rng, db, size)
        elif mode < 0.9:  # truncate
            with open(db, "r+b") as f:
                f.truncate(rng.randrange(size + 1))
        else:  # scribble over a whole page
            with open(db, "r+b") as f:
                f.seek(4096 * rng.randrange(size // 4096))
                f.write(bytes(rng.randrange(256) for _ in range(4096)))
        # probe reads AND writes: a cyclic or mis-linked interior page must
        # be a clean corruption error on INSERT/UPDATE/DELETE too, never a
        # crash or a hang
        if pk:
            probe = ("SELECT COUNT(*) FROM t;\n.tables\n"
                     "INSERT INTO t VALUES (2000001, 'probe');\n"
                     "UPDATE t SET v='probe' WHERE id=3;\n"
                     "DELETE FROM t WHERE id=4;\n"
                     "INSERT INTO t VALUES (2000002, 'probe2');\n"
                     "SELECT COUNT(*) FROM t;\n")
        else:
            probe = ("SELECT COUNT(*) FROM t;\n.tables\n"
                     "INSERT INTO t VALUES ('probe');\n"
                     "UPDATE t SET v='probe' WHERE v='r3';\n"
                     "DELETE FROM t WHERE v='r4';\n"
                     "INSERT INTO t VALUES ('probe2');\n"
                     "SELECT COUNT(*) FROM t;\n")
        check(run(db, probe), "disk round %d" % i)
        # corrupted images may validate or fail; both must be clean outcomes
        validate(db, "disk round %d corrupted" % i, False)
    print("ok: fuzz: %d rounds of on-disk corruption, no crash or hang" % DISK_ROUNDS)


def fuzz_wal(rng, tmp):
    """Mutate a populated WAL (bit flips, scribbles, truncations, header
    tears, frame surgery). Invariants every round: no crash, no sanitizer
    output, a refusal names the WAL problem, and a successful open shows
    exactly the checkpointed rows or exactly all rows - a silent partial
    set means acknowledged commits vanished."""
    base = os.path.join(tmp, "walbase.qry")
    r = subprocess.run([BIN, base], input=
        "CREATE TABLE kv (k INT PRIMARY KEY, v TEXT);\n"
        + "".join("INSERT INTO kv VALUES (%d, 'a');" % i for i in range(50))
        + ".checkpoint\n", text=True, capture_output=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([BIN, base], input="BEGIN;\n"
        + "".join("INSERT INTO kv VALUES (%d, 'b');" % i for i in range(50, 100))
        + "COMMIT;\n.crash\n", text=True, capture_output=True)
    assert os.path.getsize(base + ".wal") > 16, "WAL not populated"

    def mutate(w):
        kind = rng.randrange(6)
        if kind == 0:    # bit flip at a random offset
            i = rng.randrange(len(w)); w[i] ^= 1 << rng.randrange(8)
        elif kind == 1:  # scribble a random run
            i = rng.randrange(len(w)); n = rng.randrange(1, min(64, len(w) - i) + 1)
            w[i:i+n] = bytes(rng.randrange(256) for _ in range(n))
        elif kind == 2:  # truncate to a random length
            del w[rng.randrange(len(w) + 1):]
        elif kind == 3:  # tear the header to 1-15 bytes
            del w[rng.randrange(1, 16):]
        elif kind == 4:  # garbage over the header
            w[:16] = bytes(rng.randrange(256) for _ in range(16))
        elif len(w) > 16:  # scribble inside one frame
            i = rng.randrange(16, len(w)); n = rng.randrange(1, min(32, len(w) - i) + 1)
            w[i:i+n] = bytes(rng.randrange(256) for _ in range(n))

    for i in range(WAL_ROUNDS):
        db = os.path.join(tmp, "walmut.qry")
        for f in (db, db + ".wal"):
            if os.path.exists(f):
                os.unlink(f)
        import shutil
        shutil.copyfile(base, db)
        wal = bytearray(open(base + ".wal", "rb").read())
        mutate(wal)
        open(db + ".wal", "wb").write(wal)
        r = run(db, "SELECT k FROM kv;\n")
        check(r, "wal round %d" % i)
        if r.returncode == 1:
            blob = (r.stderr or "") + (r.stdout or "")
            assert ".wal file" in blob or "WAL" in blob, \
                "wal round %d: refusal without WAL context: %r" % (i, blob[:200])
            continue
        rows = set()
        for line in (r.stdout or "").splitlines():
            line = line.strip()
            if line.isdigit():
                rows.add(int(line))
        assert rows == set(range(50)) or rows == set(range(100)), \
            "wal round %d: silent partial set (%d rows)" % (i, len(rows))
    print("ok: fuzz: %d rounds of WAL mutation, invariants held" % WAL_ROUNDS)


def main():
    base = int(os.environ.get("QUARRY_FUZZ_SEED", "1234"))
    tmp = "/tmp/quarry-fuzz"
    os.makedirs(tmp, exist_ok=True)
    for f in os.listdir(tmp):
        os.unlink(os.path.join(tmp, f))
    # a fresh sub-seed per phase so tweaking round counts still explores
    # new paths instead of replaying one stream prefix
    fuzz_sql(random.Random(base), tmp)
    fuzz_disk(random.Random(base ^ 0x5EED), tmp)
    fuzz_wal(random.Random(base ^ 0x9A11), tmp)


if __name__ == "__main__":
    main()
