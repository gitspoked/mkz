# mkz roadmap

Post-0.1.0 ideuas - 0.1.0 ships the clean zstd archiver.

## 0.1.x (fast-follow, no format change)
- **Atomic extraction**: write files to a temp path and `rename()` into place only after the
  SHA-256 trailer verifies, so a corrupt archive leaves nothing half-written. (0.1.0 verifies the
  SHA *last*, after files are on disk; the README/CHANGELOG wording was corrected to say so.)

- **Reject unknown block-flag bits loudly**: the per-block `flags` byte uses only bit 0 (autocol).
  Make the decoder error on any bit it doesn't understand, so a future codec (see brotli below)
  fails clean on an old reader instead of feeding foreign bytes to zstd. Cheap forward-compat
  insurance; costs nothing today since every 0.1.0 block uses only bit 0.

## 0.2: read the old world (interop, decode-side only, NO format change)
- **Extract `.tar`, `.tar.gz`, `.tar.zst`**: teach `mkz -xf` to detect and unpack standard
  tarballs. Purely additive, never touches the mkz format. This is the zstd playbook: write your
  better format, read everyone else's. Gives adopters "it just works" experience.

- Note: native tar.gz *output* is intentionally NOT a goal; mkz's win (autocol's per-block
  columnar transform behind the never-worse gate) needs PAS1's per-block framing, which tar+gzip
  can't express. mkz is to tar.gz what zstd is to gzip: better, and deliberately its own format.

## 0.2: brotli codec (proven +12%, needs integration in BOTH impls)
- The corpus benchmark (`crates/psrc-corpus`) already measures brotli beating zstd on the target
  data (~0.216 vs zstd on logs). But brotli is ONLY a comparison baseline there (it shells out to
  the `brotli` CLI); it is **not** a codec in the archive format, in Rust **or** C. The shipping
  crate `psrc-mkz` depends on `zstd` only.

- Integration (an afternoon, not a port: `libbrotli` is already C; link it like libzstd):
  1. Link `libbrotlienc`/`libbrotlidec` (C) and the `brotli` crate (Rust).
  2. Claim block-flag bit 1 (or a 2-bit codec sub-field) for "payload is brotli".
  3. Extend the never-worse gate to also try `{raw-brotli, autocol+brotli}` and keep the smallest
     that round-trips; pure upside, brotli is only used where it actually wins.
  4. Decode-side dispatch on the flag, in both implementations, so C and Rust stay byte-compatible.

- Keep C<->Rust interop: both must gain brotli together, or a brotli archive from one won't open in
  the other. (Today both are zstd-only, so interop holds.)

- General cleanup/re-org...


