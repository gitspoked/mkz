# mkz roadmap

Post-0.1.0 ideas - 0.1.0 ships the clean zstd archiver.

## Sequencing (dependency order)

The post-0.1 work is a dependency chain, not a flat list. Ship in this order:

1. **0.1.x - reject unknown flag bits (the unblocker).** Prerequisite for everything after:
   codec ids, new index-segment kinds, and recovery markers all need old readers to fail
   clean on bits they do not understand. Cheap; do it first.
2. **0.2 - codec framework + resilience.** Brotli done AS the codec registry (not a one-off),
   plus per-block recovery framing + the `mkzfix` tool. Minimal format touch, guarded by (1).
3. **0.3 - PAS2 seekable segment (the big format).** Container, footer, typed index segments,
   sorted-key directory, schema header, `libmkz`; Family A (paths) then Family B (records).
   Honors the DMA-friendly layout property (chip target). Full design in
   `docs/superpowers/specs/2026-07-09-mkz-seekable-segment-design.md`.
4. **0.4+ - per-column codecs (ratio on the columnar substrate).** Rides on 0.3's schema and
   0.2's codec registry. This is where dictionary work, if any, belongs.

Orthogonal riders (fold in where convenient, not gating): the "read the old world" tar
extractor; the DMA-friendly layout property (only firms up if the chip target does).

## 0.1.x (fast-follow, no format change)
- **Atomic extraction**: write files to a temp path and `rename()` into place only after the
  SHA-256 trailer verifies, so a corrupt archive leaves nothing half-written. (0.1.0 verifies the
  SHA *last*, after files are on disk; the README/CHANGELOG wording was corrected to say so.)

- **Reject unknown block-flag bits loudly**: the per-block `flags` byte uses only bit 0 (autocol).
  Make the decoder error on any bit it doesn't understand, so a future codec (see brotli below)
  fails clean on an old reader instead of feeding foreign bytes to zstd. Cheap forward-compat
  insurance; costs nothing today since every 0.1.0 block uses only bit 0. **Prerequisite for the
  codec id field, PAS2 index-segment kinds, and recovery markers - ship before all of them.**

- **REGISTRY.md**: the id-space authority (flag bits, codec ids, PAS2 segment kinds,
  value types) now lives at the repo root; every future allocation updates it in the
  same commit.

## 0.2: codec framework (brotli as codec #1)

Proven +12% on target data, but corpus-dependent (a text/log win, like autocol; on binary the
gate falls back). Do it as the codec REGISTRY, so per-column codecs (0.4) and any future codec
are registry entries, not format changes.

- The corpus benchmark (`crates/psrc-corpus`) already measures brotli beating zstd on the target
  data (~0.216 vs zstd on logs). But brotli is ONLY a comparison baseline there (it shells out to
  the `brotli` CLI); it is **not** a codec in the archive format, in Rust **or** C. The shipping
  crate `psrc-mkz` depends on `zstd` only.

- Integration:
  1. Introduce a **codec id field** in the block header (spend a whole byte: 0 = zstd, 1 = brotli,
     reserved ids for lzma / column-specialized / etc.) plus a small codec abstraction in both
     impls: `codec_id -> {compress, decompress}`, backed by an identical registry.
  2. Link `libbrotlienc`/`libbrotlidec` (C). For Rust, prefer a **binding** to the C libbrotli
     (`brotlic`/`brotli-sys`) over the pure-Rust `brotli` crate, so C and Rust call the identical
     encoder and stay byte-identical for free (same trick as the `zstd` crate = FFI to libzstd).
     Whichever path, pin brotli quality / `lgwin` / mode explicitly in both, or defaults diverge.
  3. Extend the never-worse gate to try `{zstd, autocol+zstd, brotli, autocol+brotli}` per block,
     keep the smallest that round-trips, record the winning codec id. Pure upside; more compress
     time (4 candidates/block, and brotli is slower at high quality).
  4. Decode-side dispatch on the codec id, in both implementations.

