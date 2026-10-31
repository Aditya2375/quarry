# quarry architecture

## Files

A database is two files:

- `name.qry` - the data file: an array of 4096-byte pages. Page 0 is the
  header (magic, committed page count, catalog root page, catalog sequence).
- `name.qry.wal` - the write-ahead log.

## Pages

```text
header page 0:  magic u32 | page_count u32 | catalog_root u32 | catalog_seq u64

btree page:     type u8 (1 leaf, 2 interior) | ncells u16 | used u32 | link u32
                leaf cell:     key i64 | len u16 | payload
                interior cell: key i64 | child u32     (child holds keys < key)
                leaf link  = right sibling;  interior link = rightmost child
```

Rows are self-describing: one tag byte per column (NULL / int64 / text),
then the payload. The catalog is itself a B+tree whose records are
fixed-size table descriptions (name, root page, column list).

## Transactions and recovery

1. `pager_begin` starts a transaction. All writes go to an in-memory dirty
   set; reads check the dirty set, then the WAL map, then the data file.
2. `pager_commit` appends every dirty page to the WAL as a frame
   (magic, page number, committed page count on the last frame, CRC32 of
   the payload), then fsyncs. Only then are the in-memory WAL map and the
   page count updated and the commit reported successful. If any write or
   the fsync fails, the partial frame tail is truncated back: the WAL
   looks exactly as it did before the commit, and the still-open
   transaction can be rolled back or retried.
3. `pager_rollback` drops the dirty set. Nothing uncommitted ever reaches
   the file, so rollback is that simple.
4. A checkpoint (on close, on `.checkpoint`, or automatically after 2000
   logged frames) copies the WAL-mapped pages into the data file, fsyncs,
   and truncates the WAL.
5. On open, recovery scans the WAL: frames are validated (magic + CRC),
   complete transactions are folded into the in-memory WAL map, and a
   torn or corrupt tail (the crash point) is ignored. One deliberate
   trade-off: a CRC failure stops recovery at that frame, so if a
   *completed* frame is ever damaged, later committed transactions are
   dropped too - CRCs prove where valid data ends, not where corruption
   ends. The data file is untouched, so the database always opens at a
   consistent (at worst slightly older) state.

Consequences: a crash at any point leaves either the old state or the new
state, never a blend; and an in-progress transaction is invisible after a
crash. `.crash` in the REPL exercises exactly this path (it exits without
checkpointing): it simulates an OS or process crash, after which every
fsynced commit must survive. It does not simulate machine power loss,
which can also drop OS page-cache buffers; directory entries created for
the data and WAL files are fsynced at open to keep that part durable.

A failed *statement* inside an explicit transaction follows the same
idea at a smaller scale: the executor marks a savepoint before the
statement and restores the dirty-set length and page count on failure,
which discards exactly that statement's pages while the transaction
stays open.

## Trade-offs (why it is small enough to verify)

- Buffering a transaction's dirty set in memory makes rollback and
  read-your-writes trivial, at the cost of a per-transaction page budget
  (2048 pages). Larger transactions get an explicit error.
- Not rebalancing deletes keeps the B+tree insert/split logic small and
  fully testable; sparse pages cost space, never correctness.
- Full-scan execution keeps the executor honest: the B+tree provides
  ordering and point lookup, not query planning.
