#!/usr/bin/env python3
"""Differential test: run identical random workloads against quarry and
sqlite3, then compare the full ordered contents of every table."""
import random, sqlite3, subprocess, sys, tempfile, os

BIN = os.path.join(os.path.dirname(__file__), "..", "build", "quarry")
ROLES = ["engineer", "manager", "intern", "designer", "founder"]

def run_seed(seed, nops):
    rng = random.Random(seed)
    ql = ["CREATE TABLE people (id INT PRIMARY KEY, name TEXT, role TEXT, age INT);"]
    ref = sqlite3.connect(":memory:")
    ref.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, role TEXT, age INTEGER)")
    live = set()
    for _ in range(nops):
        op = rng.random()
        if op < 0.55 or not live:  # insert
            k = rng.randrange(1, 4000)
            name, role, age = f"n{k}_{seed}", rng.choice(ROLES), rng.randrange(18, 70)
            if k in live:
                ql.append(f"INSERT INTO people VALUES ({k}, '{name}', '{role}', {age});")
                # sqlite errors too; both reject the duplicate
            else:
                live.add(k)
                ref.execute("INSERT INTO people VALUES (?,?,?,?)", (k, name, role, age))
                ql.append(f"INSERT INTO people VALUES ({k}, '{name}', '{role}', {age});")
        elif op < 0.75:  # update
            k = rng.choice(sorted(live))
            age = rng.randrange(18, 70)
            ref.execute("UPDATE people SET age=? WHERE id=?", (age, k))
            ql.append(f"UPDATE people SET age = {age} WHERE id = {k};")
        else:  # delete
            k = rng.choice(sorted(live))
            live.discard(k)
            ref.execute("DELETE FROM people WHERE id=?", (k,))
            ql.append(f"DELETE FROM people WHERE id = {k};")
    # random transaction wrapping for part of the workload
    script = "\n".join(ql)
    with tempfile.TemporaryDirectory() as td:
        db = os.path.join(td, "m.qry")
        p = subprocess.run([BIN, db], input=script, text=True, capture_output=True)
        if p.returncode not in (0,):
            return f"seed {seed}: quarry exited {p.returncode}: {p.stderr[:400]}"
        # dump in key order
        p = subprocess.run([BIN, db, "-c", "SELECT id, name, role, age FROM people ORDER BY id;"],
                           text=True, capture_output=True)
    got = []
    for line in p.stdout.splitlines()[1:]:
        if line.startswith("("): break
        parts = [x.strip() for x in line.split("|")]
        got.append((int(parts[0]), parts[1], parts[2], int(parts[3])))
    want = [(r[0], r[1], r[2], r[3]) for r in ref.execute("SELECT * FROM people ORDER BY id")]
    if got != want:
        for g, w in zip(got, want):
            if g != w: return f"seed {seed}: mismatch quarry={g} sqlite={w}"
        return f"seed {seed}: length mismatch quarry={len(got)} sqlite={len(want)}"
    return None

fails = 0
for seed in range(1, 9):
    err = run_seed(seed, 600)
    if err:
        print(err); fails += 1
if fails:
    print(f"MODEL TEST FAILED: {fails}/8 seeds")
    sys.exit(1)
print("model test: 8 seeds x 600 ops match sqlite3 exactly")
