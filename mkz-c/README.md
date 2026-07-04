# mkz (pure C)

A dependency-light C port of `mkz`, the tar-style archiver/compressor whose backend is the
[`autocol`](../crates/psrc-autocol) reversible pre-pass followed by zstd. This build
exists so the tool can ship where a Rust toolchain isn't wanted (OpenBSD ports, the other
BSDs, older Linux, embedded), building with nothing but `cc` and `libzstd`.

It is at full parity with the Rust `mkz`: it creates, reads, autocol-encodes, and streams.
Archives are byte-compatible in both directions, and the C encoder produces autocol blobs
that are byte-identical to the Rust encoder.

```
mkz -c[z[LEVEL]][v] -f <archive> <files-or-dirs...>   # create
mkz -x[v]           -f <archive> [destdir]            # extract (default dest: ".")
```

## What it does

`mkz` is not a new compressor. It is a reversible, bit-exact structural pre-pass that makes
a general compressor (zstd) smaller on structured, line-oriented data (logs, CSV, JSONL,
telemetry) and is never worse anywhere else:

1. Split input into line-aligned blocks.
2. For each block, run the auto-columnar transform: discover record structure with no
   schema (group lines by separator skeleton, fold constant fields into a template, gather
   the varying fields into per-column streams, delta-code numeric columns, dedup repeated
   values through a shared dictionary), producing one packed blob.
3. zstd both the raw block and the transformed blob, and keep whichever is smaller
   (`min(zstd(raw), zstd(autocol))`), the never-worse gate. The transform can only help or
   no-op.
4. Frame the chosen payload into a `PAS1` container and append a streaming SHA-256 of the
   reconstructed original (the SHA-256 integrity gate). Extraction verifies this SHA-256 over
   the whole stream and reports corruption on a mismatch; it is not yet atomic, so a corrupt
   trailer can leave already-written files in place (temp+rename is planned for a future release).

Measured 20–44% smaller than plain zstd on real logs, bit-exact. On unstructured or
already-compressed data it falls back to plain zstd, so it never loses.

## Build

The only hard external dependency is libzstd (SHA-256 and base-95 are vendored, so libzstd
is genuinely the only thing you need to link).

```sh
make                             # build the CLI (mkz)
make check                       # build + run the C↔C unit tests
make install PREFIX=/usr/local   # install mkz + mkz.1 (honors DESTDIR)
make dist                        # build the release tarball mkz-<V>.tar.gz (prints sha256/size)
make clean
```

The Makefile finds libzstd via `pkg-config` and falls back to `-lzstd`; it uses GNU make
(`gmake` on the BSDs). It compiles clean under `-Wall -Wextra -Wpedantic -std=c11`. To
build by hand with no make at all (say on a BSD where zstd lives under `/usr/local`):

```sh
cc -O2 -std=c11 -I/usr/local/include -o mkz \
   mkz.c stream.c pas1.c sha256.c autocol.c archive.c -L/usr/local/lib -lzstd
```

Targets Linux, the BSDs (including OpenBSD), macOS, and anything else with a C11 compiler
and libzstd. A man page (`mkz.1`) ships alongside.

### OpenBSD port

A ready-to-drop `archivers/mkz` port skeleton lives in [`openbsd-port/`](openbsd-port/)
(Makefile + `pkg/DESCR` + `pkg/PLIST`); its build compiles the C files directly (no GNU
make), depends only on `archivers/zstd`, and installs `bin/mkz` + `man/man1/mkz.1`. See
[`openbsd-port/README.port.md`](openbsd-port/README.port.md) for the three TODOs
(`MASTER_SITES`, `HOMEPAGE`, `MAINTAINER`) and `make makesum`.

## Usage

```sh
mkz -czf logs.mkz /var/log              # create from a directory
mkz -cz19vf big.mkz data/               # zstd level 19, verbose
mkz -xvf logs.mkz out/                  # extract into out/
mkz -xf  logs.mkz                        # extract into the current dir
```

| flag | meaning |
|------|---------|
| `-c` | create |
| `-x` | extract |
| `-z[LEVEL]` | zstd level 1–22 (default 12) |
| `-f <archive>` | archive filename (required) |
| `-v` | verbose (list entries) |

Environment: `PSRC_AC_ZSTD_LEVEL` (default 12), `PSRC_AC_BLOCK_MB` (default 16).

## Streaming / memory

Both create and extract stream line-oriented input in bounded memory for typical text/logs:
peak memory is roughly one block (plus the entry list, which scales with file count, not
size). Only one input file is open at a time. For example, a 338 MB log input compresses
with about 80 MB peak RSS and extracts with about 16 MB. Pathological newline-free input is
the exception: a block is read up to the next newline, so a file with no newlines is buffered
whole and is not yet bounded (a 200 MB newline-free file peaked ~1 GB RSS).

## Safety

The decoder is the security-critical surface, since it parses untrusted archive bytes, and
it is written defensively: every length, offset, and index is bounds- and overflow-checked
before use; the decompressed block size is bomb-capped; compressed payload lengths are
bounded against the actual input; the extract sink rejects path traversal (absolute paths,
`..`, `.`, empty, embedded NUL); and nothing is accepted unless the whole stream hashes
back to the original. The decode and streaming paths are built and exercised under
`-fsanitize=address,undefined` and are clean on both valid and hostile input
(`make mkz.asan`).

The encoder consumes trusted (local) data and is lower-risk, but it is still run under the
sanitizers.

## Format

`PAS1` container:

```
"PAS1"  ( [tag=1][flags u8][uvarint orig_len][uvarint comp_len][zstd payload] )*  [tag=0]  [32-byte SHA-256]
```

`flags` bit 0 set means the zstd payload decompresses to an autocol blob (otherwise it is
the raw block). The archived byte stream itself is a flat tar-style entry stream:

```
( [tag u8: 0=file / 1=dir][uvarint path_len][path]   ·   file only: [uvarint size][bytes] )*
```

The autocol blob format (templates, record→template ids, shared value dictionary, and
per-column codecs `0=raw / 1=zigzag-delta / 2=dict-ref`) is documented in the
[`autocol`](../crates/psrc-autocol) crate; this port is byte-for-byte compatible with
it.

A note on zstd interop: the Rust `mkz` does not pledge the frame content size into the zstd
header, so this port decompresses with the streaming API (a growing buffer) rather than
reading a size up front.

## Parity & testing

- C encoder output is byte-identical to the Rust `mkz transform` on edge cases and real
  logs (including a 13.6 MB `install.log`) plus fuzz inputs.
- Full archive interop is bit-exact in all three directions: C→C, C→Rust, Rust→C.
- `make check` runs the C↔C unit tests (base-95, b95u16, PAS1 container round-trip, and the
  paranoid decoder's rejections).

The Rust `mkz` (`../crates/psrc-mkz`) is the oracle; this port is validated against it.

## License

Dual-licensed under MIT OR Apache-2.0 (OpenBSD-ports clean). 

Copyright © 2026 Matthew Klein.

## Maintainer

Matthew Klein <mk@ntele.net>. Contributions welcome; by contributing you agree to the
MIT OR Apache-2.0 dual license. Changes to the on-disk format must land in both the C and
Rust encoders, which are byte-identical.

mkz got here because of James. If you know one, thank him.
