# quarry

**[Run it live in your browser](https://aditya2375.github.io/quarry/)** - the real
storage engine and SQL executor compiled to WebAssembly, no install.

quarry is a small relational database built from first principles in C:
a page-based storage engine with a write-ahead log, B+tree indexes, and a
SQL subset with transactions.

```text
quarry> CREATE TABLE employees (id INT PRIMARY KEY, name TEXT, role TEXT, salary INT);
table EMPLOYEES created.
quarry> INSERT INTO employees VALUES (1, 'Ada', 'engineer', 120);
1 row inserted.
quarry> SELECT name, salary FROM employees WHERE role = 'engineer' ORDER BY salary DESC;
NAME | SALARY
Ada  | 120
(1 row)
```

## Quick start

```bash
make
./build/quarry my.db          # REPL (creates the file)
./build/quarry my.db -c "SELECT * FROM employees;"
make check                    # full test suite
```

## What is real

- **Crash-safe transactions.** Every commit appends the transaction's pages
  to the WAL and fsyncs before reporting success. Committed data survives a
  kill at any point (`.crash` is a built-in power-loss hook); uncommitted
  changes are never visible after recovery. `BEGIN` / `COMMIT` / `ROLLBACK`
  work, and statements outside an explicit transaction are auto-committed.
- **B+tree storage.** 4 KB pages, signed int64 keys, ordered scans via leaf
  sibling links, leaf and interior splits, root growth.
- **SQL subset.** `CREATE TABLE` (INT / TEXT columns, one optional INT
  `PRIMARY KEY`), `INSERT` (positional or named columns), `SELECT` with
  `WHERE` (ANDed `= != < <= > >=`), `ORDER BY`, `LIMIT`, `COUNT(*)`,
  `UPDATE`, `DELETE`. NULLs supported.
- **Tested like it matters.** 38,362 pager/B+tree unit checks (insert /
  find / scan / delete against a model, with commit-close-reopen and
  crash-recovery cycles), scripted CRUD / transaction / error cases, a
  20,000-row stress run that forces interior splits, and a differential
  test that runs random workloads against SQLite and compares the full
  table contents. The whole suite also runs under ASan + UBSan in CI, and
  300 rounds of garbage-input fuzzing produce no crash.

## Honest limits

- No joins, no secondary indexes - every query is a full scan of one
  table; `ORDER BY` sorts the matching rows in memory (capped at 100,000).
- A transaction's dirty pages are buffered in memory (bounded at 2048
  pages / 8 MB per transaction); a bigger transaction gets a clean error,
  not corruption.
- Deletes compact the leaf page but never merge or rebalance the tree, so
  a table that shrinks keeps its sparse pages until it is rebuilt.
- One row is capped at 1000 bytes; text values at 800 bytes.
- Single-writer, no concurrency control, no networking.
- The database file only grows (no free list); checkpoints reclaim WAL
  space, not data pages.

## Layout

```text
src/pager.c    pages, WAL, crash recovery, checkpoints, transactions
src/btree.c    B+tree: splits, ordered cursors, point lookups
src/record.c   row encoding
src/sql.c      tokenizer + parser for the SQL subset
src/db.c       catalog, executor, transactions
src/main.c     REPL
tests/         unit checks, scripted cases, sqlite3 differential test
docs/          architecture notes + the browser demo
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the file formats and
the recovery protocol.
