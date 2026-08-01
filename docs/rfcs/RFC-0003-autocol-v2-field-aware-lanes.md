# RFC-0003: autocol v2 Field-Aware Lanes

Status: Draft

## Summary

Define autocol v2 as a reversible, field-aware extension of the existing
schema-free autocol transform. v2 keeps the core contract:

```text
decode(encode(x)) == x
```

The v2 encoder may parse structured records, split values into homogeneous
lanes, and choose specialized lane representations. The decoder remains strict:
it reconstructs the original byte stream exactly or rejects.

## Motivation

autocol v1 is effective on line-oriented data because it groups repeated line
skeletons and separates values. JSONL, CSV, TSV, and log-like records often have
deeper structure than v1 currently uses:

- stable keys
- repeated enums
- constant fields
- monotonic ids
- timestamps
- offsets
- piecewise-linear counters

General compressors do better when those values are presented in homogeneous
lanes. autocol v2 formalizes those lanes while preserving v1 fallback behavior.

## Goals

- Preserve exact byte reconstruction.
- Keep autocol v1 readable and valid.
- Add a new autocol blob version for v2.
- Reject unknown v2 lane ids unless explicitly skippable and non-critical.
- Split structured input into field-aware lanes only when safe.
- Keep lane selection behind round-trip and never-worse gates.
- Provide metrics so corpus benchmarks can explain why a lane won or lost.

## Non-Goals

- autocol v2 is not a JSON canonicalizer.
- autocol v2 does not require every input to parse as JSON or CSV.
- autocol v2 does not change PAS1 block semantics by itself.
- autocol v2 does not make lossy transforms legal.
- autocol v2 does not require schema declarations from the user.

## Versioning

autocol v2 MUST use a new blob version byte:

```text
version = 2
```

v1 readers MUST reject v2 blobs. v2 readers MUST continue to read v1 blobs.

## Lane Registry

Lane ids are governed by `REGISTRY.md`.

Initial planned v2 lane ids:

| id | lane | meaning |
|----|------|---------|
| 0 | raw values | exact fallback |
| 1 | delta integers | signed integer deltas |
| 2 | dictionary / enum | repeated low-cardinality values |
| 3 | constant value | one value repeated for a lane |
| 4 | segmented-linear integer runs | `(start, step, count)` runs |
| 5 | frame-of-reference bounded integers | base plus bounded offsets |
| 6 | b95pack / b95u16 | printable key or code-unit packing |

Unknown lane ids MUST reject unless a future version defines a safe ancillary
lane mechanism.

## Exactness Rule

Every v2 lane MUST satisfy:

```text
decode_lane(encode_lane(values)) == values
```

The full v2 blob MUST satisfy:

```text
decode_autocol_v2(encode_autocol_v2(bytes)) == bytes
```

If parsing, lane construction, or reconstruction cannot prove exact byte
recovery, the encoder MUST fall back to v1 or raw.

## Record Parsing

The first field-aware profile SHOULD target JSONL:

- one record per line
- each record must parse as a JSON object
- parse failure falls back
- mixed non-object records fall back
- trailing bytes and line endings must be preserved

CSV and TSV MAY be added later with explicit policies for quoting, separators,
headers, and line endings.

## JSONL Reconstruction

JSONL reconstruction MUST preserve the original bytes. A v2 JSONL encoder may
do this by storing:

- a stable key dictionary
- object key order per record, when needed
- separators and punctuation policy
- whitespace/residual data when needed
- value lanes
- line ending form

If the encoder cannot represent those details more cheaply than fallback, it
SHOULD reject the candidate lane plan and use v1 or raw.

## Candidate Selection

The encoder may try several representations:

```text
raw
v1 autocol
v2 raw lanes
v2 field-aware lanes
v2 field-aware lanes + specialized numeric lanes
```

mkz may then try backend codecs such as:

```text
zstd
brotli
stored
```

The winning candidate SHOULD be the smallest candidate that round-trips under
the selected profile after all headers and lane metadata are counted.

## Lane Metrics

Benchmarks SHOULD report:

- original bytes
- v1 candidate bytes
- v2 candidate bytes
- backend-compressed bytes
- lane count
- selected lane ids
- fallback reason, when a candidate is rejected
- round-trip verifier result

## Compatibility

autocol v2 is a transform layer. It does not by itself define a new mkz
container. A PAS1 block may carry an autocol blob only if the reader understands
that blob version. For interchange, writers MUST consider the minimum supported
reader version before emitting v2 blobs.

## Acceptance Criteria

RFC-0003 becomes implementable when:

- the v2 blob layout is specified byte-for-byte
- JSONL exact reconstruction has golden vectors
- malformed JSONL falls back or rejects deterministically
- every initial lane has decoder tests
- v1 fixtures still decode
- v1 readers reject v2 blobs cleanly
- corpus benchmarks compare raw, v1, v2, zstd, brotli, and mkz outputs
