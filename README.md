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
  `UPDATE`, `DELETE`. NULLs supported with SQL three-valued logic:
  a `WHERE` comparison that touches NULL never matches the row.
- **Every statement is atomic.** A failed statement inside an explicit
  transaction undoes only its own pages; the transaction stays open.
  A failed `COMMIT` (disk full, I/O error) leaves no trace: partial WAL
  frames are truncated away and the transaction can still be rolled back.
- **Corruption is contained.** Every B+tree page is validated when read
  (type, cell counts, cell bounds), so a corrupted file produces a clean
  error, not a crash or an infinite loop.
- **Tested like it matters.** 38,395 pager/B+tree unit checks (insert /
  find / scan / delete against a model, with commit-close-reopen and
  crash-recovery cycles, plus corrupt-page rejection), scripted CRUD /
  transaction / error / NULL-semantics cases, a 20,000-row stress run
  that forces interior splits, fault injection that fails a commit
  mid-WAL-write via `RLIMIT_FSIZE`, and a differential test that runs
  random workloads against SQLite and compares the full table contents.
  The whole suite also runs under ASan + UBSan in CI, and the shipped
  fuzzer (`tests/fuzz.py`) throws 300 rounds of garbage SQL and 200
  rounds of on-disk corruption per run with no crash and no hang.

## Honest limits

- No joins, no secondary indexes - every query is a full scan of one
  table. The matching rows of any statement are collected in memory
  (capped at 100,000); `ORDER BY` sorts that set before printing.
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
