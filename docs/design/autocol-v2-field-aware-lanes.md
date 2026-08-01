# autocol v2 field-aware lanes

Status: design note

## Goal

Make the existing schema-free autocol transform field-aware where the input
offers stable record structure, without losing the v1 fallback.

autocol remains a reversible transform, not a compressor:

```text
decode(encode(x)) == x
```

mkz may then feed the transformed bytes to zstd, brotli, or another registered
codec behind the existing never-worse gate.

## Compatibility

- autocol v1 remains the default in the 0.1 release line.
- autocol v2 uses a new blob version byte.
- v1 readers reject v2 blobs instead of guessing.
- mkz 0.2 writers may choose autocol v2 only when the candidate round-trips and
  wins the never-worse gate.
- Any v2 lane that cannot prove exact reconstruction for the candidate input
  falls back to v1 or raw bytes.

## First Lane Set

Lane ids are reserved in `REGISTRY.md`.

| id | lane | purpose |
|----|------|---------|
| 0 | raw values | exact fallback |
| 1 | delta integers | current numeric path, carried forward |
| 2 | dictionary / enum | small repeated sets |
| 3 | constant value | one value repeated for a lane |
| 4 | segmented-linear integer runs | `(start, step, count)` segments for counters, offsets, coordinates, timestamps, and piecewise arithmetic rows |
| 5 | frame-of-reference bounded integers | later candidate |
| 6 | b95pack / b95u16 | later candidate for printable key or code-unit packing |

## JSONL Direction

The existing autocol v1 code is JSONL-friendly because it groups line skeletons,
but it does not parse JSON keys. v2 may add a field-aware path for JSONL records
with stable keys:

1. Parse only valid JSON objects, one object per line.
2. Build a stable key dictionary.
3. Store the record skeleton once, or store enough original separator and order
   data to reproduce the input bytes exactly.
4. Split values into key-indexed lanes.
5. Pick the smallest exact lane representation.
6. Reconstruct the original bytes exactly.

The field-aware path must fall back to v1 or raw when parsing, canonicalization,
or lane selection would risk changing bytes.

## Open Questions

- Should JSONL object key order be preserved as an order lane, or should v2 only
  accept records whose input form matches the selected canonical policy?
- Should the first implementation live inside autocol, or as an mkz-side
  candidate transform that later graduates into autocol?
- Should segmented-linear probing run before dictionary probing or after it?
- Which lane metrics should be surfaced in benchmark output?
