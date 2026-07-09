/* base95.h - fixed-width base-95 integer encoding for mkz (C port).
 *
 * u64 -> exactly 10 printable-ASCII bytes (0x20..0x7E), lossless (95^10 > 2^64),
 * lexicographically sortable (memcmp == numeric). Byte-identical to the Rust
 * `psrc_autocol`/`mkz` base-95 encoder; this is the interop contract.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef MKZ_BASE95_H
#define MKZ_BASE95_H

#include <stdint.h>
#include <stddef.h>

#define MKZ_U64_WIDTH 10

/* Encode `val` into exactly MKZ_U64_WIDTH bytes at `out` (no NUL terminator). */
void mkz_base95_encode_u64(uint64_t val, char out[MKZ_U64_WIDTH]);

/* Decode MKZ_U64_WIDTH bytes at `in` into *out.
 * Returns 0 on success, -1 if any byte is out of range or the value overflows u64. */
int mkz_base95_decode_u64(const char in[MKZ_U64_WIDTH], uint64_t *out);

#endif /* MKZ_BASE95_H */
