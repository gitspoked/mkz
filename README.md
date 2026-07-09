# mkz

A streaming, tar-style archiver/compressor: a reversible **autocol** pre-pass feeding
zstd, behind a **never-worse gate** and an end-to-end **SHA-256 integrity check**.
Measurably smaller on structured, line-oriented data (logs, CSV, JSONL); bit-exact
always; never larger than zstd alone.

```
mkz -c[z[LEVEL]][v] -f <archive> <files-or-dirs...>   # create
mkz -x[v]           -f <archive> [destdir]            # extract (default dest: ".")
```

## What's in this repo

| Path | What it is |
|---|---|
| [`crates/psrc-mkz`](crates/psrc-mkz) | the `mkz` CLI (Rust), [crates.io/crates/mkz](https://crates.io/crates/mkz) |
| [`crates/psrc-autocol`](crates/psrc-autocol) | the `autocol` library, [crates.io/crates/autocol](https://crates.io/crates/autocol) |
| [`mkz-c`](mkz-c) | pure-C port of the tool (cc + libzstd, nothing else), byte-compatible with the Rust build in both directions |

The `psrc-` prefix marks the crates' origin in the PSRC compression research project;
`mkz` and `autocol` are its shippable products.

## How it works

1. **autocol** reshapes line-oriented input so a general compressor sees
   per-column-homogeneous streams. It is a reversible transform, not a compressor:
   `decode(encode(x)) == x`, bit-exact.
2. **The never-worse gate**: each block keeps the autocol pre-pass only when the
   transform verifiably round-trips *and* the zstd output is strictly smaller than
   plain zstd on the same block; otherwise the block falls back to plain zstd.
3. **Integrity**: archives carry an end-to-end SHA-256, verified on extract.

## Install

```sh
cargo install mkz            # Rust build
# or the C port:
cd mkz-c && make && ./install.sh
```

## License

Dual-licensed under [MIT](LICENSE-MIT) or [Apache-2.0](LICENSE-APACHE), at your option.
