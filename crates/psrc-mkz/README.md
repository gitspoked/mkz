# mkz

mkz is a reversible pre-pass for zstd. It reshapes line-oriented data (logs, CSV,
JSONL, metrics) so the compressor sees per-column-homogeneous streams instead of
row-interleaved noise, then hands that to zstd. Two gates keep it honest: a size gate,
so the output is never larger than plain zstd, and a SHA-256 gate, so it round-trips
bit for bit. It streams line-oriented input in bounded memory for typical text/logs;
newline-free input is the exception and is buffered whole (not yet bounded).

It is not a compressor of its own. The entropy coding is still zstd's; mkz just gives
zstd better-shaped input.

The crate is `mkz`; the transform underneath is
[`autocol`](https://crates.io/crates/autocol). The CLI is tar-style: `-c` packs a
compressed archive, `-x` extracts it.

## Numbers

Real macOS `/var/log` files, measured with the PSRC benchmark harness. Ratio is
compressed / original (lower is better); every file was verified bit-exact.

|                        |  zstd | mkz ac |   %   |
| files tested           | alone | > zstd |smaller|
|------------------------|-------/--------|-------|
| `install.log` (12 MB)  | 0.024 | 0.012  |  50%  |
| `fsck_apfs.log`        | 0.030 | 0.018  |  40%  |
| `fsck_hfs.log`         | 0.031 | 0.022  |  29%  |
| `shutdown_monitor.log` | 0.064 | 0.054  |  16%  |

The gain is structural: per-column homogeneity, delta-coded numeric columns, and
cross-column value dedup that a row-by-row compressor can't see. On data with no such
structure (prose, source, already-compressed, random) mkz falls back to plain zstd, so
the output is never larger than zstd by itself. How much you gain depends on how
structured your data is; the fallback floor does not move.

## Install

```sh
cargo install mkz              # installs the `mkz` binary
# or, from a checkout:
cargo build --release -p mkz   # -> target/release/mkz
```

## Use

mkz is tar-style: it packs files or directories into one compressed archive.

```sh
mkz -czf logs.mkz /var/log      # create (c) a zstd (z) archive into file (f) logs.mkz
mkz -xf  logs.mkz out/          # extract (x) into out/ (default: current dir)
mkz -cz19vf big.mkz data/       # zstd level 19, verbose
```

Extraction is bit-exact and verifies a SHA-256 over the whole stream, reporting corruption
on a mismatch. Extracts are staged and only placed once the archive verifies (details
under Guarantees below). For the raw transform with no backend, to pipe into your own
coder:

```sh
mkz transform   app.log     app.log.ac
mkz untransform app.log.ac  app.log
```

## Guarantees

- Bit-exact. Every artifact carries a SHA-256 of the original. `decompress` recomputes
  it over the whole stream and reports corruption on a mismatch, so corruption is caught.
  Extraction is atomic: entries stage into `<dest>/.mkz-partial.<pid>` and land in `<dest>`
  only after the SHA-256 trailer verifies; a failure before that point places nothing, a
  failure during the final move can leave a partial merge, and the staging directory is
  retained and reported either way.
- Never worse. Each block ships whichever is smaller of `autocol->zstd` and `zstd`. If
  the transform doesn't help, the block is plain zstd, and output is never larger than
  zstd alone.
- Bounded memory for typical text/logs. Streaming, line-aligned blocks: peak RAM is about
  one block for line-oriented input. A 1 GB log holds a flat ~390 MB resident set. The
  exception is newline-free input: a block extends to the next newline, so a file with no
  newlines is buffered whole and is not yet bounded (a 200 MB newline-free file peaked
  ~1 GB RSS).

## How it works

1. Split each line into whole-token words plus separators, and group lines by skeleton
   (their separator sequence).
2. Fold word positions that are constant across a group into a template; the positions
   that vary become columns.
3. Code each column with the best of `{raw, zigzag-delta, global-dictionary}`, chosen
   per column, so a monotone timestamp column stays delta-coded and doesn't get
   dictionary-poisoned.
4. Pack `[templates][row->template ids][value dict][columns]` into one blob, then zstd it.

Reversible by construction (`decode(encode(x)) == x`), checked by property tests and
fuzzing.

## Tuning

| environment variabl  | default | effect                                           |
|----------------------|---------|--------------------------------------------------|
| `PSRC_AC_BLOCK_MB`   |   `16`  | block size; bigger = deeper columns, more memory |
| `PSRC_AC_ZSTD_LEVEL` |   `12`  | zstd level 1-22; higher = smaller and slower     |

## Scope

It improves zstd on structured, line-oriented data: bit-exact, never worse, streaming.
It is not a new entropy coder, a zstd or brotli replacement, or a `.zip` producer. The
container is a self-describing `PAS1` stream, and the entropy coding stays with zstd;Chad
mkz only shapes the input.

## Status

`v0.1.0`. Single-threaded, zstd backend. The transform is backend-agnostic (its output is
just bytes any coder can take), though mkz itself ships zstd. Columns currently span one
block; file-spanning column depth via larger super-blocks is on the roadmap. Format `PAS1`.

## License

Licensed under either the Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE)) 
or the MIT license ([LICENSE-MIT](LICENSE-MIT)) at your option. Unless you state otherwise, 
any contribution you submit for inclusion inthis work shall be dual licensed as above, 
with no additional terms.

The MIT option is permissive and has no patent clause, which keeps it friendly for
BSD-style distributions such as OpenBSD ports.

---

mkz got here because of James; if you know one, thank him.

-m
