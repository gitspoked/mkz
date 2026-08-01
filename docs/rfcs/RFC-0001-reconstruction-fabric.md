# RFC-0001: Reconstruction Fabric for Snapshot-Aware Sync

Status: Draft

## Summary

Define a reconstruction fabric that can turn a directory, archive, snapshot, or
snapshot-like manifest into a compact, verifiable packet. The packet is decoded
into the original state exactly by default, or into an explicitly bounded lossy
equivalence class only when the profile declares that mode.

This RFC does not replace mkz, autocol, zstd, brotli, or a snapshot engine. It
defines the contract for composing them:

```text
source state
-> manifest
-> chunks
-> shape lanes
-> compressed wire packet
-> decode
-> reconstructed state
-> verifier == source state
```

The near-term proof is a sidecar pack/unpack tool over normal filesystem input.
The longer-term target is snapshot-aware sync where peers exchange only the
missing reconstructive state and can resume, verify, and roll back.

## Motivation

File transfer and sync tools often make users reason about operational details:

- whether a file was missed
- whether a transfer can resume
- whether a destination is byte-identical
- whether two machines share enough prior state to avoid retransmission
- whether a compressed packet can be recovered after damage

The reconstruction fabric makes those concerns explicit in the format. The
encoder may spend arbitrary CPU searching for a smaller recipe. The decoder must
remain strict and boring: decode the recipe, reconstruct the target, and verify
the result before accepting it.

## Goals

- Reconstruct arbitrary input bytes exactly by default.
- Support directory and snapshot-like inputs as first-class state, not only flat
  byte streams.
- Use shared peer state when available, so already-known chunks are referenced
  instead of retransmitted.
- Shape structured metadata before general compression, improving input to
  zstd, brotli, and future codecs.
- Preserve content identity over the original bytes, even when the wire payload
  is transformed or compressed.
- Keep old-reader behavior safe: unknown formats, flags, lanes, or codec ids
  must reject loudly.
- Provide progress, resumability, and per-part verification.
- Keep Rust and C implementations byte-compatible for interchange formats.

## Non-Goals

- This RFC does not claim Hilbert coordinates alone compress data.
- This RFC does not require changing the current PAS1 mkz default writer.
- This RFC does not make lossy reconstruction the default.
- This RFC does not require a live filesystem snapshot backend.
- This RFC does not require upstream integration with any external project.
- This RFC does not require realtime synchronization.

## Definitions

### Source State

The bytes and metadata the sender intends the receiver to reconstruct.

For a flat file, source state is the file bytes. For a directory or snapshot,
source state includes path names, file bytes, file kinds, executable bits, link
targets, and any metadata a selected profile declares canonical.

### Manifest

A canonical description of source state. A manifest is not necessarily the wire
format. It is the ledger the decoder must be able to reconstruct and verify.

### Chunk

A byte range from source state identified by a content digest over the original
chunk bytes. The digest is computed before any wire compression.

### Shape Lane

A reversible transform over one homogeneous part of the manifest or payload.
Examples include path dictionaries, timestamp deltas, enum lanes, integer
frame-of-reference lanes, segmented-linear numeric runs, raw fallback lanes, and
chunk-reference lanes.

### Recipe

The packet-level instruction set that tells a decoder how to recreate source
state from shared state plus transmitted residual data.

### Verifier

The deterministic accept/reject rule. Exact profiles compare reconstructed bytes
and/or canonical manifest hashes. Lossy profiles compare against an explicitly
declared equivalence relation and error budget.

## Required Invariants

### Exact Mode

Exact mode is the default and MUST satisfy:

```text
decode(packet, shared_state) -> reconstructed_state
hash(reconstructed_state) == hash(source_state)
```

The verifier MUST fail closed. A decoder MUST NOT return reconstructed output as
accepted if the final verifier fails.

### Content Identity

Chunk identity MUST be computed over original uncompressed chunk bytes:

```text
chunk_id = digest(original_chunk_bytes)
```

Wire compression is storage and transport only. It MUST NOT redefine chunk
identity.

### Reversibility

Every exact-mode shape lane MUST satisfy:

```text
decode_lane(encode_lane(x)) == x
```

A lane that cannot prove that property for the candidate input MUST fall back to
a raw exact lane.

### Never-Worse Selection

For archive compression profiles, a transform or codec candidate SHOULD be used
only when it is smaller than the current fallback after all lane headers,
checksums, and codec ids are counted.

The fallback for any exact source bytes is raw or existing mkz/zstd behavior,
depending on the selected profile.

### Safe Evolution

Readers MUST reject unknown critical ids:

- packet format version
- critical manifest kind
- block codec id
- shape lane id
- transform flag
- verifier kind

Unknown ancillary ids MAY be skipped only when the packet declares them
non-critical and the verifier does not depend on them.

## Packet Model

The first proof packet SHOULD use this logical structure:

