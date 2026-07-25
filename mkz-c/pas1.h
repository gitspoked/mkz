/* pas1.h - PAS1 stream container for mkz (C port): byte stream <-> framed zstd blocks
 * + SHA-256 trailer. Byte-format-identical to the Rust mkz compress_stream/decompress_stream.
 *
 * Compress applies the autocol pre-pass per block behind a never-worse gate (autocol only
 * kept when it round-trips and zstd's strictly smaller than raw), exactly like the Rust
 * compress_stream. Decompress consumes both raw (flags=0) and autocol (flags bit0) blocks.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef MKZ_PAS1_H
#define MKZ_PAS1_H

#include <stdint.h>
#include <stddef.h>

/* Compress data into a PAS1 stream. mallocs *out (caller frees). 0 on success, -1 on failure.
 * block_size = uncompressed bytes per block (0 -> default 1 MiB). */
int mkz_pas1_compress(const uint8_t *data, size_t len, int zstd_level, size_t block_size,
                      uint8_t **out, size_t *out_len);

/* Decompress a PAS1 stream. mallocs *out (caller frees). 0 on success, -1 on ANY
 * malformed / truncated / overflowing / corrupt (SHA mismatch) / unsupported input.
 * Every length and offset is bounds- and overflow-checked before use. */
int mkz_pas1_decompress(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/* Per-block primitives, shared by the in-memory and the streaming codecs so the never-worse
 * gate and the audited decode live in exactly one place. */

/* Create-side statistics, accumulated per encoded block. The never-worse gate already
 * compresses both candidates, so `zstd_alone_bytes` (what plain zstd would have cost)
 * comes for free; saved = zstd_alone_bytes - payload_bytes, always >= 0 by the gate. */
struct mkz_pas1_stats {
    uint64_t blocks;           /* blocks encoded */
    uint64_t autocol_blocks;   /* blocks where the gate kept the autocol pre-pass */
    uint64_t orig_bytes;       /* total uncompressed stream bytes */
    uint64_t payload_bytes;    /* total chosen compressed payload bytes */
    uint64_t zstd_alone_bytes; /* total plain-zstd candidate bytes */
};

/* Encode one block (raw-zstd + autocol gate). Sets *flags (bit0 = autocol), mallocs *payload
 * (caller frees). If `st` is non-NULL, accumulates create statistics into it. 0 / -1. */
int mkz_pas1_encode_block_stats(const uint8_t *data, size_t len, int level,
                                uint8_t *flags, uint8_t **payload, size_t *payload_len,
                                struct mkz_pas1_stats *st);

/* Encode one block (raw-zstd + autocol gate). Sets *flags (bit0 = autocol), mallocs *payload
 * (caller frees). 0 / -1. */
int mkz_pas1_encode_block(const uint8_t *data, size_t len, int level,
                          uint8_t *flags, uint8_t **payload, size_t *payload_len);

/* Decode one block payload back to original bytes (zstd-decompress, autocol-decode if flags&1,
 * verify length == orig_len; orig_len is bomb-capped internally). mallocs *out (caller frees).
 * The caller must bound `payload_len` against the actual available input. 0 / -1. */
int mkz_pas1_decode_block(const uint8_t *payload, size_t payload_len, uint8_t flags,
                          uint64_t orig_len, uint8_t **out, size_t *out_len);

/* Reusable decode scratch for callers that decode many blocks in a loop (the streaming
 * extractor). Reusing the same scratch across calls avoids rebuilding the decode buffers
 * from empty every block: measured on this platform's allocator, that pattern leaves
 * roughly one block's worth of resident memory behind PER BLOCK (never reclaimed until
 * process exit), so peak RSS grew linearly with the number of blocks processed instead of
 * staying flat (O(block), as intended). Create once per extraction, reuse for every block,
 * free once when done. */
struct mkz_pas1_scratch;
struct mkz_pas1_scratch *mkz_pas1_scratch_new(void);
void mkz_pas1_scratch_free(struct mkz_pas1_scratch *s);

/* Same contract as mkz_pas1_decode_block, but reuses `scratch`'s buffers across calls.
 * *out is BORROWED from scratch: valid until the next call on the same scratch, or until
 * mkz_pas1_scratch_free. The caller must NOT free(*out). 0 / -1. */
int mkz_pas1_decode_block_into(const uint8_t *payload, size_t payload_len, uint8_t flags,
                               uint64_t orig_len, struct mkz_pas1_scratch *scratch,
                               uint8_t **out, size_t *out_len);

#endif /* MKZ_PAS1_H */
