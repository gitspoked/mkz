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
 [ block: sync marker + CRC + data ]   <- each block self-contained: a recognizable
 [ block: sync marker + CRC + data ]      start marker + per-block checksum, then the
 [ ... ]                                   UNCHANGED zstd/autocol payload, still size-gated
 [ INDEX SEGMENTS ]                <- one or more TYPED segments (kind #1 = sorted key
                                       directory: canonical base95/b95u16 keys, sparse,
                                       each entry -> (block, row-in-block), exact seek AND
                                       ordered range). Fixed-width, contiguous, byte-
                                       comparable. Future kinds: per-block integrity map,
                                       secondary index, vector/ANN index - all additive.
 [ SCHEMA HEADER ]                 <- column names + count, key column index, key type
                                       (open type enum: int/text/raw + reserved vector/blob)
 [ FOOTER ]                        <- offsets/sizes of segments + schema, flags, at a known
                                       tail offset (read the end first, then map/seek)
 [ 32-byte SHA-256 ]               <- whole-archive integrity, as today (coexists with
                                       the per-block checksums, which enable partial recovery)
```

Read path (no full decode): read footer -> jump to schema + index segments -> binary-search
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

## The decisions to bake in (so adapters AND future capabilities are plugins, not format changes)

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
7. **Index segments are typed and pluggable, not a hardcoded directory.** The footer lists
   index segments, each tagged with a `kind`: sorted-key directory is kind #1; a secondary
   index, a per-block integrity map, or a vector/ANN index are later kinds slotted into the
   same additive footer real estate - not a redesign. Old/unknown-kind readers reject
   loudly (pairs with the roadmap "reject unknown flag bits").
8. **Values are binary-safe, and the type system is open.** The value path is length-
   delimited and byte-clean (not "everything is newline-delimited text"), and the column
   type enum is open with reserved codes (`int`/`text`/`raw` today; reserved `vector<f32,d>`
   / `blob` / `float`). This is the vector-and-model-weights door: an embedding is a fixed-
   width blob, a checkpoint is name -> raw tensor bytes (a safetensors-shaped SSTable), so
   storing them later is a value/type addition, not a format change. Nearness/range indexing
   over vectors is a future index-segment `kind` (decision #7); the sorted scalar directory
   deliberately does not try to serve it (no total order preserves nearness in high-D).
9. **DMA-friendly physical layout (Chris's AI chip).** Keep directory/key slots fixed-width,
   contiguous, and byte-comparable so access is constant-stride. Prefer power-of-2 sizes and
   alignment (16/32/64/128 B): a 32-byte slot is one 256-bit HBM/AXI beat. base95's raw
   10-byte / 3-byte widths are the awkward non-power-of-2 sizes that force burst splitting
   (30-50% penalty), so **pad the key slot to 16/32 B if it is ever DMA-consumed.** An
   accelerator has no MMU (DMA, not mmap), so this "DMA-friendly" property is the governing
   one; zero-copy mmap on a CPU host is a bonus that the same fixed-width layout also gives.
   Never trade fixed width away for variable-length/delta-encoded keys to save space -
   that destroys both strideability and in-place binary search.

## Resilience and recovery (mkzfix)

mkz is block-structured, so unlike a single continuous DEFLATE/gzip stream (where one bad
bit desyncs the rest and cannot resync), a damaged block need not destroy the others. Today
that potential is unrealized: the only integrity is one whole-archive SHA-256 - all-or-nothing
detection that neither identifies the bad block nor blesses the good ones. This design cashes
the potential in, the way `bzip2recover` does for bzip2:

- **Per-block sync markers.** Each block starts with a recognizable marker so a recovery
  scan can find block boundaries even when the tail footer/index is the damaged part (the
  footer is otherwise a single point of failure). bzip2 does exactly this (its block magic).
- **Per-block checksums.** So "this block is intact" is *trustworthy*, not merely "it
  decompressed without erroring" (corrupt data often decompresses to plausible garbage).
  Carried as an index-segment kind (decision #7), outside the compressed data.
- **Self-contained blocks (invariant).** Recovery requires no cross-block shared dictionary
  or carried state - losing one block must not poison its neighbors. This is an additional
  reason the ~3% cross-block dictionary gain was rejected: block independence is a capability.
- **`mkzfix` - a separate recovery utility** (like `bzip2recover`, not part of the core
  reader): scan a damaged archive, find blocks via markers, keep every block that verifies,
  rebuild the directory, re-seal a fresh valid mkz, and **loudly report the holes** - never
  present a partial as whole. The seekable directory makes the report precise: Family A =
  intact members + a manifest of lost ones; Family B = surviving key ranges + named gaps
  ("recovered keys 0-3999 and 4064-10000, lost 4000-4063").
- **Loss granularity = block size**, so block size is a resilience knob (smaller = finer
  recovery, more overhead) - expose it.

This ties the threads together: sync markers + per-block CRC + the seekable directory +
block independence all sit OUTSIDE the compressed data (additive, never-worse-gate-safe) and
together give seek, recover, AND OTA-update friendliness from one structure. (FEC for a lossy
transmission channel is a heavier, separate concern - out of scope here.)

## Compression-ratio cost (firm figure)

The payload ratio is UNCHANGED: data blocks are byte-for-byte the same zstd/autocol output
as 0.1.2 and still governed by the never-worse gate. Everything this design adds is additive
overhead alongside the blocks, not a regression on them.

The shipping CLI default block is 16 MiB uncompressed (`DEFAULT_BLOCK_MB = 16`, override
`PSRC_AC_BLOCK_MB`; PAS1's internal fallback is 1 MiB). Additive overhead is per-block:
~8 B sync marker + ~8 B checksum (xxh3; 4 B CRC32) + one sparse directory entry (~32 B:
padded 16 B key + block offset + len) ~= **48 B/block worst case**, plus a one-time ~270 B
(magic + footer + schema; the 32 B SHA-256 already existed).

Against a 16 MiB block (~3.5 MiB on disk at typical text ratios) that is ~1 part in 350,000:
**under ~0.01% total overhead, effectively unmeasurable** (a 100 GiB corpus adds ~300 KB).
Break-even for 1% overhead is a compressed block near ~4.8 KB, i.e. an uncompressed block of
~20-25 KB - roughly 700x smaller than default; even dropping to 1 MiB blocks for finer
recovery granularity costs only ~0.02%. The only cost that scales with data rather than block
count is a Family-A directory over millions of tiny files (like any zip central directory).
Keep per-block checksums/markers behind an optional flag so pure-archival mode pays nothing.

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
  unknown block-flag bits loudly." Unknown index-segment kinds are rejected the same way.
- The never-worse gate still governs the data blocks; the index segments (directory,
  per-block integrity, etc.) are outside it.
- **Blocks are self-contained** - no cross-block dictionary or carried state - so a single
  block can be decoded, verified, and recovered independently.
- **Fixed-width, byte-comparable, power-of-2-aligned** key/directory slots (DMA-friendly;
  see decision #9). Never break the fixed-width invariant.

## Suggested phasing (refine in writing-plans)

1. **Container + reader foundation**: the PAS2 layout, footer, `libmkz` open/read, and
   the path-key directory (Family A member seek). Proves the machinery end to end.
2. **Record/PK mode (Family B)**: schema header, designated key column, full-record
   values, sorted-by-key storage, exact + range lookup.
3. **Adapter + secondary indexes**: a SQLite virtual-table adapter as the demo of
   `WHERE key=...` pushdown; generalize the directory for secondary indexes; enable
   per-column (projection) decode.
4. **Resilience**: per-block sync markers + per-block checksum index segment, self-contained
   block enforcement, and the `mkzfix` recovery utility. (Marker/checksum framing is cheap
   and could land as early as Phase 1; the tool follows once the directory exists.)
5. **Later**: DuckDB and Postgres adapters; vector/ANN index segment; per-column projection
   decode; DMA-consumption path (padded power-of-2 key slots) if a chip target materializes.

## Open questions (resolve before/into writing-plans)

- Exact byte format of the footer, directory entries, and schema header.
- New magic (PAS2) vs a flagged PAS1 successor; version-byte strategy.
- How path -> block-range (Family A) and record-sorted-by-key (Family B) share one
  directory shape.
- Key type declaration: infer from a CSV header/first rows vs an explicit flag.
- Row-in-block addressing granularity (per-row offsets vs re-scan within the block).
- Where `libmkz` lives (grow the existing C port into a linkable library; Rust FFI or
  parallel Rust reader).
- Block sync-marker format (value/width) and how a recovery scan disambiguates it from
  payload bytes; per-block checksum algorithm (CRC32 vs xxh3) and where it lives relative
  to the block and the whole-archive SHA-256.
- Typed index-segment header layout (kind tag, version, offset/size) shared across
  sorted-key / integrity / future secondary + vector kinds.
- Key-slot padding policy: keep native base95 widths (10/3 B) for CPU/disk, or pad to
  power-of-2 (16/32 B) unconditionally for a single DMA-ready layout - decide if/when a
  chip target is real (the DMA-friendly lens; the Qubed chip work).
