# mkz roadmap

Post-0.1.0 ideas - 0.1.0 ships the clean zstd archiver.

## Sequencing (release order)

Keep the train short. The 0.1 line is prep and compatibility; 0.2 is the first real
wire-format expansion.

1. **0.1.3 - safety release.** Ship the forward-compat reader behavior and extraction
   hardening that make future archives fail cleanly on old readers.
2. **0.1.4 - v2 prep release.** No default wire change. Freeze the ids, golden vectors,
   test harnesses, docs, and release-directory split needed to pull autocol v2 and mkz v2
   down cleanly.
3. **0.2.0 - v2 wire release.** Autocol `FORMAT_VERSION = 2` plus mkz block codec ids.
   Brotli/stored block codecs and the first field-aware JSONL/autocol v2 lanes land here
   behind the existing never-worse gate.
4. **0.2.x - cleanup / recovery riders.** Per-block recovery markers, `mkzfix`, tar/gz/zst
   extraction, and any polish that does not need a new major format idea.

Orthogonal riders (fold in where convenient, not gating): the "read the old world" tar
extractor; the DMA-friendly layout property (only firms up if the chip target does).

## 0.1.3 (safety release, no format change)
- **[shipped in 0.1.3] Atomic extraction**: write files to a temp path and `rename()` into place
  only after the SHA-256 trailer verifies, so a corrupt archive leaves nothing half-written.
  (0.1.0 verifies the SHA *last*, after files are on disk; the README/CHANGELOG wording was
  corrected to say so.)

- **[shipped in 0.1.3] Reject unknown block-flag bits loudly**: the per-block `flags` byte uses
  only bit 0 (autocol). Make the decoder error on any bit it doesn't understand, so a future
  codec (see brotli below) fails clean on an old reader instead of feeding foreign bytes to
  zstd. Cheap forward-compat insurance; costs nothing today since every 0.1.0 block uses only
  bit 0. **Prerequisite for the codec id field, PAS2 index-segment kinds, and recovery markers -
  ship before all of them.**

- **REGISTRY.md**: the id-space authority (flag bits, codec ids, PAS2 segment kinds,
  value types) now lives at the repo root; every future allocation updates it in the
  same commit.

## 0.1.4 (v2 prep release, no default wire change)

This release exists so 0.2 is a product release, not a planning bundle.

- Freeze `REGISTRY.md` allocations for mkz block codec ids and autocol v2 lane ids.
- Add golden vectors for current autocol v1/PAS1 and the base95/b95u16 ABI so v2 work cannot
  accidentally move the old floor.
- Add non-shipping probes/benchmarks for JSONL field-aware lanes, segmented-linear numeric
  lanes, and Brotli/stored candidates.
- Maintain writer defaults as PAS1 + autocol v1 + zstd. Old readers must still read default
  archives from 0.1.4.
- Create and maintain a separate v2 release staging directory seeded only with the pertinent
  public files.

## 0.2.0: v2 wire release

0.2.0 is the first release allowed to emit bytes old 0.1 readers cannot decode by default.
Because 0.1.3 readers reject unknown flag bits and 0.1.4 freezes the id registry/vectors,
old tools should fail loudly instead of guessing.

### mkz block codec framework (brotli as codec #1)

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

### autocol v2 / field-aware lanes

- Bump autocol's blob version when a new on-wire lane is emitted. v1 readers reject v2 blobs.
- Keep the existing v1 tokenizer path as the compatibility model.
- Add field-aware JSONL lanes where records share stable keys, then specialize value lanes:
  constant, enum, delta, segmented-linear, and raw fallback.
- Keep lane choice never-worse by measuring candidate lane size and then the backend result.

## 0.2.x: resilience and recovery (mkzfix)

Block-structured formats can survive damage that kills a continuous gzip stream, but today mkz's
single whole-archive SHA-256 is all-or-nothing. Cash in the block structure, bzip2recover-style:

- **Per-block sync markers** (find block boundaries even if the tail footer/index is damaged) +
  **per-block checksums** (trustworthy "this block is intact", not "it decompressed without
  erroring"). Additive block-header work; shares the "richer block header" theme with the codec id.
- **Self-contained blocks (invariant):** no cross-block dictionary/state, so a lost block never
  poisons its neighbors. (Also why the ~3% cross-block dictionary was not worth taking.)
- **`mkzfix`**: a separate utility (like `bzip2recover`) that harvests every block that verifies,
  rebuilds the directory, re-seals a fresh valid mkz, and loudly reports the holes.
- **Generation-aware re-sealing.** Because data blocks and the content trailer are identical
  across container generations, the same re-seal path that recovery uses can also convert:
  demote a newer archive to plain PAS1 for old readers (drop the additive segments; transcode
  any non-zstd blocks down to zstd), or promote a PAS1 by computing the segments. The content
  SHA-256 is preserved verbatim either way, so a converted archive provably carries the same
  data. Recovery and conversion are one verb with different inputs; both default their output
  to the oldest generation every mkz can read.
- Overhead is ~0.01% at the default 16 MiB block; keep markers/checksums behind an optional flag
  so pure-archival mode pays nothing.

## 0.2.x rider: read the old world (interop, decode-side only, no format change)
- **Extract `.tar`, `.tar.gz`, `.tar.zst`**: teach `mkz -xf` to detect and unpack standard
  tarballs. Purely additive, never touches the mkz format. This is the zstd playbook: write your
  better format, read everyone else's. Gives adopters "it just works" experience.

- Note: native tar.gz *output* is intentionally NOT a goal; mkz's win (autocol's per-block
  columnar transform behind the never-worse gate) needs PAS1's per-block framing, which tar+gzip
  can't express. mkz is to tar.gz what zstd is to gzip: better, and deliberately its own format.

## Later: seekable/keyed segment

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

## Later: per-column codecs beyond autocol v2

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
