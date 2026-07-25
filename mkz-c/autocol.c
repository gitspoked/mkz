/* autocol.c - psrc-autocol decode (C port). Format-identical to Rust psrc_autocol.
 * Untrusted input: every length/offset/index is bounds-checked; structures are calloc'd
 * so the single cleanup path frees safely from any failure point.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "autocol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct rdr { const uint8_t *p; size_t len, pos; };

static int rd_uvarint(struct rdr *r, uint64_t *out) {
    uint64_t v = 0; int shift = 0;
    for (;;) {
        if (r->pos >= r->len) return -1;
        uint8_t b = r->p[r->pos++];
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = v; return 0; }
        shift += 7;
        if (shift >= 64) return -1;
    }
}
/* borrowed slice: uvarint length + that many bytes (bounds-checked) */
static int rd_bytes(struct rdr *r, const uint8_t **p, size_t *n) {
    uint64_t L;
    if (rd_uvarint(r, &L)) return -1;
    if (L > r->len - r->pos) return -1;
    *p = r->p + r->pos; *n = (size_t)L; r->pos += (size_t)L;
    return 0;
}
static int64_t unzig(uint64_t z) { return (int64_t)(z >> 1) ^ -(int64_t)(z & 1); }

struct str { const uint8_t *p; size_t n; };          /* borrowed into blob */
struct slot { int is_const; struct str w; };
struct tmpl { struct str *seps; size_t nseps; struct slot *slots; size_t nword, nvar; };
struct val { uint8_t *p; size_t n; };                /* owned */
struct col { struct val *v; size_t n; };

struct ob { uint8_t *d; size_t len, cap; };
static int ob_push(struct ob *b, const uint8_t *p, size_t n) {
    if (n > b->cap - b->len) {
        size_t need = b->len + n; if (need < b->len) return -1;
        size_t c = b->cap ? b->cap : 256;
        while (c < need) { size_t d2 = c * 2; if (d2 < c) return -1; c = d2; }
        uint8_t *nd = (uint8_t *)realloc(b->d, c); if (!nd) return -1;
        b->d = nd; b->cap = c;
    }
    if (n) memcpy(b->d + b->len, p, n);
    b->len += n;
    return 0;
}

/* Reusable decode scratch (opaque to callers via autocol.h): just an `ob` whose buffer
 * outlives a single mkz_autocol_decode_into call, so repeated calls reuse its capacity. */
struct mkz_ac_scratch { struct ob ob; };

/* Shared decode body for both mkz_autocol_decode and mkz_autocol_decode_into: `ob` is the
 * output buffer, owned and grown by the CALLER (not this function). Reusing the same `ob`
 * across many calls (resetting only ob->len, keeping ob->d/ob->cap) avoids rebuilding a
 * large buffer from empty via malloc/realloc/free every call: that pattern was measured to
 * leave roughly one buffer's worth of resident memory behind PER CALL on this platform's
 * allocator (never reclaimed until process exit), so peak RSS grew without bound across a
 * multi-block extraction instead of staying flat. On success, *out points INTO ob->d
 * (borrowed - the caller does not own or free it directly; see the two wrappers below for
 * how ownership is actually handled). On failure, ob->d/ob->cap are left as-is (whatever
 * partial content is there is harmless garbage that the next call's ob->len = 0 discards). */
