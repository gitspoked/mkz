# Changelog

Notable changes to mkz and autocol. Versions are SemVer; format loosely follows
Keep a Changelog.

## [0.1.1] - 2026-07-05 (Rust crate release; C tool follows with the next tarball)

### mkz (both implementations)
- `-v` on create now reports what the autocol pre-pass earned: blocks kept vs total,
  payload-to-stream ratio, and exact bytes/percent saved versus zstd alone. The
  never-worse gate already compresses every block both ways to decide, so the
  comparison is measured, not estimated — and costs nothing extra.
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
- 0.1.1 cleanup · 0.1.2 dictionary work · 0.1.3 brotli backend.
