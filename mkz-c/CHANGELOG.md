# Changelog

Notable changes to mkz and autocol. Versions are SemVer; format loosely follows
Keep a Changelog.

## [0.1.3] - unreleased

### mkz (both implementations)
- Readers now REJECT blocks with unknown flag bits instead of silently decoding them as
  plain zstd (forward-compat gate for codec ids and PAS2; settlement spec 2026-07-25).
- Extraction is atomic: entries stream into `<dest>/.mkz-partial.<pid>` and are placed
  into `<dest>` only after the SHA-256 trailer verifies. A failure before that point
  places nothing; a failure during the final move can leave a partial merge. The staging
  directory is retained and reported either way.
- `mkz -xf -` (stdin) is refused with a clear message (the reader needs a seekable file).

### C build
- Fixed extraction memory that grew with total archive size instead of staying flat. The
  streaming decoder rebuilt its per-block output buffers (`mkz_autocol_decode`'s
  reconstruction buffer and `zstd_grow`'s decompression buffer) from empty via
  malloc/realloc/free on every block instead of reusing them; on this platform's
  allocator that left roughly one block's worth of resident memory behind per block,
  never reclaimed until process exit. Both buffers are now reused across the whole
  extraction (`mkz_pas1_decode_block_into` plus a per-extraction scratch freed once at
  the end), so peak RSS is now genuinely O(block), independent of total archive size.
  Measured peak RSS on extract: before the fix, 168 MB at a ~96 MB input, growing to
  374 MB at a ~305 MB input (not bounded); after the fix, 82 MB at ~96 MB and 80 MB at
  ~305 MB (flat).

### docs
- New REGISTRY.md at the repo root governs all format id spaces.

## [0.1.2] - 2026-07-09 (crates, C tool, and OpenBSD port unified on one version)

### mkz (both implementations)
- The Rust encoder now pledges the zstd frame content size (via `zstd::bulk::compress`),
  matching the C one-shot encoder. On a shared libzstd the two implementations now emit
  byte-identical archives; decoders still accept frames with or without a pledged size, so
  archives remain cross-compatible in both directions.

### C build
- `make check` now passes `${CPPFLAGS}` when compiling the container test, so `<zstd.h>` is
  found when libzstd lives under a non-default prefix (e.g. `${LOCALBASE}` on OpenBSD). The
  OpenBSD port no longer needs a Makefile patch, and the distfile roots at `${DISTNAME}/`
  (standard layout), so the port drops its `WRKDIST` override.

## [0.1.1] - 2026-07-05 (Rust crate release; C tool follows with the next tarball)

### mkz (both implementations)
- `-v` on create now reports what the autocol pre-pass earned: blocks kept vs total,
  payload-to-stream ratio, and exact bytes/percent saved versus zstd alone. The
  never-worse gate already compresses every block both ways to decide, so the
  comparison is measured, not estimated, and costs nothing extra.
- C `--version` suffix aligned with the Rust build: `(psrc)` / `(psrc, C port)`.

## [0.1.0] - 2026-07-04 (initial release)

### mkz
- Tar-style archiver: a reversible auto-columnar pre-pass (`autocol`) followed by zstd,
  with a per-block never-worse gate (`min(zstd(raw), zstd(autocol))`) and a SHA-256
  integrity gate: extraction verifies a SHA-256 over the whole stream and reports corruption
  on a mismatch (extraction is not yet atomic, so a corrupt trailer can leave already-written
  files; temp+rename is planned).
- Streaming, bounded memory (peak ~ one block) for typical line-oriented text/logs;
  pathological newline-free input is buffered whole and not yet bounded.
- Two builds: Rust (`cargo install mkz`) and pure C (`cc` + libzstd, no Rust toolchain)
  for the BSDs, older Linux, and embedded. The C and Rust encoders are byte-identical and
  archives interoperate both ways.
- Ships a man page, INSTALL + install.sh, and OpenBSD-port / Debian packaging skeletons.

### autocol
- The reversible transform as a standalone library (`autocol`). It bundles no compressor;
  you feed its output to your own backend.

### Roadmap
- 0.1.1 cleanup / 0.1.2 dictionary work / 0.1.3 brotli backend.
