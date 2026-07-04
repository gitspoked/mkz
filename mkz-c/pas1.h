/* pas1.h — PAS1 stream container for mkz (C port): byte stream <-> framed zstd blocks
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

/* Encode one block (raw-zstd + autocol gate). Sets *flags (bit0 = autocol), mallocs *payload
 * (caller frees). 0 / -1. */
int mkz_pas1_encode_block(const uint8_t *data, size_t len, int level,
                          uint8_t *flags, uint8_t **payload, size_t *payload_len);

/* Decode one block payload back to original bytes (zstd-decompress, autocol-decode if flags&1,
 * verify length == orig_len; orig_len is bomb-capped internally). mallocs *out (caller frees).
 * The caller must bound `payload_len` against the actual available input. 0 / -1. */
int mkz_pas1_decode_block(const uint8_t *payload, size_t payload_len, uint8_t flags,
                          uint64_t orig_len, uint8_t **out, size_t *out_len);

#endif /* MKZ_PAS1_H */
