# mkz north star

Status: design note

## One Sentence

mkz is an exact, verifiable archive core growing into a seekable, streamable,
reconstructive segment format.

## Product Promise

The user should not have to wonder:

- whether every file arrived
- whether a transfer can resume
- whether the destination is byte-identical
- whether an old reader guessed wrong
- whether a damaged archive lost everything
- whether a peer already had most of the needed state

mkz should make those answers mechanical:

```text
decode
verify
accept or reject
```

## Layers

### Layer 0: Exact Archive Core

PAS1 is the floor:

- block-compressed stream
- reversible autocol transform
- never-worse writer policy
- SHA-256 verification
- C and Rust implementations
- old-reader safety work

This layer remains boring on purpose. It is the trusted ground.

### Layer 1: Better Archive

The 0.2 line adds capability without changing the core identity:

- codec ids
- brotli and stored candidates
- cleaner fallback behavior
- per-block recovery direction
- foreign-format read support when useful

The default rule stays exact reconstruction.

### Layer 2: Seekable Segment

PAS2 turns a sealed archive into a direct-access segment:

- new magic
- footer-first reads
- typed index segments
- path-key member seek
- record-key point lookup and range scan
- optional per-block integrity

This is where mkz stops being only "compress and extract" and becomes
"open, seek, verify, and return exactly what was asked for."

### Layer 3: Reconstruction Fabric

Reconstruction fabric uses manifests, chunks, shape lanes, shared state, and
verifiers to move the least necessary state between peers.

This is not magic compression. It is disciplined reconstruction:

```text
shared state + packet -> reconstructed state -> verifier == target
```

mkz provides the exact archive and segment substrate. Higher-level sync and
snapshot systems can decide what state is missing and how to request it.

## Guardrails

- Exact mode is default.
- Lossy mode must be explicit and bounded.
- Unknown critical ids fail closed.
- Content identity is computed over original decoded bytes.
- Metadata evolution is additive where possible.
- Blocks stay independently decodable when recovery or seeking depends on it.
- C and Rust must agree on interchange bytes.
- Public docs describe contracts, not local scratch state.

## What We Avoid

- silent best-effort extraction
- undocumented id allocation
- changing content identity during transport
- making PAS1 carry incompatible PAS2 semantics
- cross-block state that breaks recovery
- chasing ratio at the expense of verifiability

## The Shape of Winning

The project is winning when a user can say:

```text
put this state there
resume if interrupted
show me progress
prove it arrived
let me seek one thing
let me recover what survived
```

and mkz can provide the exact archive and segment machinery underneath that
workflow.
