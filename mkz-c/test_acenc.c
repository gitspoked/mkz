/* test_acenc.c — read a file, run mkz_autocol_encode, write the blob.
 * Used to diff the C blob byte-for-byte against `mkz transform` (the Rust oracle) and to
 * feed `mkz untransform` (Rust decode of a C-produced blob). Built with sanitizers.
 *   test_acenc <input> <out.ac>
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "autocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int slurp(const char *path, uint8_t **buf, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!b) { fclose(f); return -1; }
    size_t r = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(b); return -1; }
    *buf = b; *len = (size_t)sz;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <input> <out.ac>\n", argv[0]); return 2; }
    uint8_t *in = NULL; size_t in_len = 0;
    if (slurp(argv[1], &in, &in_len)) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }

    uint8_t *blob = NULL; size_t blob_len = 0;
    if (mkz_autocol_encode(in, in_len, &blob, &blob_len)) { fprintf(stderr, "encode failed\n"); free(in); return 1; }

    /* self-check: our own decoder must invert the blob exactly */
    uint8_t *back = NULL; size_t back_len = 0;
    if (mkz_autocol_decode(blob, blob_len, &back, &back_len)) { fprintf(stderr, "self-decode failed\n"); free(in); free(blob); return 1; }
    if (back_len != in_len || (in_len && memcmp(back, in, in_len) != 0)) {
        fprintf(stderr, "self-roundtrip MISMATCH (in %zu, back %zu)\n", in_len, back_len);
        free(in); free(blob); free(back); return 1;
    }
    free(back);

    FILE *o = fopen(argv[2], "wb");
    if (!o) { fprintf(stderr, "cannot write %s\n", argv[2]); free(in); free(blob); return 1; }
    size_t w = blob_len ? fwrite(blob, 1, blob_len, o) : 0;
    int e = (w != blob_len) | (fclose(o) != 0);
    fprintf(stderr, "encoded %s: %zu -> %zu bytes (self-roundtrip OK)\n", argv[1], in_len, blob_len);
    free(in); free(blob);
    return e ? 1 : 0;
}