- Keep C<->Rust interop: both must gain brotli together, or a brotli archive from one won't open
  in the other. (Today both are zstd-only, so interop holds.) Requires the 0.1.x reject-unknown
  work first, so a zstd-only reader refuses a brotli block instead of feeding it to zstd.

## 0.2: resilience and recovery (mkzfix)

Block-structured formats can survive damage that kills a continuous gzip stream, but today mkz's
single whole-archive SHA-256 is all-or-nothing. Cash in the block structure, bzip2recover-style:

- **Per-block sync markers** (find block boundaries even if the tail footer/index is damaged) +
  **per-block checksums** (trustworthy "this block is intact", not "it decompressed without
  erroring"). Additive block-header work; shares the "richer block header" theme with the codec id.
- **Self-contained blocks (invariant):** no cross-block dictionary/state, so a lost block never
  poisons its neighbors. (Also why the ~3% cross-block dictionary was not worth taking.)
- **`mkzfix`**: a separate utility (like `bzip2recover`) that harvests every block that verifies,
  rebuilds the directory, re-seals a fresh valid mkz, and loudly reports the holes.
- Overhead is ~0.01% at the default 16 MiB block; keep markers/checksums behind an optional flag
  so pure-archival mode pays nothing.

## 0.2 (rider): read the old world (interop, decode-side only, NO format change)
- **Extract `.tar`, `.tar.gz`, `.tar.zst`**: teach `mkz -xf` to detect and unpack standard
  tarballs. Purely additive, never touches the mkz format. This is the zstd playbook: write your
  better format, read everyone else's. Gives adopters "it just works" experience.

- Note: native tar.gz *output* is intentionally NOT a goal; mkz's win (autocol's per-block
  columnar transform behind the never-worse gate) needs PAS1's per-block framing, which tar+gzip
  can't express. mkz is to tar.gz what zstd is to gzip: better, and deliberately its own format.

## 0.3: PAS2 seekable/keyed segment (the big format)

Turn a sealed archive into a seekable, keyed, self-describing SSTable-shaped segment (query by
key without unpacking). Full design + phasing + open questions:
`docs/superpowers/specs/2026-07-09-mkz-seekable-segment-design.md`. Headlines:

- New PAS2 marker (old readers reject cleanly); data blocks UNCHANGED and still size-gated; the
  directory and schema live OUTSIDE the compressed blocks (additive, never-worse-gate-safe).
- **Typed, pluggable index segments** (sorted-key = kind #1; secondary / per-block-integrity /
  vector-ANN are later kinds). Ship `libmkz` (open/seek/get/scan). Family A (path keys) proves the
  machinery; Family B (record/PK keys) is the SQL-pushdown target.
- **Binary-safe values + open type enum** (reserved `vector<f32,d>`/`blob`) = the model-weights /
  vector door; nearness/range over vectors is a future index kind, not the scalar directory.
- **DMA-friendly layout property (chip target):** fixed-width, contiguous, byte-comparable,
  power-of-2-aligned key slots; pad base95's 10/3 B forms to 16/32 B if ever DMA-consumed. No-MMU
  accelerators DMA rather than mmap. Honor if/when the chip target firms up.
- Ratio cost of all this structure: ~0.01% at the default block. The payload ratio is unchanged.

## 0.4+: per-column codecs (ratio on the columnar substrate)

Rides on 0.3's schema/columns and 0.2's codec registry. Once a column is homogeneous, a
specialized codec (delta for monotonic timestamps/ids, RLE/dictionary for low cardinality,
frame-of-reference for bounded ints, brotli for free text) can beat a general codec by far more
than the ~3% a whole-block dictionary managed. Gate generalizes to `{transforms} x {codecs}` per
column, driven by type-based defaults + sampling (combinatorial otherwise).

- **Dictionary work belongs here, not earlier.** On whole mixed blocks it was ~3% marginal
  (redundant with backend LZ). Its only real home is a codec-menu entry: a trained zstd dictionary
  for many-small-similar-file archives, chosen per column/segment where it actually wins.

## Housekeeping
- General cleanup/re-org...
- Fix the `crates/psrc-corpus` reference if that crate is renamed/moved (it is cited above but not
  present in the current tree).
