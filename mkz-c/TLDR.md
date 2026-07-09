# mkz (C): TL;DR

A pure-C port of `mkz`, a tar-style archiver whose backend is the reversible autocol
pre-pass plus zstd. Builds with just `cc` and libzstd, no Rust toolchain, for OpenBSD
ports and other broad portability.

```sh
make mkz
mkz -czf logs.mkz /var/log     # create
mkz -xf  logs.mkz out/         # extract
```

- Not a new compressor: a structural pre-pass that makes zstd 20-44% smaller on
  logs/CSV/JSONL, and never worse (per-block `min(zstd(raw), zstd(autocol))`).
- Full parity with the Rust `mkz`: creates, reads, encodes, streams. Archives interop
  bit-exact both ways, and the C encoder is byte-identical to the Rust one.
- Streaming, bounded memory (about one block) for typical line-oriented text/logs: 338 MB
  in gives about 80 MB peak on create, about 16 MB on extract. Newline-free input is the
  exception: it is buffered whole and not yet bounded.
- Bit-exact and integrity-gated: every archive carries an SHA-256; extraction verifies it
  over the whole stream and reports corruption on a mismatch. Extraction is not yet atomic,
  so a corrupt trailer can leave already-written files (temp+rename is planned).
- Audited decoder: untrusted bytes are fully bounds- and overflow-checked, path traversal
  is guarded, and it is ASan/UBSan-clean on valid and hostile input.
- One hard dependency (libzstd); SHA-256 and base-95 are vendored. MIT OR Apache-2.0,
- Say it with me now, thank you James! If you know one.

(c) Matthew Klein

!TLDR:  [README.md](README.md).
