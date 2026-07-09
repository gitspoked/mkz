/* base95.c - fixed-width base-95 integer encoding for mkz (C port).
 * Byte-identical to the Rust psrc_autocol base-95 encoder.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "base95.h"

#define BASE   95u
#define OFFSET 0x20 /* ASCII space = digit 0 */

void mkz_base95_encode_u64(uint64_t val, char out[MKZ_U64_WIDTH]) {
    /* left-pad with spaces (digit 0) */
    for (int i = 0; i < MKZ_U64_WIDTH; i++) {
        out[i] = (char)OFFSET;
    }
    uint64_t v = val;
    int pos = MKZ_U64_WIDTH;
    while (v > 0 && pos > 0) {
        pos--;
        out[pos] = (char)(OFFSET + (v % BASE));
        v /= BASE;
    }
}

int mkz_base95_decode_u64(const char in[MKZ_U64_WIDTH], uint64_t *out) {
    uint64_t val = 0;
    for (int i = 0; i < MKZ_U64_WIDTH; i++) {
        unsigned char b = (unsigned char)in[i];
        if (b < OFFSET || b > 0x7E) {
            return -1; /* byte out of printable-ASCII range */
        }
        uint64_t d = (uint64_t)(b - OFFSET);
        /* checked_mul + checked_add, no __int128 (portable): would val*95+d overflow? */
        if (val > (UINT64_MAX - d) / BASE) {
            return -1;
        }
        val = val * BASE + d;
    }
    *out = val;
    return 0;
}
