# The mkz format family (v0.1)

Byte-level specification of the formats implemented, byte-identically, by the Rust
(`crates/psrc-mkz`, `crates/psrc-autocol`) and C (`mkz-c/`) implementations. The two
implementations are the normative reference; where this document and both implementations
disagree, the implementations win. Decoders are the security surface: every length,
offset, and index in these formats arrives untrusted and MUST be bounds-checked.

Integer primitive used throughout:

**uvarint** is unsigned LEB128. Little-endian base-128: each byte carries 7 value bits,
bit 7 set means "more bytes follow". At most 10 bytes; readers MUST reject encodings that
overflow a u64.

---

## 1. PAS1: the stream container

A PAS1 stream carries one byte stream (of any content) as framed, individually-compressed
blocks with an end-to-end integrity trailer.

```
"PAS1"                                 4-byte magic
repeat:
  u8  tag                              1 = block record, 0 = end of blocks
  --- block record (tag = 1) ---
  u8      flags                        bit 0: payload decompresses to an autocol blob
                                       bits 1-7: reserved, writers MUST emit 0
  uvarint orig_len                     size of this block of the original stream
  uvarint payload_len
  bytes   payload[payload_len]         exactly one zstd frame
end (tag = 0)
u8[32]  sha256                         SHA-256 of the entire original byte stream
```

**Decoding a block:** zstd-decompress the payload (readers MUST cap the declared
decompressed size; both implementations refuse blocks claiming > 16 GiB). If `flags & 1`,
the decompressed bytes are an autocol blob (section 2), autocol-decode them. The result MUST
be exactly `orig_len` bytes. Concatenating all block results in order reconstructs the
original stream.

**Integrity:** the trailer is the SHA-256 of the reconstructed original. Readers MUST
verify it and treat a mismatch as fatal before reporting success. (Note the check
necessarily happens after decompression; a truncated or corrupted stream may cost work
before it is rejected, but never yields silently-wrong output.)

**Block boundaries are the writer's choice** and carry no meaning; readers accept any.
The reference writers cut blocks at ~1 MiB (configurable) extended to the next `\n`, so
blocks hold whole lines; the autocol transform requires line-complete input to help.

**The never-worse gate is writer policy, not format:** the reference writers keep the
autocol pre-pass for a block only when the transform verifiably round-trips AND
`len(zstd(autocol(block))) < len(zstd(block))`; otherwise they emit the plain-zstd block
with flags = 0. Any mix of gated/ungated blocks is a valid stream.

**Reserved flag bits:** readers MUST reject a block whose flags byte has any bit other
than bit 0 set (enforced since 0.1.3 in both implementations). Writers MUST NOT set
reserved bits. Allocation of future flag bits is governed by REGISTRY.md.

---

## 2. autocol blob: the columnar transform (FORMAT_VERSION 1)

A reversible, schema-free re-arrangement of line-oriented text such that a general
compressor sees per-column-homogeneous streams. `decode(encode(x)) == x`, bit-exact.

```
u8      version                        = 1; readers MUST reject other values
uvarint ntemplates
  x ntemplates: uvarint len, bytes[len]        line templates ("skeletons")
uvarint nlines
  x nlines:     uvarint template_id            per line, index into templates
uvarint ndict
  x ndict:      uvarint len, bytes[len]        shared value dictionary
uvarint ncolumns
  x ncolumns:
    u8 codec:
      0 = raw:   uvarint n, then n x { uvarint len, bytes[len] }        literal values
      1 = delta: uvarint n, then n x uvarint zigzag(v[i] - v[i-1])      numeric values,
                                                                        prev starts at 0
      2 = dict:  uvarint n, then n x uvarint dict_index                 refs into the
                                                                        shared dictionary
```

zigzag is the standard mapping `(n << 1) ^ (n >> 63)` on i64.

How an encoder tokenizes lines, groups them into templates, and assigns column codecs is
**encoder freedom** (quality, not conformance); what a decoder must accept is exactly the
layout above. All indices (template_id, dict_index) MUST be range-checked; any
out-of-range value, truncation, or trailing garbage makes the blob invalid.

---

## 3. mkz archive: the entry stream

A `.mkz` file is a PAS1 stream (section 1) whose reconstructed content is an **entry stream**: a
flat concatenation of entries.

```
repeat until end of stream:
  u8      tag                          0 = file, 1 = directory
  uvarint pathlen
  bytes   path[pathlen]                relative path, '/'-separated
  --- file only ---
  uvarint size
  bytes   content[size]
```

Writers emit each directory before its name-sorted children and store normalized
relative paths. Extractors MUST reject unsafe paths: absolute paths, empty paths, any
`.` or `..` component, or an embedded NUL. Symlinks and special files are not
represented in v0.1.

---

## 4. base95: printable fixed-width u64 (foundation, not yet used by sections 1-3)

`u64 -> exactly 10 bytes`, each in `0x20..0x7E` (95 printable ASCII values, space
through `~`). Digits are most-significant-first with byte value `0x20 + digit`
(95^10 > 2^64, so every u64 fits). Properties: lossless; `memcmp` order on encodings
equals numeric order; framing-safe (no byte below 0x20). Decoders MUST reject
out-of-range bytes and values that overflow u64.

## 5. b95u16: printable fixed-width UTF-16 text (foundation, not yet used by sections 1-3)

Each UTF-16 **code unit** (u16) -> exactly 3 bytes from the same 95-character alphabet
(95^3 = 857375 > 65536); a text of n units encodes to exactly 3n bytes. Operates on raw
code units; lossless including unpaired surrogates; sorts in code-unit order;
framing-safe. Decoders MUST reject a length not divisible by 3, out-of-range bytes, and
groups decoding above 0xFFFF.

---

## Versioning

- PAS1 has no version field; incompatible container changes will use a new magic.
- The autocol blob carries an explicit version byte (currently 1).
- Readers reject unknown flag bits since 0.1.3; this is the forward-compat gate that
  future format work (codec ids, PAS2 segment kinds) relies on. Planned allocations live
  in REGISTRY.md and ROADMAP.md.
