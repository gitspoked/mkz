/* test_base95.c — verify the C base-95 codec is byte-identical to Rust mkz.
 * Anchors: Rust test_zero (0 -> 10 spaces) and test_one (1 -> 9 spaces + '!').
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "base95.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
    char buf[MKZ_U64_WIDTH];
    uint64_t back, dummy;

    /* Rust oracle: encode_u64(0) == [0x20; 10] */
    mkz_base95_encode_u64(0, buf);
    CHECK(memcmp(buf, "          ", 10) == 0, "0 -> 10 spaces (Rust test_zero)");

    /* Rust oracle: encode_u64(1) == 9 spaces + '!' (0x21) */
    mkz_base95_encode_u64(1, buf);
    CHECK(memcmp(buf, "         !", 10) == 0, "1 -> spaces + '!' (Rust test_one)");

    /* roundtrip (Rust test_roundtrip_various) */
    uint64_t vals[] = {0, 1, 94, 95, 96, 9024, 1000000ULL,
                       0xFFFFFFFFULL, UINT64_MAX / 2, UINT64_MAX - 1, UINT64_MAX};
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        mkz_base95_encode_u64(vals[i], buf);
        CHECK(mkz_base95_decode_u64(buf, &back) == 0 && back == vals[i], "roundtrip");
    }

    /* lexicographic order == numeric order (Rust test_lexicographic_order) */
    uint64_t sorted[] = {0, 1, 94, 95, 100, 1000, 10000, UINT64_MAX / 2, UINT64_MAX};
    char prev[MKZ_U64_WIDTH], cur[MKZ_U64_WIDTH];
    mkz_base95_encode_u64(sorted[0], prev);
    for (size_t i = 1; i < sizeof(sorted) / sizeof(sorted[0]); i++) {
        mkz_base95_encode_u64(sorted[i], cur);
        CHECK(memcmp(prev, cur, 10) < 0, "lexicographic order");
        memcpy(prev, cur, 10);
    }

    /* reject out-of-range byte (Rust test_decode_invalid_byte) */
    mkz_base95_encode_u64(42, buf);
    buf[5] = 0x7F; /* DEL */
    CHECK(mkz_base95_decode_u64(buf, &dummy) != 0, "reject 0x7F");

    /* reject overflow: '~~~~~~~~~~' = 95^10 - 1 > u64::MAX */
    char ov[MKZ_U64_WIDTH];
    memset(ov, 0x7E, MKZ_U64_WIDTH);
    CHECK(mkz_base95_decode_u64(ov, &dummy) != 0, "reject overflow");

    if (fails == 0) {
        printf("base95 C: ALL PASS (byte-identical to Rust)\n");
        return 0;
    }
    printf("base95 C: %d FAIL\n", fails);
    return 1;
}
