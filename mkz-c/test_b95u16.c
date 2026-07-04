/* test_b95u16.c — verify the C b95u16 codec is byte-identical to Rust mkz.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "b95u16.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
    char buf[MKZ_U16_WIDTH];
    uint16_t back;

    /* Rust oracle anchors (same base-95 big-endian algorithm) */
    mkz_b95u16_encode_u16(0, buf);
    CHECK(memcmp(buf, "   ", 3) == 0, "0 -> 3 spaces");
    mkz_b95u16_encode_u16(1, buf);
    CHECK(memcmp(buf, "  !", 3) == 0, "1 -> '  !'");
    mkz_b95u16_encode_u16(65535, buf);
    CHECK(memcmp(buf, "'8p", 3) == 0, "65535 -> \"'8p\" (7,24,80)");

    /* full 0..=65535 sweep: roundtrip + all printable (mirrors Rust full_sweep) */
    for (uint32_t v = 0; v <= 0xFFFF; v++) {
        mkz_b95u16_encode_u16((uint16_t)v, buf);
        for (int i = 0; i < 3; i++) {
            unsigned char b = (unsigned char)buf[i];
            CHECK(b >= 0x20 && b <= 0x7E, "printable");
            if (b < 0x20 || b > 0x7E) break;
        }
        CHECK(mkz_b95u16_decode_u16(buf, &back) == 0 && back == (uint16_t)v, "sweep roundtrip");
    }

    /* sortable in code-unit order */
    uint16_t sv[] = {0, 1, 94, 95, 1000, 40000, 0xD7FF, 0xD800, 0xE000, 0xFFFF};
    char prev[3], cur[3];
    mkz_b95u16_encode_u16(sv[0], prev);
    for (size_t i = 1; i < sizeof(sv)/sizeof(sv[0]); i++) {
        mkz_b95u16_encode_u16(sv[i], cur);
        CHECK(memcmp(prev, cur, 3) < 0, "sort order");
        memcpy(prev, cur, 3);
    }

    /* lone surrogates + an astral pair round-trip as raw u16 (WTF-16 fidelity) */
    uint16_t units[] = {0xD800, 0x0041, 0xDFFF, 0xD83C, 0xDFA9 /* top hat pair */};
    char enc[5 * 3];
    uint16_t dec[5]; size_t dn = 0;
    mkz_b95u16_encode_units(units, 5, enc);
    CHECK(mkz_b95u16_decode_units(enc, sizeof(enc), dec, &dn) == 0 && dn == 5
          && memcmp(units, dec, sizeof(units)) == 0, "units roundtrip incl surrogates");

    /* rejects */
    uint16_t d;
    char bad[3] = {0x1F, 0x20, 0x20};
    CHECK(mkz_b95u16_decode_u16(bad, &d) != 0, "reject 0x1F");
    char ov[3] = {0x7E, 0x7E, 0x7E}; /* 857374 > 65535 */
    CHECK(mkz_b95u16_decode_u16(ov, &d) != 0, "reject > u16 max");
    size_t dn2;
    CHECK(mkz_b95u16_decode_units("ab", 2, dec, &dn2) != 0, "reject non-multiple-of-3");

    if (fails == 0) { printf("b95u16 C: ALL PASS (byte-identical to Rust)\n"); return 0; }
    printf("b95u16 C: %d FAIL\n", fails);
    return 1;
}