static int mkz_autocol_decode_impl(const uint8_t *blob, size_t blob_len, struct ob *ob,
                                   uint8_t **out, size_t *out_len) {
    struct rdr r = { blob, blob_len, 0 };
    struct tmpl *tmpls = NULL; size_t ntmpl = 0;
    size_t *gid = NULL; size_t nlines = 0;
    struct str *dict = NULL; size_t ndict = 0;
    struct col *cols = NULL; size_t ncol = 0;
    size_t *base = NULL, *cursor = NULL;
    int ret = -1;
    uint64_t u;

    ob->len = 0; /* reuse ob->d/ob->cap across calls; forget only the previous contents */

    /* version byte */
    if (r.pos >= r.len || blob[r.pos] != 1) goto done;
    r.pos++;

    /* templates */
    if (rd_uvarint(&r, &u)) goto done;
    ntmpl = (size_t)u;
    if (ntmpl > blob_len) goto done;
    tmpls = (struct tmpl *)calloc(ntmpl ? ntmpl : 1, sizeof *tmpls);
    if (!tmpls) goto done;
    for (size_t ti = 0; ti < ntmpl; ti++) {
        const uint8_t *tp; size_t tn;
        if (rd_bytes(&r, &tp, &tn)) goto done;
        struct rdr tr = { tp, tn, 0 };
        uint64_t nw;
        if (rd_uvarint(&tr, &nw)) goto done;
        size_t nword = (size_t)nw;
        if (nword > tn) goto done;
        struct tmpl *T = &tmpls[ti];
        T->nword = nword; T->nseps = nword + 1;
        T->seps = (struct str *)calloc(T->nseps, sizeof *T->seps);
        T->slots = (struct slot *)calloc(nword ? nword : 1, sizeof *T->slots);
        if (!T->seps || !T->slots) goto done;
        if (rd_bytes(&tr, &T->seps[0].p, &T->seps[0].n)) goto done;
        for (size_t j = 0; j < nword; j++) {
            if (tr.pos >= tr.len) goto done;
            uint8_t flag = tr.p[tr.pos++];
            if (flag == 1) {
                T->slots[j].is_const = 1;
                if (rd_bytes(&tr, &T->slots[j].w.p, &T->slots[j].w.n)) goto done;
            } else {
                T->nvar++;
            }
            if (rd_bytes(&tr, &T->seps[j + 1].p, &T->seps[j + 1].n)) goto done;
        }
    }

    /* record -> template ids */
    if (rd_uvarint(&r, &u)) goto done;
    nlines = (size_t)u;
    if (nlines > blob_len) goto done;
    gid = (size_t *)calloc(nlines ? nlines : 1, sizeof *gid);
    if (!gid) goto done;
    for (size_t i = 0; i < nlines; i++) {
        if (rd_uvarint(&r, &u) || u >= ntmpl) goto done;
        gid[i] = (size_t)u;
    }

    /* global value dictionary (borrowed slices) */
    if (rd_uvarint(&r, &u)) goto done;
    ndict = (size_t)u;
    if (ndict > blob_len) goto done;
    dict = (struct str *)calloc(ndict ? ndict : 1, sizeof *dict);
    if (!dict) goto done;
    for (size_t i = 0; i < ndict; i++) {
        if (rd_bytes(&r, &dict[i].p, &dict[i].n)) goto done;
    }

    /* columns (owned values) */
    if (rd_uvarint(&r, &u)) goto done;
    ncol = (size_t)u;
    if (ncol > blob_len) goto done;
    cols = (struct col *)calloc(ncol ? ncol : 1, sizeof *cols);
    if (!cols) goto done;
    for (size_t i = 0; i < ncol; i++) {
        if (r.pos >= r.len) goto done;
        uint8_t codec = r.p[r.pos++];
        uint64_t cnt;
        if (rd_uvarint(&r, &cnt)) goto done;
        if (cnt > blob_len) goto done;
        struct col *C = &cols[i];
        C->n = (size_t)cnt;
        C->v = (struct val *)calloc(cnt ? (size_t)cnt : 1, sizeof *C->v);
        if (!C->v) goto done;
        if (codec == 1) { /* delta (zigzag running sum -> decimal) */
            int64_t prev = 0;
            for (size_t k = 0; k < (size_t)cnt; k++) {
                uint64_t z;
                if (rd_uvarint(&r, &z)) goto done;
                prev += unzig(z);
                char tmp[24];
                int m = snprintf(tmp, sizeof tmp, "%lld", (long long)prev);
                if (m < 0) goto done;
                C->v[k].p = (uint8_t *)malloc((size_t)m ? (size_t)m : 1);
                if (!C->v[k].p) goto done;
                memcpy(C->v[k].p, tmp, (size_t)m); C->v[k].n = (size_t)m;
            }
        } else if (codec == 2) { /* dict-ref */
            for (size_t k = 0; k < (size_t)cnt; k++) {
                uint64_t id;
                if (rd_uvarint(&r, &id) || id >= ndict) goto done;
                size_t dn = dict[id].n;
                C->v[k].p = (uint8_t *)malloc(dn ? dn : 1);
                if (!C->v[k].p) goto done;
                if (dn) memcpy(C->v[k].p, dict[id].p, dn);
                C->v[k].n = dn;
            }
        } else { /* raw (codec 0, or anything else -> Rust's else branch) */
            for (size_t k = 0; k < (size_t)cnt; k++) {
                const uint8_t *vp; size_t vn;
                if (rd_bytes(&r, &vp, &vn)) goto done;
                C->v[k].p = (uint8_t *)malloc(vn ? vn : 1);
                if (!C->v[k].p) goto done;
                if (vn) memcpy(C->v[k].p, vp, vn);
                C->v[k].n = vn;
            }
        }
    }

    /* columns assigned gid-major, slot order: base[g] = prefix sum of nvar; total == ncol */
    base = (size_t *)calloc(ntmpl + 1, sizeof *base);
    if (!base) goto done;
    for (size_t g = 0; g < ntmpl; g++) base[g + 1] = base[g] + tmpls[g].nvar;
    if (base[ntmpl] != ncol) goto done;

    /* reconstruct each line, join with '\n' */
    cursor = (size_t *)calloc(ncol ? ncol : 1, sizeof *cursor);
    if (!cursor) goto done;
    for (size_t i = 0; i < nlines; i++) {
        if (i) { uint8_t nl = '\n'; if (ob_push(ob, &nl, 1)) goto done; }
        size_t g = gid[i];
        struct tmpl *T = &tmpls[g];
        size_t var_k = 0;
        if (ob_push(ob, T->seps[0].p, T->seps[0].n)) goto done;
        for (size_t j = 0; j < T->nword; j++) {
            if (T->slots[j].is_const) {
                if (ob_push(ob, T->slots[j].w.p, T->slots[j].w.n)) goto done;
            } else {
                size_t col_idx = base[g] + var_k;
                var_k++;
                if (col_idx >= ncol) goto done;
                struct col *C = &cols[col_idx];
                if (cursor[col_idx] >= C->n) goto done;
                struct val *vv = &C->v[cursor[col_idx]++];
                if (ob_push(ob, vv->p, vv->n)) goto done;
            }
            if (ob_push(ob, T->seps[j + 1].p, T->seps[j + 1].n)) goto done;
        }
    }

    *out = ob->d; *out_len = ob->len;
    ret = 0;

done:
    if (cols) {
        for (size_t i = 0; i < ncol; i++) {
            if (cols[i].v) {
                for (size_t k = 0; k < cols[i].n; k++) free(cols[i].v[k].p);
                free(cols[i].v);
            }
        }
        free(cols);
    }
    if (tmpls) {
        for (size_t i = 0; i < ntmpl; i++) { free(tmpls[i].seps); free(tmpls[i].slots); }
        free(tmpls);
    }
    free(dict);
    free(gid);
    free(base);
    free(cursor);
    return ret;
}

