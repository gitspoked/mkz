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
    return 0;
}
