/* mkz.c — C mkz CLI. Creates and extracts .mkz archives (PAS1 container + autocol/zstd
 * blocks + tar-style entry stream), byte-compatible with the Rust mkz.
 *
 *   mkz -c[z[LEVEL]][v] -f <archive> <files-or-dirs...>   create
 *   mkz -x[v]           -f <archive> [destdir]            extract (default dest ".")
 *
 * Both directions stream line-oriented input in bounded memory for typical text/logs: peak
 * memory ≈ one block, matching the Rust mkz. Newline-free input is the exception — a block is
 * read up to the next newline, so a file with no newlines is buffered whole and not yet
 * bounded. Create runs the autocol pre-pass per block behind a never-worse gate; extract
 * verifies the SHA-256 trailer before declaring success — but streams entries to disk first,
 * so a corrupt trailer can leave already-written output (extraction is not yet atomic).
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MKZ_VERSION "0.1.0"

static const char *HELP =
    "mkz (C) — create/extract psrc-autocol/zstd (PAS1) archives\n"
    "usage:\n"
    "  mkz -c[z[LEVEL]][v] -f <archive> <files-or-dirs...>   create\n"
    "  mkz -x[v]           -f <archive> [destdir]            extract (default dest: \".\")\n"
    "flags:  -c create  -x extract  -z[LEVEL] zstd 1-22 (default 12)  -f archive  -v verbose\n"
    "env:    PSRC_AC_BLOCK_MB (default 16)   PSRC_AC_ZSTD_LEVEL (default 12)\n";

static int env_int(const char *name, int dflt, int lo, int hi) {
    const char *s = getenv(name);
    if (!s || !*s) return dflt;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || v < lo || v > hi) return dflt;
    return (int)v;
}

int main(int argc, char **argv) {
    int extract = 0, verbose = 0, create = 0;
    int level = env_int("PSRC_AC_ZSTD_LEVEL", 12, 1, 22);
    size_t block = (size_t)env_int("PSRC_AC_BLOCK_MB", 16, 1, 1 << 15) << 20;
    const char *archive = NULL;

    /* positionals: input paths (create) or destdir (extract) */
    const char **pos = (const char **)calloc((size_t)(argc > 0 ? argc : 1), sizeof *pos);
    if (!pos) { fprintf(stderr, "mkz: out of memory\n"); return 1; }
    size_t npos = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { printf("%s", HELP); free(pos); return 0; }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("mkz %s (psrc, C port)\n", MKZ_VERSION); free(pos); return 0; }
        if (a[0] == '-' && a[1] != '\0' && a[1] != '-') {
            for (const char *c = a + 1; *c; c++) {
                switch (*c) {
                    case 'x': extract = 1; break;
                    case 'v': verbose = 1; break;
                    case 'c': create = 1; break;
                    case 'z': {
                        int n = 0, have = 0;
                        while (c[1] >= '0' && c[1] <= '9') { n = n * 10 + (c[1] - '0'); c++; have = 1; }
                        if (have) {
                            if (n < 1 || n > 22) { fprintf(stderr, "mkz: zstd level must be 1-22\n"); free(pos); return 2; }
                            level = n;
                        }
                        break;
                    }
                    case 'f':
                        if (i + 1 >= argc) { fprintf(stderr, "mkz: -f needs an archive\n"); free(pos); return 2; }
                        archive = argv[++i];
                        break;
                    default: fprintf(stderr, "mkz: unknown flag -%c\n\n%s", *c, HELP); free(pos); return 2;
                }
            }
        } else {
            pos[npos++] = a;
        }
    }

    if (create == extract) { fprintf(stderr, "mkz: specify exactly one of -c or -x\n\n%s", HELP); free(pos); return 2; }
    if (!archive) { fprintf(stderr, "mkz: missing -f <archive>\n\n%s", HELP); free(pos); return 2; }

    int rc;
    if (create) {
        if (npos == 0) { fprintf(stderr, "mkz: create needs at least one file or directory\n"); free(pos); return 2; }
        rc = mkz_create_stream(pos, npos, archive, level, block, verbose);
        if (rc) { fprintf(stderr, "mkz: create failed\n"); }
        else {
            struct stat st;
            if (stat(archive, &st) == 0)
                fprintf(stderr, "mkz: wrote %s (%lld bytes, zstd %d)\n", archive, (long long)st.st_size, level);
            else
                fprintf(stderr, "mkz: wrote %s (zstd %d)\n", archive, level);
        }
    } else {
        if (npos > 1) { fprintf(stderr, "mkz: extract takes at most one destdir\n"); free(pos); return 2; }
        const char *dest = npos ? pos[0] : ".";
        rc = mkz_extract_stream(archive, dest, verbose);
        if (rc) fprintf(stderr, "mkz: %s could not be extracted (not a valid PAS1 archive, or failed integrity check)\n", archive);
        else    fprintf(stderr, "mkz: extracted %s -> %s (SHA-256 verified)\n", archive, dest);
    }
    free(pos);
    return rc ? 1 : 0;
}