/* One-shot decode: fresh buffer every call, caller frees *out. Unchanged contract/behavior
 * for existing callers (tests, `mkz untransform`, the in-memory mkz_pas1_decompress path). */
int mkz_autocol_decode(const uint8_t *blob, size_t blob_len, uint8_t **out, size_t *out_len) {
    struct ob ob = {0};
    int rc = mkz_autocol_decode_impl(blob, blob_len, &ob, out, out_len);
    if (rc) { free(ob.d); return rc; }
    return 0; /* success: ownership of ob.d (== *out) transfers to the caller */
}

/* Reusable-scratch decode for hot loops that call this once per block (the streaming
 * extractor): `scratch` owns a buffer that persists and grows across calls instead of being
 * rebuilt from empty every time. *out is BORROWED from scratch - valid until the next call
 * on the same scratch, or until mkz_autocol_scratch_free; the caller must not free(*out). */
int mkz_autocol_decode_into(const uint8_t *blob, size_t blob_len, struct mkz_ac_scratch *scratch,
                            uint8_t **out, size_t *out_len) {
    return mkz_autocol_decode_impl(blob, blob_len, &scratch->ob, out, out_len);
}

struct mkz_ac_scratch *mkz_autocol_scratch_new(void) {
    return (struct mkz_ac_scratch *)calloc(1, sizeof(struct mkz_ac_scratch));
}

