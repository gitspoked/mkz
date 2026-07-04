/* b95u16.c — base-95-of-UTF-16 text encoding for mkz (C port).
 * Byte-identical to the Rust psrc_autocol b95u16 module.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "b95u16.h"

#define BASE   95u
#define OFFSET 0x20

void mkz_b95u16_encode_u16(uint16_t val, char out[MKZ_U16_WIDTH]) {
    out[0] = (char)OFFSET;
    out[1] = (char)OFFSET;
    out[2] = (char)OFFSET;
    uint32_t v = val;
    int pos = MKZ_U16_WIDTH;
    while (v > 0 && pos > 0) {
        pos--;
        out[pos] = (char)(OFFSET + (v % BASE));
        v /= BASE;
    }
}

int mkz_b95u16_decode_u16(const char in[MKZ_U16_WIDTH], uint16_t *out) {
    uint32_t val = 0;
    for (int i = 0; i < MKZ_U16_WIDTH; i++) {
        unsigned char b = (unsigned char)in[i];
        if (b < OFFSET || b > 0x7E) {
            return -1;
        }
        val = val * BASE + (uint32_t)(b - OFFSET);
    }
    if (val > 0xFFFFu) {
        return -1; /* corrupt: 3 base-95 chars can reach 857374, but a u16 maxes at 65535 */
    }
    *out = (uint16_t)val;
    return 0;
}

void mkz_b95u16_encode_units(const uint16_t *units, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) {
        mkz_b95u16_encode_u16(units[i], out + i * MKZ_U16_WIDTH);
    }
}

int mkz_b95u16_decode_units(const char *in, size_t in_len, uint16_t *out, size_t *out_n) {
    if (in_len % MKZ_U16_WIDTH != 0) {
        return -1;
    }
    size_t n = in_len / MKZ_U16_WIDTH;
    for (size_t i = 0; i < n; i++) {
        if (mkz_b95u16_decode_u16(in + i * MKZ_U16_WIDTH, &out[i]) != 0) {
            return -1;
        }
    }
    *out_n = n;
    return 0;
}
