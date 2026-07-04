/* archive.c — tar-style entry-stream unpack for mkz (C port).
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "archive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#define MKZ_PATH_MAX 4096

static int rd_uv(const uint8_t *in, size_t len, size_t *pos, uint64_t *out) {
    uint64_t v = 0; int s = 0;
    for (;;) {
        if (*pos >= len) return -1;
        uint8_t b = in[(*pos)++];
        v |= (uint64_t)(b & 0x7f) << s;
        if (!(b & 0x80)) { *out = v; return 0; }
        s += 7;
        if (s >= 64) return -1;
    }
}

/* mkdir -p (creates all intermediate components of `path`). */
int mkz_mkdir_p(const char *path) {
    char tmp[MKZ_PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Validate stored rel path (no absolute, "..", ".", empty, or NUL) and build dest/rel. */
int mkz_safe_join(const char *dest, const uint8_t *rel, size_t rel_len,
                  char *out, size_t out_sz) {
    if (rel_len == 0 || rel_len >= 2048 || rel[0] == '/') return -1;
    for (size_t i = 0; i < rel_len; i++) {
        if (rel[i] == 0) return -1; /* embedded NUL */
    }
    char r[2048];
    memcpy(r, rel, rel_len);
    r[rel_len] = '\0';

    /* validate each '/'-separated component on a throwaway copy */
    char chk[2048];
    memcpy(chk, r, rel_len + 1);
    char *save = NULL;
    char *tok = strtok_r(chk, "/", &save);
    if (!tok) return -1;
    while (tok) {
        if (tok[0] == '\0' || strcmp(tok, ".") == 0 || strcmp(tok, "..") == 0) return -1;
        tok = strtok_r(NULL, "/", &save);
    }

    int m = snprintf(out, out_sz, "%s/%s", dest, r);
    if (m < 0 || (size_t)m >= out_sz) return -1;
    return 0;
}

int mkz_archive_extract(const uint8_t *es, size_t es_len, const char *dest, int verbose) {
    if (mkz_mkdir_p(dest)) return -1;
    size_t pos = 0;
    char path[MKZ_PATH_MAX];

    while (pos < es_len) {
        uint8_t tag = es[pos++];
        uint64_t pl;
        if (rd_uv(es, es_len, &pos, &pl)) return -1;
        if (pl > es_len - pos) return -1;
        const uint8_t *rel = es + pos;
        size_t rel_len = (size_t)pl;
        pos += rel_len;
        if (mkz_safe_join(dest, rel, rel_len, path, sizeof path)) return -1;

        if (tag == 1) { /* directory */
            if (mkz_mkdir_p(path)) return -1;
            if (verbose) fprintf(stderr, "d %.*s\n", (int)rel_len, (const char *)rel);
        } else if (tag == 0) { /* file */
            uint64_t sz;
            if (rd_uv(es, es_len, &pos, &sz)) return -1;
            if (sz > es_len - pos) return -1;
            const uint8_t *content = es + pos;
            pos += (size_t)sz;

            /* ensure parent dirs exist */
            char parent[MKZ_PATH_MAX];
            size_t pn = strlen(path);
            memcpy(parent, path, pn + 1);
            char *slash = strrchr(parent, '/');
            if (slash) {
                *slash = '\0';
                if (parent[0] && mkz_mkdir_p(parent)) return -1;
            }
            FILE *f = fopen(path, "wb");
            if (!f) return -1;
            if (sz && fwrite(content, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return -1; }
            fclose(f);
            if (verbose) fprintf(stderr, "x %.*s\n", (int)rel_len, (const char *)rel);
        } else {
            return -1; /* unknown tag */
        }
    }
    return 0;
}

/* ───────────────────────── create side (tree -> entry stream) ─────────────────────────
 * Mirrors the Rust ArchiveReader: a flat byte stream of entries, each directory emitted
 * before its (name-sorted) children, paths normalized to relocatable rel paths (leading
 * root / "." / ".." components dropped). Builds the whole stream in memory — consistent
 * with the in-memory reader (v1); streaming create is future work.
 */

struct ebuf { uint8_t *d; size_t len, cap; };
static int eb_reserve(struct ebuf *b, size_t extra) {
    if (extra <= b->cap - b->len) return 0;
    size_t need = b->len + extra;
    if (need < b->len) return -1; /* size_t overflow */
    size_t ncap = b->cap ? b->cap : 4096;
    while (ncap < need) { size_t d2 = ncap * 2; if (d2 < ncap) return -1; ncap = d2; }
    uint8_t *nd = (uint8_t *)realloc(b->d, ncap);
    if (!nd) return -1;
    b->d = nd; b->cap = ncap;
    return 0;
}
static int eb_push(struct ebuf *b, const void *p, size_t n) {
    if (eb_reserve(b, n)) return -1;
    if (n) memcpy(b->d + b->len, p, n);
    b->len += n;
    return 0;
}
static int eb_uvarint(struct ebuf *b, uint64_t n) {
    uint8_t tmp[10]; int i = 0;
    for (;;) {
        uint8_t byte = (uint8_t)(n & 0x7f); n >>= 7;
        if (n) tmp[i++] = byte | 0x80; else { tmp[i++] = byte; break; }
    }
    return eb_push(b, tmp, (size_t)i);
}

/* Normalized rel path: keep only "Normal" components (drop "", ".", "..", leading root),
 * joined by '/'. Matches Rust safe_rel. -1 if it would overflow `out`. */
static int rel_normalize(const char *path, char *out, size_t out_sz) {
    size_t o = 0;
    const char *p = path;
    int first = 1;
    while (*p) {
        while (*p == '/') p++;                 /* skip separators */
        const char *c0 = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - c0);
        if (clen == 0) break;
        if (clen == 1 && c0[0] == '.') continue;
        if (clen == 2 && c0[0] == '.' && c0[1] == '.') continue;
        if (!first) { if (o + 1 >= out_sz) return -1; out[o++] = '/'; }
        if (o + clen >= out_sz) return -1;
        memcpy(out + o, c0, clen); o += clen;
        first = 0;
    }
    out[o] = '\0';
    return 0;
}

/* Read a whole file into a malloc'd buffer (caller frees). 0 / -1. */
static int slurp(const char *path, uint8_t **buf, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    struct ebuf b = {0};
    uint8_t chunk[1 << 16];
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (eb_push(&b, chunk, r)) { fclose(f); free(b.d); return -1; }
    }
    int err = ferror(f);
    fclose(f);
    if (err) { free(b.d); return -1; }
    *buf = b.d; *len = b.len;
    return 0;
}

static char *join_path(const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    char *r = (char *)malloc(dl + 1 + nl + 1);
    if (!r) return NULL;
    memcpy(r, dir, dl); r[dl] = '/'; memcpy(r + dl + 1, name, nl); r[dl + 1 + nl] = '\0';
    return r;
}

/* Unsigned lexicographic compare (matches Rust's OsStr byte ordering). */
static int cmp_cstr(const void *a, const void *b) {
    const unsigned char *x = *(const unsigned char *const *)a;
    const unsigned char *y = *(const unsigned char *const *)b;
    while (*x && *x == *y) { x++; y++; }
    return (int)*x - (int)*y;
}

static int emit_dir(struct ebuf *es, const char *rel, int verbose) {
    uint8_t tag = 1;
    if (eb_push(es, &tag, 1) || eb_uvarint(es, strlen(rel)) || eb_push(es, rel, strlen(rel)))
        return -1;
    if (verbose) fprintf(stderr, "d %s\n", rel);
    return 0;
}

static int emit_file(struct ebuf *es, const char *abspath, const char *rel, int verbose) {
    uint8_t *content = NULL; size_t clen = 0;
    if (slurp(abspath, &content, &clen)) return -1;
    uint8_t tag = 0;
    int e = eb_push(es, &tag, 1)
         || eb_uvarint(es, strlen(rel)) || eb_push(es, rel, strlen(rel))
         || eb_uvarint(es, clen)        || eb_push(es, content, clen);
    free(content);
    if (e) return -1;
    if (verbose) fprintf(stderr, "a %s\n", rel);
    return 0;
}

#define MKZ_REL_MAX 8192

/* ── entry collection (shared by the in-memory build and the streaming source) ──
 * Walks `paths` into a flat list in the canonical order (each dir before its name-sorted
 * children), rel paths normalized; files carry their abs path + stat size, dirs abs==NULL. */

struct elist { struct mkz_entry *e; size_t n, cap; };
static int el_push(struct elist *L, int is_dir, const char *rel, const char *abs, uint64_t size) {
    if (L->n == L->cap) {
        size_t nc = L->cap ? L->cap * 2 : 32;
        struct mkz_entry *ne = (struct mkz_entry *)realloc(L->e, nc * sizeof *ne);
        if (!ne) return -1;
        L->e = ne; L->cap = nc;
    }
    struct mkz_entry *en = &L->e[L->n];
    en->is_dir = is_dir; en->size = size;
    en->rel = strdup(rel);
    en->abs = abs ? strdup(abs) : NULL;
    if (!en->rel || (abs && !en->abs)) { free(en->rel); free(en->abs); return -1; }
    L->n++;
    return 0;
}

void mkz_free_entries(struct mkz_entry *e, size_t n) {
    if (!e) return;
    for (size_t i = 0; i < n; i++) { free(e[i].rel); free(e[i].abs); }
    free(e);
}

static int walk_collect(const char *abspath, struct elist *L) {
    char rel[MKZ_REL_MAX];
    if (rel_normalize(abspath, rel, sizeof rel)) return -1;
    if (rel[0] != '\0' && el_push(L, 1, rel, abspath, 0)) return -1;   /* skip the nameless walk-root ('.') */

    DIR *d = opendir(abspath);
    if (!d) return -1;
    char **names = NULL; size_t nn = 0, cap = 0;
    int rc = -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (nn == cap) {
            size_t nc = cap ? cap * 2 : 16;
            char **nm = (char **)realloc(names, nc * sizeof *nm);
            if (!nm) goto wclean;
            names = nm; cap = nc;
        }
        names[nn] = strdup(de->d_name);
        if (!names[nn]) goto wclean;
        nn++;
    }
    closedir(d); d = NULL;
    qsort(names, nn, sizeof *names, cmp_cstr);

    for (size_t i = 0; i < nn; i++) {
        char *child = join_path(abspath, names[i]);
        if (!child) goto wclean;
        struct stat st;
        if (lstat(child, &st) != 0) { free(child); goto wclean; }
        if (S_ISDIR(st.st_mode)) {
            if (walk_collect(child, L)) { free(child); goto wclean; }
        } else if (S_ISREG(st.st_mode)) {
            char crel[MKZ_REL_MAX];
            if (rel_normalize(child, crel, sizeof crel)
                || el_push(L, 0, crel, child, (uint64_t)st.st_size)) { free(child); goto wclean; }
        }
        /* symlinks / special files: silently skipped (as the Rust walk does) */
        free(child);
    }
    rc = 0;
wclean:
    if (d) closedir(d);
    for (size_t i = 0; i < nn; i++) free(names[i]);
    free(names);
    return rc;
}