void mkz_autocol_scratch_free(struct mkz_ac_scratch *s) {
    if (!s) return;
    free(s->ob.d);
    free(s);
}

/* ============================ ENCODE ============================
 * Faithful port of Rust psrc_autocol::encode. Output is byte-identical to `mkz transform`.
 * Input is our own (trusted) bytes; the security-critical surface is decode, above.
 */

static int ob_byte(struct ob *b, uint8_t x) { return ob_push(b, &x, 1); }
static int ob_uvarint(struct ob *b, uint64_t n) {
    uint8_t t[10]; int i = 0;
    for (;;) { uint8_t by = (uint8_t)(n & 0x7f); n >>= 7; if (n) t[i++] = by | 0x80; else { t[i++] = by; break; } }
    return ob_push(b, t, (size_t)i);
}

static int is_word_byte(uint8_t b) {
    return (b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z');
}
static size_t uvlen(uint64_t n) { size_t c = 1; while (n >= 0x80) { n >>= 7; c++; } return c; }
static uint64_t zig(int64_t n) { return ((uint64_t)n << 1) ^ (uint64_t)(n >> 63); }

/* lexicographic unsigned byte compare, shorter-is-less (matches Rust <[u8]>::cmp) */
static int bytes_cmp(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    size_t m = an < bn ? an : bn;
    int c = m ? memcmp(a, b, m) : 0;
    if (c) return c;
    if (an == bn) return 0;
    return an < bn ? -1 : 1;
}

/* -- byte-string hash map (chaining). Keys are BORROWED, never freed by the map. -- */
struct hnode {
    const uint8_t *key; size_t klen;
    uint64_t u0;    /* gmap: gid ;  vmap: frequency */
    uint64_t u1;    /* vmap: tent_id (frequency rank) */
    uint64_t u2;    /* vmap: final_id (index in final dict) */
    int flag;       /* vmap: value referenced by a dict-ref column */
    struct hnode *next;
};
struct hmap {
    struct hnode **buckets; size_t nbuckets, count;
    struct hnode **order; size_t norder, ocap;   /* insertion order, for enumeration */
};
static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
static int hm_init(struct hmap *m, size_t hint) {
    size_t nb = 1024;
    while (nb < hint) { size_t d = nb * 2; if (d < nb) break; nb = d; }
    m->buckets = (struct hnode **)calloc(nb, sizeof *m->buckets);
    if (!m->buckets) return -1;
    m->nbuckets = nb; m->count = 0;
    m->order = NULL; m->norder = 0; m->ocap = 0;
    return 0;
}
static void hm_free(struct hmap *m) {
    for (size_t i = 0; i < m->norder; i++) free(m->order[i]); /* nodes only, NOT keys */
    free(m->order);
    free(m->buckets);
    m->buckets = NULL; m->order = NULL;
}
static struct hnode *hm_get(struct hmap *m, const uint8_t *k, size_t kl) {
    size_t b = fnv1a(k, kl) & (m->nbuckets - 1);
    for (struct hnode *n = m->buckets[b]; n; n = n->next)
        if (n->klen == kl && (kl == 0 || memcmp(n->key, k, kl) == 0)) return n;
    return NULL;
}
static int hm_resize(struct hmap *m) {
    size_t nb = m->nbuckets * 2;
    if (nb < m->nbuckets) return -1;
    struct hnode **nbk = (struct hnode **)calloc(nb, sizeof *nbk);
    if (!nbk) return -1;
    for (size_t i = 0; i < m->nbuckets; i++) {
        struct hnode *n = m->buckets[i];
        while (n) {
            struct hnode *next = n->next;
            size_t b = fnv1a(n->key, n->klen) & (nb - 1);
            n->next = nbk[b]; nbk[b] = n;
            n = next;
        }
    }
    free(m->buckets);
    m->buckets = nbk; m->nbuckets = nb;
    return 0;
}
/* Insert key (borrowed) if absent. Returns the node; *created set to 1 if new. NULL on OOM. */
static struct hnode *hm_put(struct hmap *m, const uint8_t *k, size_t kl, int *created) {
    struct hnode *e = hm_get(m, k, kl);
    if (e) { *created = 0; return e; }
    if (m->count + 1 > m->nbuckets * 2 && hm_resize(m)) return NULL;
    struct hnode *n = (struct hnode *)calloc(1, sizeof *n);
    if (!n) return NULL;
    n->key = k; n->klen = kl;
    size_t b = fnv1a(k, kl) & (m->nbuckets - 1);
    n->next = m->buckets[b]; m->buckets[b] = n;
    if (m->norder == m->ocap) {
        size_t nc = m->ocap ? m->ocap * 2 : 256;
        struct hnode **no = (struct hnode **)realloc(m->order, nc * sizeof *no);
        if (!no) { free(n); return NULL; }
        m->order = no; m->ocap = nc;
    }
    m->order[m->norder++] = n;
    m->count++;
    *created = 1;
    return n;
}

/* per-line tokenization: seps/words borrowed into `data`; seps.n == words.n + 1 */
struct toks { struct str *seps; size_t nsep; struct str *words; size_t nword; };

static int push_span(struct str **a, size_t *n, size_t *cap, const uint8_t *p, size_t len) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        struct str *na = (struct str *)realloc(*a, nc * sizeof **a);
        if (!na) return -1;
        *a = na; *cap = nc;
    }
    (*a)[*n].p = p; (*a)[*n].n = len; (*n)++;
    return 0;
}
static int tokenize(const uint8_t *line, size_t n, struct toks *t) {
    struct str *seps = NULL, *words = NULL;
    size_t ns = 0, sc = 0, nw = 0, wc = 0, i = 0;
    for (;;) {
        size_t s0 = i;
        while (i < n && !is_word_byte(line[i])) i++;
        if (push_span(&seps, &ns, &sc, line + s0, i - s0)) goto oom;
        if (i >= n) break;
        size_t w0 = i;
        while (i < n && is_word_byte(line[i])) i++;
        if (push_span(&words, &nw, &wc, line + w0, i - w0)) goto oom;
    }
    t->seps = seps; t->nsep = ns; t->words = words; t->nword = nw;
    return 0;
oom:
    free(seps); free(words);
    return -1;
}

