/* b95u16.h — base-95-of-UTF-16 text encoding for mkz (C port).
 *
 * UTF-16 code unit (u16) <-> exactly 3 printable-ASCII bytes (95^3 = 857375 > 65536).
 * Lossless incl. lone surrogates (operates on raw u16, never a "string"), sortable in
 * code-unit order, framing-safe (min byte 0x20). Byte-identical to the Rust b95u16.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef MKZ_B95U16_H
#define MKZ_B95U16_H

#include <stdint.h>
#include <stddef.h>

#define MKZ_U16_WIDTH 3

/* Encode one code unit into exactly 3 bytes at `out` (no NUL). */
void mkz_b95u16_encode_u16(uint16_t val, char out[MKZ_U16_WIDTH]);

/* Decode 3 bytes into *out. Returns 0, or -1 on out-of-range byte / value > 0xFFFF. */
int mkz_b95u16_decode_u16(const char in[MKZ_U16_WIDTH], uint16_t *out);

/* Encode `n` code units into `out` (caller must size out >= n*3). */
void mkz_b95u16_encode_units(const uint16_t *units, size_t n, char *out);

/* Decode `in_len` bytes (must be a multiple of 3) into `out` (caller sized >= in_len/3).
 * Writes the unit count to *out_n. Returns 0 on success, -1 on bad length / bad group. */
int mkz_b95u16_decode_units(const char *in, size_t in_len, uint16_t *out, size_t *out_n);

#endif /* MKZ_B95U16_H */
