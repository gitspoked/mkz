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
    return 0;
}