/* try to parse a column as canonical non-negative i64 decimals (no leading zeros, <=18 digits).
 * Returns 1 + sets *out (owned) on success; 0 (no parse / OOM) with *out = NULL otherwise. */
static int try_ints(const struct str *col, size_t n, int64_t **out) {
    *out = NULL;
    int64_t *vals = (int64_t *)malloc((n ? n : 1) * sizeof *vals);
    if (!vals) return 0;
    for (size_t k = 0; k < n; k++) {
        const uint8_t *v = col[k].p; size_t vl = col[k].n;
        if (vl == 0 || vl > 18) { free(vals); return 0; }
        if (v[0] == '0' && vl > 1) { free(vals); return 0; }
        int64_t nn = 0;
        for (size_t j = 0; j < vl; j++) {
            if (v[j] < '0' || v[j] > '9') { free(vals); return 0; }
            nn = nn * 10 + (int64_t)(v[j] - '0');
        }
        vals[k] = nn;
    }
    *out = vals;
    return 1;
}

static int cmp_rank(const void *a, const void *b) {
    const struct hnode *x = *(const struct hnode *const *)a;
    const struct hnode *y = *(const struct hnode *const *)b;
    if (x->u0 != y->u0) return x->u0 > y->u0 ? -1 : 1;   /* higher frequency first */
    return bytes_cmp(x->key, x->klen, y->key, y->klen);  /* then value ascending */
}

