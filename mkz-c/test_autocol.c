/* test_autocol.c — decode a Rust-produced autocol blob and compare to the original.
 * usage: test_autocol <blob.ac> <expected>   (blob = output of `mkz transform expected blob.ac`)
 * Built with -fsanitize=address,undefined so the decoder is exercised for memory safety.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "autocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, uint8_t **buf, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!b) { fclose(f); return -1; }
    size_t rd = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(b); return -1; }
    *buf = b; *len = (size_t)sz;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <blob.ac> <expected>\n", argv[0]); return 2; }
    uint8_t *blob = NULL, *exp = NULL, *got = NULL;
    size_t bl = 0, el = 0, gl = 0;
    if (read_file(argv[1], &blob, &bl)) { fprintf(stderr, "read blob failed\n"); return 2; }
    if (read_file(argv[2], &exp, &el)) { fprintf(stderr, "read expected failed\n"); free(blob); return 2; }

    int rc = mkz_autocol_decode(blob, bl, &got, &gl);
    int ok = (rc == 0 && gl == el && (el == 0 || memcmp(got, exp, el) == 0));
    printf("  %-28s %s (decoded %zu B, expected %zu B)\n",
           argv[2], ok ? "BIT-EXACT" : "MISMATCH", gl, el);

    free(blob); free(exp); free(got);
    return ok ? 0 : 1;
}
