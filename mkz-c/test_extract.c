/* test_extract.c - atomicity: a corrupt trailer must leave dest untouched.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pas1.h"
#include "archive.h"
#include "stream.h"

/* Count dest entries that are not "." / ".." / ".mkz-partial.*" */
static int visible_entries(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (strncmp(e->d_name, ".mkz-partial.", 13) == 0) continue;
        n++;
    }
    closedir(d);
    return n;
}

/* Regression for move_tree's depth handling: a ~200-level single-char directory chain
 * (well under move_tree's MKZ_MOVE_TREE_MAX_DEPTH cap of 256, but deep enough that the
 * old stack-array-per-frame implementation would have burned ~200 * 16 KB =~ 3.2 MB of
 * stack) must extract cleanly via the heap-allocated-per-frame move_tree, with the
 * deep file's content intact. This does not, by itself, prove the stack-exhaustion bug
 * is fixed (3.2 MB comfortably fits an 8 MB default stack even pre-fix) - it proves the
 * heap-based recursion still works correctly at real depth. */
#define DEEP_LEVELS 200

static void test_deep_nesting(void) {
    const char *base = "/tmp/mkz_test_deep";
    char ws[128];
    snprintf(ws, sizeof ws, "%s.%ld", base, (long)getpid());

    char srctop[160];
    snprintf(srctop, sizeof srctop, "%s/src", ws);
    assert(mkz_mkdir_p(srctop) == 0);

    /* extend an absolute path by "/d" DEEP_LEVELS times, mkdir_p-ing as we go */
    char deep[1024];
    size_t n = (size_t)snprintf(deep, sizeof deep, "%s", srctop);
    assert(n < sizeof deep);
    for (int i = 0; i < DEEP_LEVELS; i++) {
        assert(n + 2 < sizeof deep);
        deep[n++] = '/'; deep[n++] = 'd'; deep[n] = '\0';
        assert(mkz_mkdir_p(deep) == 0);
    }

    char leaf[1040];
    assert((size_t)snprintf(leaf, sizeof leaf, "%s/leaf.log", deep) < sizeof leaf);
    FILE *lf = fopen(leaf, "wb");
    assert(lf && fputs("deep\n", lf) >= 0 && fclose(lf) == 0);

    char arc[160], dest[160];
    snprintf(arc, sizeof arc, "%s/a.mkz", ws);
    snprintf(dest, sizeof dest, "%s/out", ws);

    assert(mkz_create_stream((const char *[]){srctop}, 1, arc, 12, 0, 0) == 0);
    assert(mkz_extract_stream(arc, dest, 0) == 0);

    /* `leaf` is an absolute path (starts with '/'); dest + leaf is exactly the
     * extracted path mkz_safe_join builds (dest + "/" + rel, rel = abs path minus its
     * leading '/'). Confirm the deep file landed with the right content. */
    char extracted[1200];
    assert((size_t)snprintf(extracted, sizeof extracted, "%s%s", dest, leaf) < sizeof extracted);
    FILE *ef = fopen(extracted, "rb");
    assert(ef);
    char buf[16] = {0};
    size_t got = fread(buf, 1, sizeof buf - 1, ef);
    fclose(ef);
    assert(got == 5 && memcmp(buf, "deep\n", 5) == 0);

    fprintf(stderr, "ok: %d-level nested extract landed with correct content\n", DEEP_LEVELS);
}

/* Regression for the 0.1.3 scratch-reuse fix (mkz_pas1_decode_block_into / struct
 * mkz_pas1_scratch): the extractor now hands the caller a pointer BORROWED from a
 * reused per-extraction scratch buffer instead of a fresh malloc per block, and lazily
 * allocates the autocol half of that scratch only the first time a block actually needs
 * it. Neither of those was exercised by any other test here (they all use single-block
 * or few-block archives), so this builds a source that: (a) produces at least 8 blocks
 * at a small block size (passed directly to mkz_create_stream - the real mechanism it
 * uses internally, not the PSRC_AC_BLOCK_MB env var, which only the CLI in mkz.c reads),
 * and (b) alternates highly-columnar sections (autocol should win) with high-entropy
 * sections (autocol shouldn't help, raw should win), so both the zstd-only and the
 * autocol decode paths through the scratch get exercised, and the lazy autocol-scratch
 * allocation happens partway through the run rather than on the first block. Whether a
 * given archive actually ends up with both flag values present is verified directly by
 * walking its block headers below, not assumed from the input design. */
