#!/usr/bin/env python3
"""quarry fuzzer: garbage SQL text and on-disk corruption.

Every round must end gracefully: exit 0 (ok) or 1 (handled error).
A signal, a timeout (hang), sanitizer output, or an unexpected exit
code is a bug.

Rounds are tunable for CI vs. local runs:
  QUARRY_FUZZ_SQL   garbage-SQL rounds   (default 300)
  QUARRY_FUZZ_DISK  corruption rounds    (default 200)
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


def fuzz_sql(rng, tmp):
    for i in range(SQL_ROUNDS):
        db = os.path.join(tmp, "fuzzsql.qry")
        if i % 3 == 0:  # sometimes pre-seed a table to bounce off real state
            subprocess.run([BIN, db], input="CREATE TABLE t (id INT PRIMARY KEY, v TEXT);\n",
                           text=True, capture_output=True)
        check(run(db, garbage_sql(rng)), "sql round %d" % i)
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
            else:  # link: self-cycle, huge, or zero
                f.seek(base + 7)
                f.write(rng.choice([pgno.to_bytes(4, "little"),
                                    b"\xff\xff\xff\xff",
                                    (1).to_bytes(4, "little")]))


def fuzz_disk(rng, tmp):
    db = os.path.join(tmp, "fuzzdisk.qry")
    for i in range(DISK_ROUNDS):
        for f in (db, db + ".wal"):
            if os.path.exists(f):
                os.unlink(f)
        seed = "CREATE TABLE t (id INT PRIMARY KEY, v TEXT);\n" + "".join(
            "INSERT INTO t VALUES (%d, 'r%d');\n" % (k, k)
            for k in range(rng.randint(1, 120)))
        r = subprocess.run([BIN, db], input=seed, text=True,
                           errors="replace", capture_output=True)
        assert r.returncode == 0, r.stderr
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
        check(run(db, "SELECT COUNT(*) FROM t;\n.tables\n"), "disk round %d" % i)
    print("ok: fuzz: %d rounds of on-disk corruption, no crash or hang" % DISK_ROUNDS)


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


if __name__ == "__main__":
    main()
