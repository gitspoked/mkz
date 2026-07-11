# mkz seekable segment (0.2) - design

Status: draft (brainstorm converged; awaiting spec review, then writing-plans)
Date: 2026-07-09
Thread: AUTOCOL and MKZ Compression (engine work; release mechanics live in the OpenBSD/Releases thread)

## North star

Turn a sealed mkz archive into something you can query by key without unpacking it:
eventually `SELECT * FROM matt.mkz WHERE col1='blah'` through a thin adapter, with
the `WHERE` pushed down to a seek rather than a full scan. Concretely: take a text
row-export (CSV/TSV/JSONL - the natural export of MySQL/Postgres/Oracle/MSSQL) and
store it as a sealed, sorted, seekable, self-describing mkz segment.

This is, precisely, an **SSTable** (sorted string table): an immutable, block-compressed
key -> value file, sorted by key, with a sparse index at the tail. mkz is already ~80%
of one (block compression + integrity trailer); this design adds the missing 20%.

## Why (how we got here)

- "Dictionary work" for compression ratio is marginal (~3%; the backend LZ already
  dedups). So the goal is **capability, not ratio**.
- The capability wanted is **canonical sortable keys + seek**, finally using the
  dormant `base95` (u64 -> 10 printable bytes, memcmp == numeric) and `b95u16`
  (UTF-16 code unit -> 3 printable bytes, sorts in code-unit order) primitives, which
  are already proven byte-exact across the C and Rust implementations and across arm64.
- Tension: the never-worse gate keeps only the smaller candidate, and sortable-key
  forms are usually larger. Resolution: **the key directory lives OUTSIDE the
  compressed blocks** (additive), so the data blocks stay byte-for-byte gated and the
  directory is never subject to the size gate.

## What we are building

A new sealed segment format (working name PAS2, or a flagged PAS1 successor):

```
 "PAS2"  magic                     <- new marker; old readers MUST reject cleanly
 [ blocks ... ]                    <- UNCHANGED zstd/autocol blocks, still size-gated
 [ KEY DIRECTORY ]                 <- sorted canonical keys (base95/b95u16),
                                       sparse: each entry -> (block, row-in-block),
                                       supports exact seek AND ordered range
 [ SCHEMA HEADER ]                 <- column names + count, key column index, key type
 [ FOOTER ]                        <- offsets/sizes of directory + schema, flags
 [ 32-byte SHA-256 ]               <- integrity, as today
```

Read path (no full decode): read footer -> jump to schema + directory -> binary-search
the sorted keys (zero decompression for existence / range / ordered iteration) ->
decompress only the one block a hit points at -> return the row(s). Honest boundary:
**key queries = zero decode; fetching a value = one block.**

The three original wants all fall out of this one structure: seek is the footer +
sorted directory; canonical sortable keys are required for the binary search; text-safe
is free because base95/b95u16 keys are printable ASCII (no byte < 0x20).

## Key source (what becomes a lookup key)

One directory structure, two key sources (same machinery, different input):

- **Family A - path/filename keys (foundation).** Every archived entry already carries
  its relative path; persist those sorted with a path -> block-range map. This is the
  classic central directory (zip has it, tar lacks it): seek to and extract one file
  without inflating the whole archive. Needs no new schema.