#define SCRATCH_TEST_BLOCK_BYTES (1u << 20)   /* 1 MiB: small enough for >= 8 blocks */
#define SCRATCH_TEST_SECTIONS    8            /* alternating columnar / high-entropy */
#define SCRATCH_TEST_LINES       60000        /* per section; ~2 MB/section either way */

/* Minimal PAS1 block-header walker (magic, then repeated [tag=1,flags,uvarint(orig_len),
 * uvarint(comp_len),payload] until tag=0): counts blocks by flags so the test can assert
 * the mix it needs actually occurred, instead of hoping the input design worked. */
static void count_block_flags(const char *archive, int *n_raw, int *n_autocol) {
    FILE *f = fopen(archive, "rb");
    assert(f);
    char magic[4];
    assert(fread(magic, 1, 4, f) == 4 && memcmp(magic, "PAS1", 4) == 0);
    *n_raw = 0; *n_autocol = 0;
    for (;;) {
        int tag = fgetc(f);
        assert(tag != EOF);
        if (tag == 0) break;
        assert(tag == 1);
        int flags = fgetc(f);
        assert(flags != EOF);
        if (flags & 1) (*n_autocol)++; else (*n_raw)++;
        uint64_t vals[2] = {0, 0};
        for (int vi = 0; vi < 2; vi++) {
            int sh = 0;
            for (;;) {
                int c = fgetc(f);
                assert(c != EOF);
                vals[vi] |= (uint64_t)(c & 0x7f) << sh;
                sh += 7;
                if (!(c & 0x80)) break;
            }
        }
        assert(fseek(f, (long)vals[1] /* comp_len */, SEEK_CUR) == 0);
    }
    fclose(f);
}

static int files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    int eq = 1;
    for (;;) {
        unsigned char ba[65536], bb[65536];
        size_t ra = fread(ba, 1, sizeof ba, fa);
        size_t rb = fread(bb, 1, sizeof bb, fb);
        if (ra != rb || (ra && memcmp(ba, bb, ra) != 0)) { eq = 0; break; }
        if (ra == 0) break;
    }
    fclose(fa); fclose(fb);
    return eq;
}

