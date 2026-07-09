/* test_pas1.c - SHA-256 vectors + PAS1 container C<->C roundtrip + the paranoid
 * decoder's rejections. SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "pas1.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static int sha_is(const char *s, size_t n, const uint8_t want[32]) {
    uint8_t got[32];
    mkz_sha256((const uint8_t *)s, n, got);
    return memcmp(got, want, 32) == 0;
}

static void roundtrip(const uint8_t *data, size_t len, size_t block, const char *msg) {
    uint8_t *comp = NULL, *back = NULL;
    size_t clen = 0, blen = 0;
    int ce = mkz_pas1_compress(data, len, 12, block, &comp, &clen);
    CHECK(ce == 0, msg);
    if (ce) return;
    int de = mkz_pas1_decompress(comp, clen, &back, &blen);
    CHECK(de == 0 && blen == len && (len == 0 || memcmp(back, data, len) == 0), msg);
    free(comp);
    free(back);
}

int main(void) {
    /* SHA-256 known-answer vectors */
    static const uint8_t sha_abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    static const uint8_t sha_empty[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55};
    CHECK(sha_is("abc", 3, sha_abc), "sha256(\"abc\")");
    CHECK(sha_is("", 0, sha_empty), "sha256(\"\")");

    /* roundtrips: empty, tiny, and multi-block (block=16 forces many blocks) */
    roundtrip((const uint8_t *)"", 0, 64, "empty roundtrip");
    roundtrip((const uint8_t *)"hello world\n", 12, 64, "tiny roundtrip");

    /* a ~50 KB structured buffer, forced into many small blocks */
    size_t n = 50000;
    uint8_t *big = (uint8_t *)malloc(n);
    for (size_t i = 0; i < n; i++) big[i] = (uint8_t)("ABCDEFGH\n"[i % 9]);
    roundtrip(big, n, 16, "multi-block roundtrip (block=16)");
    roundtrip(big, n, 1u << 20, "single-block roundtrip");

    /* incompressible (random-ish) data still round-trips */
    uint32_t s = 0x12345678u;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; big[i] = (uint8_t)s; }
    roundtrip(big, n, 4096, "incompressible roundtrip");

    /* --- paranoid decoder rejections --- */
    uint8_t *comp = NULL, *back = NULL; size_t clen = 0, blen = 0;
    mkz_pas1_compress(big, n, 12, 4096, &comp, &clen);

    /* corrupt a payload byte -> SHA mismatch -> reject */
    uint8_t *corrupt = (uint8_t *)malloc(clen);
    memcpy(corrupt, comp, clen);
    corrupt[clen / 2] ^= 0xff;
    CHECK(mkz_pas1_decompress(corrupt, clen, &back, &blen) != 0, "reject corrupted payload");
    free(corrupt);

    /* corrupt the SHA trailer -> reject */
    corrupt = (uint8_t *)malloc(clen);
    memcpy(corrupt, comp, clen);
    corrupt[clen - 1] ^= 0xff;
    CHECK(mkz_pas1_decompress(corrupt, clen, &back, &blen) != 0, "reject corrupted trailer");
    free(corrupt);

    /* truncated stream -> reject (no out-of-bounds read) */
    CHECK(mkz_pas1_decompress(comp, clen / 2, &back, &blen) != 0, "reject truncated");
    /* bad magic -> reject */
    CHECK(mkz_pas1_decompress((const uint8_t *)"NOPExxxx", 8, &back, &blen) != 0, "reject bad magic");
    /* too short for magic -> reject */
    CHECK(mkz_pas1_decompress((const uint8_t *)"PA", 2, &back, &blen) != 0, "reject < magic");

    free(comp);
    free(big);

    if (fails == 0) { printf("pas1 C: ALL PASS (sha vectors + container roundtrip + paranoid rejects)\n"); return 0; }
    printf("pas1 C: %d FAIL\n", fails);
    return 1;
}
