# RFC-0002: PAS2 Seekable Segments

Status: Draft

## Summary

Define PAS2, a future mkz container for sealed, seekable, block-compressed
segments. PAS2 keeps the existing mkz promise of exact reconstruction, while
adding:

- a new magic so old readers reject cleanly
- self-contained blocks
- typed index segments
- a footer that supports range reads
- canonical sortable keys
- optional per-block integrity data for verified streaming and recovery

PAS2 is a new container, not a flagged PAS1 variant.

## Motivation

PAS1 is a strong streaming archive: blocks are compressed independently, the
original content is verified with SHA-256, and autocol is kept only when it wins
the never-worse gate. What PAS1 does not provide is direct access. A reader must
decode the archive sequentially to find one member or one record.

PAS2 adds the missing directory and footer layer so a sealed archive can act
like an immutable segment:

```text
open footer
-> read schema and index segments
-> seek to the block that may contain the key
-> decode only that block
-> verify before accepting output
```

The first capability target is member seek: extract one archived path without
inflating the whole archive. The second target is keyed record seek over
structured exports such as CSV, TSV, and JSONL.

## Goals

- Keep exact reconstruction as the default invariant.
- Use a new magic so old PAS1 readers fail closed.
- Keep compressed data blocks governed by the existing never-worse principle.
- Keep blocks self-contained so a damaged block does not poison its neighbors.
- Add typed index segments outside compressed blocks.
- Support exact point lookups and ordered range scans by key.
- Allow range reads from ordinary files, object stores, and HTTP servers.
- Enable future recovery tooling without changing the archive core again.
- Keep C and Rust implementations byte-compatible for interchange bytes.

## Non-Goals

- PAS2 is not a mutable database.
- PAS2 does not replace PAS1 for simple streaming archives.
- PAS2 does not require SQL adapters in the first implementation.
- PAS2 does not require vector or approximate-nearest-neighbor indexes.
- PAS2 does not change the semantics of autocol v1.
- PAS2 does not permit lossy reconstruction.

## Required Invariants

### New Magic

PAS2 MUST use a new magic:

```text
"PAS2"
```

It MUST NOT be represented as a PAS1 stream with a reserved flag bit set. Older
PAS1 readers existed before reserved-bit rejection, so a flagged PAS1 successor
would risk being misread instead of rejected.

### Exact Content

The decoded PAS2 content MUST match the canonical source state:

```text
decode_pas2(segment) -> content
hash(content) == recorded_content_hash
```

### Self-Contained Blocks

Each data block MUST be independently decodable. A reader MUST NOT require
cross-block compression dictionaries or carried decoder state to decode a later
block.

### Additive Metadata

Indexes, schemas, integrity maps, and provenance metadata live outside the
compressed data blocks. Adding or removing an ancillary index segment MUST NOT
change the decoded payload bytes.

## Logical Layout

The draft logical layout is:

```text
PAS2_SEGMENT :=
  MAGIC
  DATA_BLOCK*
  INDEX_SEGMENT*
  SCHEMA_SEGMENT?
  FOOTER
  CONTENT_DIGEST
```

Where:

```text
MAGIC := "PAS2"
DATA_BLOCK := block_header compressed_payload
INDEX_SEGMENT := segment_header segment_payload
FOOTER := fixed_tail_pointer segment_directory flags
CONTENT_DIGEST := SHA-256 over canonical decoded content
```

The exact byte layout is left to implementation planning, but the footer MUST
make it possible to locate all index and schema segments from the end of the
file.

## Data Blocks

A PAS2 data block carries:

- original decoded length
- compressed payload length
- block codec id
- transform flags or transform id
- optional fast corruption checksum
- compressed payload

The codec id is governed by `REGISTRY.md`. The initial planned ids are zstd,
brotli, and stored.

The writer may try multiple candidates per block:

```text
stored
zstd
autocol + zstd
brotli
autocol + brotli
```

The writer SHOULD keep the smallest candidate that round-trips and satisfies the
selected policy.

## Index Segments

Index segments are typed. Segment kinds are governed by `REGISTRY.md`.

Each segment kind has a criticality rule:

- unknown critical segment: reject
- unknown ancillary segment: skip with warning when safe

Initial planned segment kinds:

| kind | segment | criticality |
|------|---------|-------------|
| 1 | sorted-key directory | ancillary |
| 2 | per-block integrity map / Merkle leaves | ancillary |
| 3 | secondary index | ancillary |
| 4 | vector/ANN index | ancillary |
| 5 | signature/provenance | ancillary |

The criticality flag lets future advisory metadata be added without making old
readers reject data they can otherwise decode.

## Key Families

PAS2 uses one directory mechanism for two initial key families.

### Family A: Path Keys

Every archive member path becomes a key. The sorted-key directory maps:

```text
path_key -> block range or member location
```

This gives mkz the practical benefit of a central directory: extracting a single
member without scanning the whole archive.

### Family B: Record Keys

Structured records may designate a primary key column. The directory maps:

```text
record_key -> block location and row location
```

This enables exact point lookup and ordered range scans over sealed record
exports.

## Key Encoding

PAS2 key encodings use the frozen base key primitives from `FORMAT.md`:

- `base95`: fixed-width printable u64 keys
- `b95u16`: fixed-width printable UTF-16 code-unit keys
- raw bytes: binary-safe fallback

The directory MUST compare encoded keys bytewise. A key encoding that claims
numeric or text ordering MUST preserve that order under byte comparison.

## Schema Segment

Record-keyed PAS2 segments SHOULD carry a schema segment containing:

- field count
- field names
- key field index
- key type
- value type declarations when known

Path-keyed PAS2 segments MAY omit the schema segment.

## Verified Streaming

PAS2 MAY carry a per-block integrity segment. The intended form is a list of
cryptographic block digests and a root digest over that list.

The whole-content digest remains the final exactness check. The per-block
integrity map exists so a range-read client can verify each fetched block
without waiting for the full object.

## Recovery

Recovery tooling is a first-class PAS2 use case. A recovery tool MAY scan for
block sync markers, keep blocks that verify, rebuild available index data, and
write a fresh segment that loudly reports missing content.

Recovery MUST NOT present a partial archive as complete.

## Reader Behavior

A PAS2 reader MUST:

- reject unknown magic
- reject unknown critical segment kinds
- reject unknown codec ids
- reject malformed footers and segment directories
- bounds-check every offset, length, key count, and row pointer
- verify decoded content before reporting success

An ancillary segment may be skipped only when the reader can still satisfy the
requested operation and final verification requirements.

## Writer Behavior

A PAS2 writer MUST:

- emit deterministic directory ordering
- emit self-contained blocks
- record codec ids explicitly
- use registry-allocated ids only
- keep index segments outside compressed payload blocks
- compute content identity over decoded original content

## Compatibility

PAS2 is not readable by PAS1 readers. That is intentional. PAS1 remains valid,
and default PAS1 writers do not change.

C and Rust implementations MUST agree on:

- integer encoding
- endianness
- key encoding
- segment ordering
- footer layout
- digest inputs
- codec parameters for byte-identical writers

## Acceptance Criteria

RFC-0002 becomes implementable when:

- the fixed byte layout is specified
- the footer and segment directory are fully defined
- Family A path-key seek has golden vectors
- old-reader rejection is tested
- C and Rust can read the same PAS2 fixture
- malformed offset and length tests fail closed
- benchmark output shows overhead against PAS1 for representative archives
