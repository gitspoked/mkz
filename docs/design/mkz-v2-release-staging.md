# mkz v2 release staging

Status: design note

## Purpose

The v2 staging tree is a separate working copy for the first v2 wire-format
work. It is not the source of truth until it has been refreshed from the
accepted 0.1.5 base.

## Version Rule

- `0.1.3` ships safety: unknown-flag rejection, atomic extract, bounded C
  extraction memory, and `REGISTRY.md`.
- `0.1.4` shipped the public bridge: crates and registry prep, with no default
  archive byte change.
- `0.1.5` ships the cleanup base: public design docs and RFCs, the OpenBSD
  deep-extract placement fix, OpenBSD package proof, and the v2 staging rule.
  Default writers still emit PAS1 + autocol v1 + zstd.
- v2 starts from `v0.1.5`.
- `0.2.0` is the first release that may emit v2 interchange bytes: mkz block
  codec ids, autocol `FORMAT_VERSION = 2`, and field-aware lanes such as
  constant, enum, and segmented-linear.

## Staging Rule

Do not bulk-copy unrelated build output into the v2 staging tree. Add files only
as they become part of the v2 release surface.

Before starting implementation in the v2 staging tree:

1. Refresh the staging directory from the accepted `0.1.5` release state.
2. Preserve these design notes.
3. Ensure Rust crate versions, C version strings, and OpenBSD port metadata all
   agree on the intended v2 starting point.
4. Run Rust and C byte-compatibility tests from the refreshed tree.

## Public Tree Rule

Public design docs should describe the expected process, not local machine
state. Local path names, temporary staging mismatches, and unpublished release
scratch notes belong in private notes or issue comments, not in committed
design records.
