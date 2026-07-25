# mkz format registry

The single authority for every enumerated id in the mkz format family. Rules:

- Ids are NEVER reused or renumbered. Retired ids stay reserved forever.
- Both implementations (mkz-c and crates/psrc-mkz) conform to this file.
- A release that allocates an id updates this file in the same commit.
- For each IMPLEMENTED codec id, this file records the normative C library, pinned
  version, and parameters. Byte-identical C/Rust output requires both implementations to
  call the same library with the same settings. Byte-exactness is a WRITER property
  (reproducible object identity); readers only need correctness.

## PAS1 block flag bits (u8)

| bit | meaning | status |
|-----|---------|--------|
| 0 | payload decompresses to an autocol blob | allocated (0.1.0) |
| 1-7 | reserved; writers MUST emit 0; readers MUST reject (since 0.1.3) | reserved |

## Codec ids (planned block-header field, ships in 0.2)

| id | codec | status | normative library / parameters |
|----|-------|--------|-------------------------------|
| 0 | zstd | allocated (implied today by absence of the field) | libzstd; level recorded per archive |
| 1 | brotli | reserved for 0.2 | C libbrotli via FFI both impls; pin quality/lgwin/mode at allocation |
| 2 | stored (identity, no compression) | reserved for 0.2 | none; measured necessary for high-entropy payloads (settlement 2.3) |
| 3 | lz4 | reserved; implement only if the no-MMU chip target demands a trivial RTL decoder | liblz4 if ever implemented |
| 4-15 | future general codecs | reserved | |
| 16-31 | per-column specialized codecs (0.4: delta, RLE/dict, frame-of-reference, b95pack) | reserved | |
| 240-255 | private/experimental; never in interchange archives | reserved | |

xz/lzma and lzop are explicitly NOT allocated (settlement 2.3: measured no-win, and
supply-chain caution on liblzma).

## PAS2 index-segment kinds (ships in 0.3; criticality per settlement 2.4)

Criticality: a reader hitting an UNKNOWN critical kind MUST reject loudly; an unknown
ancillary kind MUST be skipped with a single warning.

| kind | segment | criticality | status |
|------|---------|-------------|--------|
| 1 | sorted-key directory | ancillary | reserved for 0.3 |
| 2 | per-block integrity map / Merkle leaves | ancillary | reserved |
| 3 | secondary index | ancillary | reserved |
| 4 | vector/ANN index (Ring 3 consumes) | ancillary | reserved |
| 5 | signature/provenance | ancillary | reserved |
| 6+ | unallocated | | |

## Value type enum (PAS2 schema, ships in 0.3)

| code | type | status |
|------|------|--------|
| 0 | int (base95 key encoding) | reserved for 0.3 |
| 1 | text (b95u16 key encoding) | reserved for 0.3 |
| 2 | raw | reserved for 0.3 |
| 3 | vector<f32,d> | reserved (the model-weights/vector door) |
| 4 | blob | reserved |
| 5 | float | reserved |
| 6+ | unallocated | |

## Key encodings (frozen ABI)

base95 (u64 -> 10 bytes) and b95u16 (UTF-16 code unit -> 3 bytes) are frozen as specified
in FORMAT.md sections 4-5, byte-identical across mkz-c, crates/psrc-autocol, and Qubed.