- **Family B - record/primary-key keys (the target).** A designated column (first
  column, header-named field, or column #) is the key; the value is the full record;
  records are stored sorted by key. This is the SSTable / InnoDB-record use case and
  what makes SQL pushdown meaningful. Requires the schema/key-field concept.

Family A is the appetizer that proves the machinery; Family B is the goal. Column-value
keys plug into the same directory the path keys use.

## Ingestion

Newline-delimited text, CSV as the headline case (also TSV/JSONL/SQL-INSERT). mkz stays
database-agnostic: `DB -> text export -> sealed seekable mkz segment`. mkz never parses
proprietary DB files (`.ibd`/`.mdf`/`.dbf` are opaque pages - a poor fit, autocol no-ops).

## The seven decisions to bake in (so any SQL adapter is a plugin, not a format change)

Every candidate host links a C/C++/.NET reader and wants the same things:

1. **Ship a stable C reader library (`libmkz`), not just a CLI**: `open / seek(key) /
   get / scan(range)`. Highest-leverage decision - every adapter links it.
2. **Carry a schema header**: column names + count, key column index, key type. SQL
   cannot present a table without names; this makes the schema concept first-class.
3. **Encode the key canonically by declared type**: base95 for integer keys, b95u16 for
   text, raw fallback - so an adapter's `=`, `<`, `BETWEEN` match SQL ordering via plain
   memcmp, without the adapter knowing mkz internals.
4. **Directory supports exact seek AND ordered range** (sparse `min_key/max_key ->
   block`): pushdown is half point-lookups, half range scans.
5. **Store the full record as the value**, directory -> (block, row-in-block), so
   `SELECT *` returns the row.
6. **Keep per-column decode possible** (do not force materializing every column) - this
   is what lets DuckDB/Postgres do projection pushdown later. Not required in v1; just
   do not preclude it.
7. **Treat the directory as a general index segment** so a secondary index (a second
   seekable column) is another instance later, not a redesign.

## SQL host friendliness (informs priorities, not required for v1)

- **DuckDB** - friendliest: table function / extension, filter + projection pushdown
  first-class (same path Parquet uses).
- **Postgres** - very friendly: FDW with qualifier + column + sort pushdown (mkz is
  already sorted, so ORDER BY key pushes down).
- **SQLite** - cheapest first target: virtual table `xBestIndex` gives real pushdown.
- **Oracle** - moderate: pipelined TVF is easy but no pushdown; real pushdown needs the
  heavy Data Cartridge extensible-indexing path.
- **MS SQL** - awkward: SQLCLR TVF or PolyBase; weak pushdown, often admin-locked.
- **MySQL** - hardest, explicitly an afterthought: custom storage engine (heavy) or
  CONNECT+CSV (scan only, no seek benefit).

Pattern: engines built to query external files push `WHERE key=...` into a seek; the big
traditional RDBMSs stream rows out of a function but resist pushing the predicate in.

## Non-goals

- Not a live, mutable, transactional database. No in-place updates, no MVCC. Updates are
  LSM-style (write a new segment, merge/compact later) - out of scope here.
- Not changing normal `.mkz` archives: 0.1.2 archives stay valid and byte-identical; the
  seekable segment is a new, additive format.
- Not a native MySQL engine (afterthought). Not parsing proprietary DB on-disk files.
- Not chasing compression ratio; block compression is unchanged.

## Constraints / invariants

- C and Rust must both implement it and produce byte-identical segments. The directory
  must be deterministic (base95/b95u16 are already byte-exact across impls/arch).
- Old readers must reject the new format cleanly - pairs with the roadmap item "reject
  unknown block-flag bits loudly."
- The never-worse gate still governs the data blocks; the directory is outside it.

## Suggested phasing (refine in writing-plans)

1. **Container + reader foundation**: the PAS2 layout, footer, `libmkz` open/read, and
   the path-key directory (Family A member seek). Proves the machinery end to end.
2. **Record/PK mode (Family B)**: schema header, designated key column, full-record
   values, sorted-by-key storage, exact + range lookup.
3. **Adapter + secondary indexes**: a SQLite virtual-table adapter as the demo of
   `WHERE key=...` pushdown; generalize the directory for secondary indexes; enable
   per-column (projection) decode.
4. **Later**: DuckDB and Postgres adapters.

## Open questions (resolve before/into writing-plans)

- Exact byte format of the footer, directory entries, and schema header.
- New magic (PAS2) vs a flagged PAS1 successor; version-byte strategy.
- How path -> block-range (Family A) and record-sorted-by-key (Family B) share one
  directory shape.
- Key type declaration: infer from a CSV header/first rows vs an explicit flag.
- Row-in-block addressing granularity (per-row offsets vs re-scan within the block).
- Where `libmkz` lives (grow the existing C port into a linkable library; Rust FFI or
  parallel Rust reader).
