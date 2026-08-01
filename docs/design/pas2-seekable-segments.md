# PAS2 seekable segments

Status: design note

## North Star

Make mkz more than a streaming archive without weakening its core promise.

PAS1 answers:

```text
Can I compress this data and recover it exactly?
```

PAS2 should additionally answer:

```text
Can I seek directly to the member, key, range, or block I need?
Can I verify streamed parts before the whole object arrives?
Can I recover intact blocks from a damaged segment?
```

The product shape is an immutable, block-compressed, self-describing segment.
It is closer to an SSTable than to a traditional tarball: sorted keys, sparse
indexes, footer-first reads, and exact payload verification.

## Why This Belongs in mkz

mkz already has:

- block compression
- exact integrity checks
- a reversible transform layer
- C and Rust implementations
- printable sortable key primitives
- a registry for future ids

The missing capability is direct access. A normal archive asks the reader to
walk the whole stream. A seekable segment lets the reader inspect the footer,
load an index, and decode only the needed block.

## Shape

The conceptual layout is:

```text
"PAS2"
data block
data block
...
index segment
index segment
schema segment
footer
content digest
```

The data blocks stay self-contained. The index and schema segments are additive
metadata outside compressed payloads.

## Family A: Member Seek

The first practical win is path lookup:

```text
relative/path -> block range or member location
```

This gives mkz a central-directory style capability. A reader can extract one
file without inflating the full archive.

## Family B: Record Seek

The second win is keyed records:

```text
primary_key -> block location and row location
```

A CSV, TSV, JSONL, or similar export can become a sealed keyed segment. The
reader can perform point lookup or range scan by key without scanning the full
file.

## Footer-First Reads

PAS2 is designed for local files and range-capable object storage.

Read path:

```text
read tail
-> parse footer
-> locate schema and index segments
-> binary-search keys
-> fetch/decode selected block
-> verify result
```

That keeps transport boring. Any normal file server or object store that can
serve ranges can carry a PAS2 segment.

## Integrity and Recovery

PAS1 has a whole-content SHA-256. That remains the final exactness check.

PAS2 may add per-block integrity data so range-read clients can verify one
block at a time. Recovery tools can use sync markers and per-block checks to
salvage intact blocks and report missing ranges.

The invariant is strict:

```text
partial recovery is allowed
pretending partial recovery is complete is not allowed
```

## Key Encoding

PAS2 uses existing key primitives:

- `base95` for fixed-width printable integer keys
- `b95u16` for fixed-width printable UTF-16 code-unit keys
- raw bytes when no ordered printable encoding is promised

The directory compares keys bytewise. Ordered key encodings must preserve the
declared order under byte comparison.

## Phasing

1. Define the exact PAS2 footer and segment directory.
2. Implement Family A path lookup and member extraction.
3. Add per-block integrity and recovery markers.
4. Add Family B record-key mode.
5. Add optional readers/adapters that use the seek interface.

## Design Pressure

PAS2 has to stay disciplined:

- new magic for incompatible container changes
- no cross-block decoder state
- metadata outside compressed blocks
- registry-governed ids
- deterministic byte layout
- old readers fail closed
- C and Rust agree on interchange bytes

That discipline is what lets mkz grow from archive to segment without becoming
an unbounded format pile.
