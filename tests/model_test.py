#!/usr/bin/env python3
"""Differential test: run identical random workloads against quarry and
sqlite3, then compare the full ordered contents of every table.

The workload mixes inserts (some known duplicates, some NULL fields),
updates (including SET col = NULL), deletes, and explicit transactions
with both COMMIT and ROLLBACK - including failed statements inside an
open transaction, which both engines must absorb without state drift."""
import random, sqlite3, subprocess, sys, tempfile, os

BIN = os.path.join(os.path.dirname(__file__), "..", "build", "quarry")
ROLES = ["engineer", "manager", "intern", "designer", "founder"]

def lit(v):
    if v is None:
        return "NULL"
    if isinstance(v, int):
        return str(v)
    return "'%s'" % v

def run_seed(seed, nops):
    rng = random.Random(seed)
    ql = ["CREATE TABLE people (id INT PRIMARY KEY, name TEXT, role TEXT, age INT);"]
    ref = sqlite3.connect(":memory:")
    ref.isolation_level = None  # autocommit; explicit BEGIN/COMMIT below
    ref.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, role TEXT, age INTEGER)")
    live = set()
    in_txn = False
    journal = []  # ("add"|"del", key) applied to `live` since BEGIN
    for _ in range(nops):
        op = rng.random()
        if not in_txn and op < 0.06:
            in_txn = True
            journal = []
            ql.append("BEGIN;")
            ref.execute("BEGIN")
            continue
        if in_txn and op < 0.12:
            if rng.random() < 0.75:
                ql.append("COMMIT;")
                ref.execute("COMMIT")
            else:
                ql.append("ROLLBACK;")
                ref.execute("ROLLBACK")
                for act, k in reversed(journal):
                    if act == "add":
                        live.discard(k)
                    else:
                        live.add(k)
            in_txn = False
            journal = []
            continue
        if op < 0.55 or not live:  # insert (sometimes a known duplicate)
            k = rng.randrange(1, 4000)
            name = None if rng.random() < 0.1 else f"n{k}_{seed}"
            role = rng.choice(ROLES + [None])
            age = None if rng.random() < 0.1 else rng.randrange(18, 70)
            ql.append(f"INSERT INTO people VALUES ({k}, {lit(name)}, {lit(role)}, {lit(age)});")
            if k in live:
                # both engines reject the duplicate and carry on
                try:
                    ref.execute("INSERT INTO people VALUES (?,?,?,?)",
                                (k, name, role, age))
                except sqlite3.IntegrityError:
                    pass
            else:
                ref.execute("INSERT INTO people VALUES (?,?,?,?)",
                            (k, name, role, age))
                live.add(k)
                if in_txn:
                    journal.append(("add", k))
        elif op < 0.75:  # update, sometimes to NULL
            k = rng.choice(sorted(live))
            age = None if rng.random() < 0.15 else rng.randrange(18, 70)
            ref.execute("UPDATE people SET age=? WHERE id=?", (age, k))
            ql.append(f"UPDATE people SET age = {lit(age)} WHERE id = {k};")
        else:  # delete
            k = rng.choice(sorted(live))
            live.discard(k)
            ref.execute("DELETE FROM people WHERE id=?", (k,))
            ql.append(f"DELETE FROM people WHERE id = {k};")
            if in_txn:
                journal.append(("del", k))
    if in_txn:
        ql.append("COMMIT;")
        ref.execute("COMMIT")
    script = "\n".join(ql)
    with tempfile.TemporaryDirectory() as td:
        db = os.path.join(td, "m.qry")
        p = subprocess.run([BIN, db], input=script, text=True,
                           errors="replace", capture_output=True)
        if p.returncode not in (0,):
            return f"seed {seed}: quarry exited {p.returncode}: {p.stderr[:400]}"
        # dump in key order
        p = subprocess.run([BIN, db, "-c", "SELECT id, name, role, age FROM people ORDER BY id;"],
                           text=True, errors="replace", capture_output=True)
    def pv(s, is_int):
        s = s.strip()
        return None if s == "NULL" else (int(s) if is_int else s)
    got = []
    for line in p.stdout.splitlines()[1:]:
        if line.startswith("("):
            break
        parts = [x.strip() for x in line.split("|")]
        got.append((int(parts[0]), pv(parts[1], False), pv(parts[2], False), pv(parts[3], True)))
    want = [(r[0], r[1], r[2], r[3]) for r in ref.execute("SELECT * FROM people ORDER BY id")]
    if got != want:
        for g, w in zip(got, want):
            if g != w:
                return f"seed {seed}: mismatch quarry={g} sqlite={w}"
        return f"seed {seed}: length mismatch quarry={len(got)} sqlite={len(want)}"
    return None

def run_txn_scenario():
    """Rollback must resync the in-memory catalog: a rolled-back CREATE
    TABLE and roots reverted by ROLLBACK (here via 200 in-txn inserts that
    force leaf/root splits) must not leak into later writes."""
    script = ["CREATE TABLE a (id INT PRIMARY KEY, v INT);",
              "INSERT INTO a VALUES (1, 1);", "INSERT INTO a VALUES (2, 2);",
              "BEGIN;"]
    script += [f"INSERT INTO a VALUES ({i + 3}, {i});" for i in range(200)]
    script += ["ROLLBACK;",
               "CREATE TABLE b (id INT PRIMARY KEY, w INT);"]
    script += [f"INSERT INTO b VALUES ({i}, {i});" for i in range(1, 400)]
    script += ["BEGIN;", "CREATE TABLE x (id INT PRIMARY KEY);",
               "INSERT INTO x VALUES (5);", "ROLLBACK;",
               "CREATE TABLE x (id INT PRIMARY KEY);",
               "INSERT INTO x VALUES (7);", "INSERT INTO x VALUES (8);"]
    with tempfile.TemporaryDirectory() as td:
        db = os.path.join(td, "txn.qry")
        p = subprocess.run([BIN, db], input="\n".join(script) + "\n",
                           text=True, capture_output=True)
        if p.returncode != 0:
            return f"txn scenario: quarry exited {p.returncode}: {p.stderr.strip()[:200]}"
        ref = sqlite3.connect(":memory:")
        ref.isolation_level = None
        for line in script:
            ref.execute(line.rstrip(";"))
        for t in ("a", "b", "x"):
            q = subprocess.run([BIN, db, "-c", f"SELECT * FROM {t} ORDER BY id;"],
                               text=True, capture_output=True)
            got = []
            for ln in q.stdout.splitlines()[1:]:
                if ln.startswith("("):
                    break
                got.append(tuple(x.strip() for x in ln.split("|")))
            want = [tuple(str(c) for c in r)
                    for r in ref.execute(f"SELECT * FROM {t} ORDER BY id")]
            if got != want:
                return (f"txn scenario: table {t} mismatch "
                        f"quarry={got[:3]}({len(got)}) sqlite={want[:3]}({len(want)})")
    return None

fails = 0
err = run_txn_scenario()
if err:
    print(err)
    fails += 1
for seed in range(1, 9):
    err = run_seed(seed, 600)
    if err:
        print(err)
        fails += 1
if fails:
    print(f"MODEL TEST FAILED: {fails}/8 seeds")
    sys.exit(1)
print("model test: 8 seeds x 600 ops (NULLs, txns, failed statements) match sqlite3 exactly")
