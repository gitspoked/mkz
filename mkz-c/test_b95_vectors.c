/* test_b95_vectors.c - pin the C base-95 codecs to the shared ABI vector file.
 * Reads test_vectors_b95.txt (same directory) and checks every line both ways:
 * encode(value) must produce exactly the listed bytes, decode(bytes) must return
 * exactly the value. Qubed pins its Rust encoders to a copy of the same file.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "base95.h"
#include "b95u16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(cond, line, msg) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", line, msg); fails++; } \
} while (0)

/* Decode a hex string into bytes. Returns the byte count, or -1 on bad input. */
static int unhex(const char *hex, unsigned char *out, size_t cap) {
    size_t len = strlen(hex);
    if (len % 2 != 0 || len / 2 > cap) {
        return -1;
    }
    for (size_t i = 0; i < len; i += 2) {
        unsigned v;
        if (sscanf(hex + i, "%2x", &v) != 1) {
            return -1;
        }
        out[i / 2] = (unsigned char)v;
    }
    return (int)(len / 2);
}

int main(void) {
    const char *path = "test_vectors_b95.txt";
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("FAIL: cannot open %s (run from the source directory)\n", path);
        return 1;
    }

    char linebuf[256];
    int lineno = 0, vectors = 0;
    while (fgets(linebuf, sizeof(linebuf), f)) {
        lineno++;
        if (linebuf[0] == '#' || linebuf[0] == '\n') {
            continue;
        }
        char kind[8], hex[64];
        unsigned long long val;
        if (sscanf(linebuf, "%7s %llu %63s", kind, &val, hex) != 3) {
            CHECK(0, lineno, "unparseable vector line");
            continue;
        }
        unsigned char want[16];
        int n = unhex(hex, want, sizeof(want));

        if (strcmp(kind, "u64") == 0) {
            CHECK(n == MKZ_U64_WIDTH, lineno, "u64 vector is not 10 bytes");
            char enc[MKZ_U64_WIDTH];
            mkz_base95_encode_u64((uint64_t)val, enc);
            CHECK(memcmp(enc, want, MKZ_U64_WIDTH) == 0, lineno, "u64 encode mismatch");
            uint64_t back;
            CHECK(mkz_base95_decode_u64((const char *)want, &back) == 0 && back == val,
                  lineno, "u64 decode mismatch");
        } else if (strcmp(kind, "u16") == 0) {
            CHECK(n == MKZ_U16_WIDTH, lineno, "u16 vector is not 3 bytes");
            CHECK(val <= 0xFFFF, lineno, "u16 value out of range");
            char enc[MKZ_U16_WIDTH];
            mkz_b95u16_encode_u16((uint16_t)val, enc);
            CHECK(memcmp(enc, want, MKZ_U16_WIDTH) == 0, lineno, "u16 encode mismatch");
            uint16_t back;
            CHECK(mkz_b95u16_decode_u16((const char *)want, &back) == 0 && back == val,
                  lineno, "u16 decode mismatch");
        } else {
            CHECK(0, lineno, "unknown vector kind");
            continue;
        }
        vectors++;
    }
    fclose(f);

    CHECK(vectors >= 22, 0, "vector file lost entries (expected at least 22)");

    if (fails == 0) {
        printf("b95 vectors C: ALL PASS (%d vectors, ABI pinned)\n", vectors);
        return 0;
    }
    printf("b95 vectors C: %d FAIL\n", fails);
    return 1;
}