struct grp { size_t *idx; size_t n, cap; uint8_t *skey; };  /* skey owned */
struct col2 { struct str *v; size_t n; };                   /* values borrowed into data */

int mkz_autocol_encode(const uint8_t *data, size_t len, uint8_t **out, size_t *out_len) {
    int ret = -1, created;

    /* 1. split into lines on '\n' (Rust split: yields trailing empty after a final '\n',
     *    and a single empty element for empty input). */
    size_t nlines = 1;
    for (size_t i = 0; i < len; i++) if (data[i] == '\n') nlines++;
    struct str *lines = (struct str *)malloc(nlines * sizeof *lines);
    struct toks *toks = (struct toks *)calloc(nlines, sizeof *toks);
    size_t *line_gid = (size_t *)malloc(nlines * sizeof *line_gid);
    if (!lines || !toks || !line_gid) goto done0;
    {
        size_t li = 0, start = 0;
        for (size_t i = 0; i < len; i++)
            if (data[i] == '\n') { lines[li].p = data + start; lines[li].n = i - start; li++; start = i + 1; }
        lines[li].p = data + start; lines[li].n = len - start; /* li == nlines-1 */
    }

    /* 2. tokenize every line */
    for (size_t i = 0; i < nlines; i++)
        if (tokenize(lines[i].p, lines[i].n, &toks[i])) goto done0;

    /* 3. group records by skeleton (separator sequence); gid = first-occurrence order */
    struct hmap gmap; int gmap_ok = 0;
    struct grp *groups = NULL; size_t ngrp = 0, gcap = 0, ntmpl = 0;
    struct ob skbuf = {0};
    struct ob *templates = NULL;
    struct col2 *columns = NULL; size_t ncol = 0, ccap = 0;
    struct hmap vmap; int vmap_ok = 0;
    int *chosen = NULL; int64_t **dvals = NULL;
    struct hnode **fd = NULL; size_t nfd = 0;
    struct ob blob = {0};

    if (hm_init(&gmap, nlines)) goto done1;
    gmap_ok = 1;

    for (size_t i = 0; i < nlines; i++) {
        struct str *seps = toks[i].seps; size_t nsep = toks[i].nsep;
        /* skeleton_key = uvarint(nsep) then each (uvarint(len)+bytes) */
        skbuf.len = 0;
        if (ob_uvarint(&skbuf, nsep)) goto done1;
        for (size_t s = 0; s < nsep; s++) {
            if (ob_uvarint(&skbuf, seps[s].n) || ob_push(&skbuf, seps[s].p, seps[s].n)) goto done1;
        }
        struct hnode *nd = hm_get(&gmap, skbuf.d, skbuf.len);
        size_t gid;
        if (nd) {
            gid = nd->u0;
        } else {
            uint8_t *skey = (uint8_t *)malloc(skbuf.len ? skbuf.len : 1);
            if (!skey) goto done1;
            if (skbuf.len) memcpy(skey, skbuf.d, skbuf.len);
            nd = hm_put(&gmap, skey, skbuf.len, &created);
            if (!nd) { free(skey); goto done1; }
            gid = ngrp;
            nd->u0 = gid;
            if (ngrp == gcap) {
                size_t nc = gcap ? gcap * 2 : 16;
                struct grp *ng = (struct grp *)realloc(groups, nc * sizeof *ng);
                if (!ng) { free(skey); goto done1; }
                groups = ng; gcap = nc;
            }
            groups[ngrp].idx = NULL; groups[ngrp].n = 0; groups[ngrp].cap = 0;
            groups[ngrp].skey = skey;
            ngrp++;
        }
        line_gid[i] = gid;
        struct grp *g = &groups[gid];
        if (g->n == g->cap) {
            size_t nc = g->cap ? g->cap * 2 : 8;
            size_t *ni = (size_t *)realloc(g->idx, nc * sizeof *ni);
            if (!ni) goto done1;
            g->idx = ni; g->cap = nc;
        }
        g->idx[g->n++] = i;
    }
    ntmpl = ngrp;

    /* 4. build a template per group + collect variable columns (gid-major, slot order) */
    templates = (struct ob *)calloc(ntmpl ? ntmpl : 1, sizeof *templates);
    if (!templates) goto done1;
    for (size_t gid = 0; gid < ntmpl; gid++) {
        struct grp *g = &groups[gid];
        size_t first = g->idx[0];
        struct str *seps0 = toks[first].seps;
        struct str *words0 = toks[first].words;
        size_t nword = toks[first].nword;

        uint8_t *is_var = (uint8_t *)calloc(nword ? nword : 1, 1);
        if (!is_var) goto done1;
        for (size_t k = 1; k < g->n; k++) {
            struct str *wj = toks[g->idx[k]].words;
            for (size_t j = 0; j < nword; j++)
                if (wj[j].n != words0[j].n || (wj[j].n && memcmp(wj[j].p, words0[j].p, wj[j].n)))
                    is_var[j] = 1;
        }

        struct ob *t = &templates[gid];
        if (ob_uvarint(t, nword)
            || ob_uvarint(t, seps0[0].n) || ob_push(t, seps0[0].p, seps0[0].n)) { free(is_var); goto done1; }
        for (size_t j = 0; j < nword; j++) {
            if (is_var[j]) {
                if (ob_byte(t, 0)) { free(is_var); goto done1; }
            } else {
                if (ob_byte(t, 1) || ob_uvarint(t, words0[j].n) || ob_push(t, words0[j].p, words0[j].n)) { free(is_var); goto done1; }
            }
            if (ob_uvarint(t, seps0[j + 1].n) || ob_push(t, seps0[j + 1].p, seps0[j + 1].n)) { free(is_var); goto done1; }
        }

        for (size_t j = 0; j < nword; j++) {
            if (!is_var[j]) continue;
            if (ncol == ccap) {
                size_t nc = ccap ? ccap * 2 : 16;
                struct col2 *ncols = (struct col2 *)realloc(columns, nc * sizeof *ncols);
                if (!ncols) { free(is_var); goto done1; }
                columns = ncols; ccap = nc;
            }
            struct col2 *C = &columns[ncol];
            C->n = g->n;
            C->v = (struct str *)malloc((g->n ? g->n : 1) * sizeof *C->v);
            if (!C->v) { free(is_var); goto done1; }
            for (size_t k = 0; k < g->n; k++) C->v[k] = toks[g->idx[k]].words[j];
            ncol++;
        }
        free(is_var);
    }

    /* 5. per-column codec selection via a frequency-ranked tentative dictionary */
    if (hm_init(&vmap, 1024)) goto done1;
    vmap_ok = 1;
    for (size_t c = 0; c < ncol; c++)
        for (size_t k = 0; k < columns[c].n; k++) {
            struct hnode *nd = hm_put(&vmap, columns[c].v[k].p, columns[c].v[k].n, &created);
            if (!nd) goto done1;
            nd->u0++;   /* frequency */
        }
    /* rank: frequency desc, then value asc; tent_id = rank index */
    qsort(vmap.order, vmap.norder, sizeof *vmap.order, cmp_rank);
    for (size_t r = 0; r < vmap.norder; r++) vmap.order[r]->u1 = r;

    chosen = (int *)malloc((ncol ? ncol : 1) * sizeof *chosen);
    dvals = (int64_t **)calloc(ncol ? ncol : 1, sizeof *dvals);
    if (!chosen || !dvals) goto done1;
    for (size_t c = 0; c < ncol; c++) {
        struct col2 *C = &columns[c];
        size_t raw_sz = 0;
        for (size_t k = 0; k < C->n; k++) raw_sz += uvlen(C->v[k].n) + C->v[k].n;
        int64_t *ints = NULL;
        int is_int = try_ints(C->v, C->n, &ints);
        size_t best = raw_sz; int codec = 0; /* RAW */
        if (is_int) {
            size_t s = 0; int64_t prev = 0;
            for (size_t k = 0; k < C->n; k++) { s += uvlen(zig(ints[k] - prev)); prev = ints[k]; }
            if (s <= best) { best = s; codec = 1; /* DELTA */ }
        }
        size_t dict_sz = 0;
        for (size_t k = 0; k < C->n; k++) dict_sz += uvlen(hm_get(&vmap, C->v[k].p, C->v[k].n)->u1);
        if (dict_sz < best) codec = 2; /* DICT */
        chosen[c] = codec;
        if (codec == 1) dvals[c] = ints; else free(ints);
    }

    /* 6. final dictionary: values used by dict-ref columns, re-ranked (ascending tent_id) */
    for (size_t c = 0; c < ncol; c++)
        if (chosen[c] == 2)
            for (size_t k = 0; k < columns[c].n; k++)
                hm_get(&vmap, columns[c].v[k].p, columns[c].v[k].n)->flag = 1;
    fd = (struct hnode **)malloc((vmap.norder ? vmap.norder : 1) * sizeof *fd);
    if (!fd) goto done1;
    for (size_t r = 0; r < vmap.norder; r++)        /* vmap.order is now in tent_id order */
        if (vmap.order[r]->flag) { vmap.order[r]->u2 = nfd; fd[nfd++] = vmap.order[r]; }

    /* 7. pack one blob */
    if (ob_byte(&blob, 1)) goto done1;                       /* FORMAT_VERSION */
    if (ob_uvarint(&blob, ntmpl)) goto done1;
    for (size_t gid = 0; gid < ntmpl; gid++)
        if (ob_uvarint(&blob, templates[gid].len) || ob_push(&blob, templates[gid].d, templates[gid].len)) goto done1;
    if (ob_uvarint(&blob, nlines)) goto done1;
    for (size_t i = 0; i < nlines; i++)
        if (ob_uvarint(&blob, line_gid[i])) goto done1;
    if (ob_uvarint(&blob, nfd)) goto done1;
    for (size_t f = 0; f < nfd; f++)
        if (ob_uvarint(&blob, fd[f]->klen) || ob_push(&blob, fd[f]->key, fd[f]->klen)) goto done1;
    if (ob_uvarint(&blob, ncol)) goto done1;
    for (size_t c = 0; c < ncol; c++) {
        struct col2 *C = &columns[c];
        if (chosen[c] == 1) {
            if (ob_byte(&blob, 1) || ob_uvarint(&blob, C->n)) goto done1;
            int64_t prev = 0;
            for (size_t k = 0; k < C->n; k++) { if (ob_uvarint(&blob, zig(dvals[c][k] - prev))) goto done1; prev = dvals[c][k]; }
        } else if (chosen[c] == 2) {
            if (ob_byte(&blob, 2) || ob_uvarint(&blob, C->n)) goto done1;
            for (size_t k = 0; k < C->n; k++)
                if (ob_uvarint(&blob, hm_get(&vmap, C->v[k].p, C->v[k].n)->u2)) goto done1;
        } else {
            if (ob_byte(&blob, 0) || ob_uvarint(&blob, C->n)) goto done1;
            for (size_t k = 0; k < C->n; k++)
                if (ob_uvarint(&blob, C->v[k].n) || ob_push(&blob, C->v[k].p, C->v[k].n)) goto done1;
        }
    }

    *out = blob.d; *out_len = blob.len; blob.d = NULL;
    ret = 0;

done1:
    free(blob.d);
    free(fd);
    if (dvals) { for (size_t c = 0; c < ncol; c++) free(dvals[c]); free(dvals); }
    free(chosen);
    if (vmap_ok) hm_free(&vmap);
    if (columns) { for (size_t c = 0; c < ncol; c++) free(columns[c].v); free(columns); }
    if (templates) { for (size_t gid = 0; gid < ntmpl; gid++) free(templates[gid].d); free(templates); }
    free(skbuf.d);
    if (groups) { for (size_t g = 0; g < ngrp; g++) { free(groups[g].idx); free(groups[g].skey); } free(groups); }
    if (gmap_ok) hm_free(&gmap);
done0:
    if (toks) { for (size_t i = 0; i < nlines; i++) { free(toks[i].seps); free(toks[i].words); } free(toks); }
    free(line_gid);
    free(lines);
    return ret;
}