int mkz_collect_entries(const char *const *paths, size_t npaths, struct mkz_entry **out, size_t *n) {
    if (npaths == 0) return -1;
    struct elist L = {0};
    for (size_t i = 0; i < npaths; i++) {
        struct stat st;
        if (lstat(paths[i], &st) != 0) goto fail;
        if (S_ISDIR(st.st_mode)) {
            if (walk_collect(paths[i], &L)) goto fail;
        } else if (S_ISREG(st.st_mode)) {
            char rel[MKZ_REL_MAX];
            if (rel_normalize(paths[i], rel, sizeof rel)
                || (rel[0] != '\0' && el_push(&L, 0, rel, paths[i], (uint64_t)st.st_size))) goto fail;
        } else {
            goto fail; /* top-level entry must be a regular file or directory */
        }
    }
    *out = L.e; *n = L.n;
    return 0;
fail:
    mkz_free_entries(L.e, L.n);
    return -1;
}

int mkz_archive_build(const char *const *paths, size_t npaths, int verbose,
                      uint8_t **es_out, size_t *es_len) {
    struct mkz_entry *ents = NULL; size_t nent = 0;
    if (mkz_collect_entries(paths, npaths, &ents, &nent)) return -1;
    struct ebuf es = {0};
    for (size_t i = 0; i < nent; i++) {
        if (ents[i].is_dir) { if (emit_dir(&es, ents[i].rel, verbose)) goto fail; }
        else                { if (emit_file(&es, ents[i].abs, ents[i].rel, verbose)) goto fail; }
    }
    mkz_free_entries(ents, nent);
    *es_out = es.d; *es_len = es.len;
    return 0;
fail:
    mkz_free_entries(ents, nent);
    free(es.d);
    return -1;
}