```text
RF_PACKET := HEADER MANIFEST_LANES PAYLOAD_LANES VERIFIER

HEADER := magic version profile flags codec_registry_ref
MANIFEST_LANES := lane_count lane*
PAYLOAD_LANES := chunk_group_count chunk_group*
VERIFIER := source_digest manifest_digest optional_chunk_digests
```

The physical encoding is intentionally left open for the first prototype. The
prototype may use a simple binary envelope or JSON for inspection, as long as it
can prove deterministic byte-for-byte reconstruction.

## Lane Families

The first implementation SHOULD test these lane families:

| Lane | Purpose | Fallback |
|------|---------|----------|
| raw | exact bytes | none |
| path-dict | repeated path prefixes and file names | raw |
| metadata-delta | modes, sizes, timestamps, ids | raw |
| enum | repeated low-cardinality values | raw |
| chunk-ref | chunk ids and file-to-chunk references | raw |
| segmented-linear | counters, offsets, timestamps, coordinates | raw |
| frame-of-reference | bounded integers around a base | raw |
| b95/b95u16 | printable keys or UTF-16 code-unit-shaped fields | raw |

The lane ids for autocol v2 remain governed by `REGISTRY.md`. Reconstruction
fabric packet ids SHOULD either reuse those ids by reference or allocate their
own registry section before becoming an interchange format.

## Shared State

Peers MAY share prior state:

- chunks from older archives
- files already present at the destination
- known base images
- known package trees
- dictionaries
- snapshot manifests
- format registries

Shared state is an optimization only. A packet MUST declare whether shared state
is optional or required.

If shared state is required, the packet MUST name it by digest or registry id so
the decoder can reject when the peer has the wrong model.

## Resumability

A packet SHOULD be splittable into independently verifiable parts:

- manifest header
- lane section
- chunk group
- chunk
- recovery block

The decoder SHOULD persist enough progress to avoid refetching verified parts.
Partial output MUST NOT be promoted to accepted output until the verifier passes.

## Lossy Profiles

Lossy profiles are allowed only when explicitly selected. A lossy profile MUST
declare:

- canonicalization rule
- distance function
- maximum permitted error
- verifier rule
- whether the output may be used as a source for later exact packets

Exact and lossy packets MUST be distinguishable at the header/profile level.

## Relation to mkz 0.1 and 0.2

mkz 0.1 remains an exact streaming archive built from PAS1, autocol v1, zstd,
the never-worse gate, and SHA-256 verification.

mkz 0.2 is the first planned release allowed to emit v2 bytes by default. The
reconstruction fabric SHOULD NOT block the 0.2 wire release. Instead, it should
begin as a sidecar proof that informs later PAS2 or sync-oriented work.

Recommended sequencing:

1. Keep 0.1.5 as the v2 prep base.
2. Land RFC-0001 as a draft design record.
3. Build a sidecar proof command outside the default writer path.
4. Benchmark against raw, zip, zstd, brotli, mkz, autocol, and shaped variants.
5. Promote only proven packet pieces into `REGISTRY.md`.

## Prototype Shape

The initial tool MAY be named one of:

```text
rf-pack
rf-unpack
mkz-rf-pack
mkz-rf-unpack
```

The first proof SHOULD support:

```text
rf-pack <input-dir> <packet>
rf-unpack <packet> <output-dir>
rf-verify <input-dir> <output-dir>
rf-bench <input-dir>
```

Minimum proof:

```text
input directory
-> manifest
-> chunks
-> packet
-> output directory
-> canonical digest match
```

## Benchmark Ladder

Every benchmark row SHOULD report:

- source bytes
- packet bytes
- overhead bytes
- compression ratio
- saved percent
- encode time
- decode time
- exact/lossy mode
- verifier result

Required comparisons:

```text
raw
zip
zstd
brotli
mkz current
autocol + zstd
autocol + brotli
reconstruction fabric packet
```

For sync-oriented tests, also report:

```text
peer already has 0%
peer already has 25%
peer already has 50%
peer already has 75%
peer already has 95%
```

## Open Questions

- Should reconstruction fabric ids live in `REGISTRY.md` immediately, or wait
  until the first binary packet is implemented?
- Should the sidecar proof live in `crates/psrc-mkz`, a new crate, or a separate
  workspace member?
- Which metadata is canonical for directory proofs across macOS, Linux, and
  OpenBSD?
- Should chunking reuse mkz block boundaries first, or introduce
  content-defined chunking for sync profiles?
- Should remote-peer transfer be modeled now, or after the local packet proof is
  complete?

## Acceptance Criteria

RFC-0001 becomes implementable when:

- an exact canonical digest is defined for flat files and directories
- a packet envelope is specified
- at least raw, path, metadata, and chunk-ref lanes have exact decoders
- old-reader rejection behavior is documented
- benchmark scripts produce comparable rows against existing mkz baselines
- Rust and C compatibility expectations are stated for any emitted interchange
  bytes