static void test_scratch_reuse_mixed_blocks(void) {
    const char *base = "/tmp/mkz_test_scratch";
    char ws[128];
    snprintf(ws, sizeof ws, "%s.%ld", base, (long)getpid());
    char srcdir[192], srcfile[224], arc[192], dest[192];
    snprintf(srcdir, sizeof srcdir, "%s/src", ws);
    snprintf(srcfile, sizeof srcfile, "%s/mixed.log", srcdir);
    snprintf(arc, sizeof arc, "%s/a.mkz", ws);
    snprintf(dest, sizeof dest, "%s/out", ws);
    assert(mkz_mkdir_p(srcdir) == 0);

    FILE *f = fopen(srcfile, "wb");
    assert(f);
    unsigned rngstate = 0xc0ffeeu;
    static const char alphabet[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (int sec = 0; sec < SCRATCH_TEST_SECTIONS; sec++) {
        if (sec % 2 == 0) {
            /* highly-columnar: same skeleton every line, numeric + repeated fields ->
             * autocol should win comfortably (matches the format used elsewhere in this
             * test suite / the Task 8 memory measurements). */
            for (int i = 0; i < SCRATCH_TEST_LINES; i++) {
                fprintf(f, "2026 host%d GET /p/%d 200 %d\n", i % 7, i, 100 + (i % 900));
            }
        } else {
            /* high-entropy, single-word-per-line: every value is unique, so neither
             * autocol's delta nor dict-ref codec helps, and the transform's raw-value
             * codec adds no real structure over the original bytes - the never-worse
             * gate should keep this raw. */
            for (int i = 0; i < SCRATCH_TEST_LINES; i++) {
                for (int k = 0; k < 40; k++) {
                    rngstate = rngstate * 1103515245u + 12345u;
                    fputc(alphabet[(rngstate >> 16) % (sizeof alphabet - 1)], f);
                }
                fputc('\n', f);
            }
        }
    }
    fclose(f);

    assert(mkz_create_stream((const char *[]){srcfile}, 1, arc, 12,
                             SCRATCH_TEST_BLOCK_BYTES, 0) == 0);

    int n_raw = 0, n_autocol = 0;
    count_block_flags(arc, &n_raw, &n_autocol);
    assert(n_raw + n_autocol >= 8);
    assert(n_raw > 0);      /* some blocks stayed raw */
    assert(n_autocol > 0);  /* some blocks won autocol */
    fprintf(stderr, "ok: mixed-content archive has %d block(s), %d raw / %d autocol\n",
            n_raw + n_autocol, n_raw, n_autocol);

    assert(mkz_extract_stream(arc, dest, 0) == 0);

    /* srcfile is an absolute path; dest + srcfile is exactly what mkz_safe_join builds
     * (dest + "/" + rel, rel = srcfile minus its leading '/'), same as test_deep_nesting
     * above. */
    char extracted[416];
    assert((size_t)snprintf(extracted, sizeof extracted, "%s%s", dest, srcfile)
           < sizeof extracted);
    assert(files_equal(srcfile, extracted));
    fprintf(stderr, "ok: %d-block mixed autocol/raw extract is byte-exact "
                    "(scratch reuse across blocks verified)\n", n_raw + n_autocol);
}

int main(void) {
    const char *base = "/tmp/mkz_test_atomic";
    char cmd[256];
    /* fresh workspace; moving any previous run aside is fine, deletion is not needed */
    snprintf(cmd, sizeof cmd, "%s.%ld", base, (long)getpid());
    const char *ws = cmd; /* /tmp/mkz_test_atomic.<pid> */
    char srcdir[256], dest[256], arc[256];
    snprintf(srcdir, sizeof srcdir, "%s/src", ws);
    snprintf(dest, sizeof dest, "%s/out", ws);
    snprintf(arc, sizeof arc, "%s/a.mkz", ws);
    assert(mkz_mkdir_p(srcdir) == 0);
    FILE *f = fopen(strcat(srcdir, "/a.log"), "wb"); /* srcdir now the file path */
    assert(f && fputs("alpha\nbeta\n", f) >= 0 && fclose(f) == 0);
    srcdir[strlen(srcdir) - 6] = '\0';               /* back to .../src */

    /* create an archive of srcdir via the streaming writer
     * (mkz_create_stream(paths, npaths, archive, level, block, verbose) - see stream.h) */
    assert(mkz_create_stream((const char *[]){srcdir}, 1, arc, 12, 0, 0) == 0);

    /* corrupt the SHA-256 trailer: flip the last byte in place */
    f = fopen(arc, "r+b");
    assert(f && fseek(f, -1, SEEK_END) == 0);
    int c = fgetc(f);
    assert(c != EOF && fseek(f, -1, SEEK_END) == 0);
    assert(fputc(c ^ 0xff, f) != EOF && fclose(f) == 0);

    assert(mkz_extract_stream(arc, dest, 0) == -1);
    assert(visible_entries(dest) == 0);   /* atomicity: nothing placed in dest */
    fprintf(stderr, "ok: corrupt trailer left dest untouched\n");

    test_deep_nesting();
    test_scratch_reuse_mixed_blocks();
    return 0;
}
