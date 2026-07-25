/* stream.c - streaming create/extract for mkz (C port); bounded memory for typical text/logs.
 *
 * Create: a pull-based entry-stream generator (one file open at a time) feeds a line-aligned
 * block reader; each block goes through mkz_pas1_encode_block (raw + autocol gate) and is
 * framed straight to the output FILE*, hashing incrementally. Extract: a buffered FILE reader
 * pulls one framed block at a time, mkz_pas1_decode_block reconstructs it, and a streaming sink
 * writes entries to disk as their bytes arrive, then the SHA-256 trailer is verified.
 *
 * Peak memory ~ one block for typical line-oriented input; a newline-free file is buffered
 * whole and not yet bounded. The decode path is the security surface; it reuses the audited
 * mkz_pas1_decode_block + mkz_safe_join guards.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "stream.h"
#include "pas1.h"
#include "archive.h"
#include "sha256.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MKZ_CHUNK   (1u << 16)   /* 64 KiB working chunk */
#define MKZ_PATHBUF 8192         /* dest/rel build buffer (mkz_safe_join caps rel < 2048) */

/* -- growable byte buffer -- */
struct sbuf { uint8_t *d; size_t len, cap; };
static int sb_reserve(struct sbuf *b, size_t total) {
    if (total <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < total) { size_t d2 = nc * 2; if (d2 < nc) return -1; nc = d2; }
    uint8_t *nd = (uint8_t *)realloc(b->d, nc);
    if (!nd) return -1;
    b->d = nd; b->cap = nc;
    return 0;
}
static int sb_push(struct sbuf *b, const uint8_t *p, size_t n) {
    if (sb_reserve(b, b->len + n)) return -1;
    if (n) memcpy(b->d + b->len, p, n);
    b->len += n;
    return 0;
}

/* uvarint parse from a (possibly incomplete) slice: 0 ok / -1 incomplete / -2 overflow */
static int uv_slice(const uint8_t *s, size_t n, uint64_t *out, size_t *adv) {
    uint64_t v = 0; int sh = 0; size_t i = 0;
    for (;;) {
        if (i >= n) return -1;
        uint8_t b = s[i++];
        v |= (uint64_t)(b & 0x7f) << sh;
        if (!(b & 0x80)) { *out = v; *adv = i; return 0; }
        sh += 7;
        if (sh >= 64) return -2;
    }
}

/* ============================ CREATE ============================ */

/* Pull-based entry-stream source: serves header bytes then file body bytes per entry,
 * with a small pushback buffer so the block reader can line-align without byte-at-a-time IO. */
struct src {
    struct mkz_entry *ents; size_t nent, cur;
    uint8_t *hdr; size_t hlen, hpos, hcap;
    FILE *fp; uint64_t remaining;
    uint8_t *pend; size_t pend_pos, pend_len, pend_cap;
    int verbose;
};

static int hdr_push(struct src *s, const uint8_t *p, size_t n) {
    if (s->hlen + n > s->hcap) {
        size_t nc = s->hcap ? s->hcap : 64;
        while (nc < s->hlen + n) nc *= 2;
        uint8_t *nd = (uint8_t *)realloc(s->hdr, nc);
        if (!nd) return -1;
        s->hdr = nd; s->hcap = nc;
    }
    memcpy(s->hdr + s->hlen, p, n); s->hlen += n;
    return 0;
}
static int hdr_u8(struct src *s, uint8_t x) { return hdr_push(s, &x, 1); }
static int hdr_uv(struct src *s, uint64_t n) {
    uint8_t t[10]; int i = 0;
    for (;;) { uint8_t b = (uint8_t)(n & 0x7f); n >>= 7; if (n) t[i++] = b | 0x80; else { t[i++] = b; break; } }
    return hdr_push(s, t, (size_t)i);
}

/* Begin serving the next entry: build its header, open its file. 1=advanced, 0=EOF, -1=error. */
static int src_advance(struct src *s) {
    if (s->cur >= s->nent) return 0;
    struct mkz_entry *e = &s->ents[s->cur++];
    s->hlen = 0; s->hpos = 0;
    uint8_t tag = e->is_dir ? 1 : 0;
    size_t rl = strlen(e->rel);
    if (hdr_u8(s, tag) || hdr_uv(s, (uint64_t)rl) || hdr_push(s, (const uint8_t *)e->rel, rl)) return -1;
    if (s->verbose) fprintf(stderr, "%s %s\n", e->is_dir ? "d" : "a", e->rel);
    if (!e->is_dir) {
        if (hdr_uv(s, e->size)) return -1;
        s->fp = fopen(e->abs, "rb");
        if (!s->fp) return -1;
        s->remaining = e->size;
    }
    return 1;
}

/* Produce up to `n` bytes of the entry stream into `buf`. *got==0 means clean EOF. 0 / -1. */
static int src_read(struct src *s, uint8_t *buf, size_t n, size_t *got) {
    *got = 0;
    if (n == 0) return 0;
    for (;;) {
        if (s->pend_pos < s->pend_len) {                 /* pushed-back overshoot */
            size_t avail = s->pend_len - s->pend_pos;
            size_t take = avail < n ? avail : n;
            memcpy(buf, s->pend + s->pend_pos, take);
            s->pend_pos += take;
            if (s->pend_pos == s->pend_len) s->pend_pos = s->pend_len = 0;
            *got = take; return 0;
        }
        if (s->hpos < s->hlen) {                         /* header bytes */
            size_t avail = s->hlen - s->hpos;
            size_t take = avail < n ? avail : n;
            memcpy(buf, s->hdr + s->hpos, take);
            s->hpos += take;
            *got = take; return 0;
        }
        if (s->fp) {                                     /* file body */
            if (s->remaining == 0) { fclose(s->fp); s->fp = NULL; }
            else {
                size_t want = ((uint64_t)n > s->remaining) ? (size_t)s->remaining : n;
                size_t r = fread(buf, 1, want, s->fp);
                if (r == 0) return -1;                   /* truncated/shrunk vs stat */
                s->remaining -= r;
                if (s->remaining == 0) { fclose(s->fp); s->fp = NULL; }
                *got = r; return 0;
            }
        }
        int adv = src_advance(s);                        /* move to next entry */
        if (adv < 0) return -1;
        if (adv == 0) { *got = 0; return 0; }            /* clean EOF */
    }
}

static int src_pushback(struct src *s, const uint8_t *p, size_t len) {
    if (len == 0) return 0;                              /* pend is empty here (drained first) */
    if (len > s->pend_cap) {
        uint8_t *nd = (uint8_t *)realloc(s->pend, len);
        if (!nd) return -1;
        s->pend = nd; s->pend_cap = len;
    }
    memcpy(s->pend, p, len);
    s->pend_pos = 0; s->pend_len = len;
    return 0;
}

/* Read one line-aligned block of ~target bytes into `blk` (reset). Sets *eof at stream end.
 * Matches the Rust read_block: take `target`, then extend through the next '\n'. */
static int read_block(struct src *s, size_t target, struct sbuf *blk, int *eof) {
    blk->len = 0; *eof = 0;
    if (sb_reserve(blk, target ? target : 1)) return -1;
    while (blk->len < target) {
        size_t got;
        if (src_read(s, blk->d + blk->len, target - blk->len, &got)) return -1;
        if (got == 0) { *eof = 1; break; }
        blk->len += got;
    }
    if (!*eof && blk->len > 0 && blk->d[blk->len - 1] != '\n') {
        uint8_t tmp[MKZ_CHUNK];
        for (;;) {
            size_t got;
            if (src_read(s, tmp, sizeof tmp, &got)) return -1;
            if (got == 0) { *eof = 1; break; }
            size_t p = 0; int found = 0;
            for (; p < got; p++) if (tmp[p] == '\n') { found = 1; break; }
            if (found) {
                if (sb_push(blk, tmp, p + 1)) return -1;
                if (src_pushback(s, tmp + p + 1, got - (p + 1))) return -1;
                break;
            }
            if (sb_push(blk, tmp, got)) return -1;
        }
    }
    return 0;
}

static int wr_u8(FILE *f, uint8_t x) { return fputc(x, f) == EOF ? -1 : 0; }
static int wr_uv(FILE *f, uint64_t n) {
    for (;;) { uint8_t b = (uint8_t)(n & 0x7f); n >>= 7; if (n) { if (fputc(b | 0x80, f) == EOF) return -1; } else return fputc(b, f) == EOF ? -1 : 0; }
}

int mkz_create_stream(const char *const *paths, size_t npaths, const char *archive,
                      int level, size_t block, int verbose) {
    if (npaths == 0) return -1;
    if (block == 0) block = 16u << 20;

    struct mkz_entry *ents = NULL; size_t nent = 0;
    if (mkz_collect_entries(paths, npaths, &ents, &nent)) return -1;

    FILE *out = fopen(archive, "wb");
    if (!out) { mkz_free_entries(ents, nent); return -1; }

    struct src s = {0};
    s.ents = ents; s.nent = nent; s.verbose = verbose;
    struct sbuf blk = {0};
    struct mkz_sha256_ctx sha; mkz_sha256_init(&sha);
    struct mkz_pas1_stats stt = {0};
    int rc = -1;

    if (fwrite("PAS1", 1, 4, out) != 4) goto done;
    for (;;) {
        int eof;
        if (read_block(&s, block, &blk, &eof)) goto done;
        if (blk.len == 0) break;
        mkz_sha256_update(&sha, blk.d, blk.len);
        uint8_t flags = 0, *payload = NULL; size_t payload_len = 0;
        if (mkz_pas1_encode_block_stats(blk.d, blk.len, level, &flags, &payload, &payload_len, &stt)) goto done;
        int e = wr_u8(out, 1) || wr_u8(out, flags)
                || wr_uv(out, (uint64_t)blk.len) || wr_uv(out, (uint64_t)payload_len)
                || (payload_len && fwrite(payload, 1, payload_len, out) != payload_len);
        free(payload);
        if (e) goto done;
        if (eof) break;
    }
    {
        uint8_t digest[32];
        mkz_sha256_final(&sha, digest);
        if (wr_u8(out, 0) || fwrite(digest, 1, 32, out) != 32) goto done;
    }
    if (verbose) {
        /* The gate compresses every block both ways, so the "vs zstd alone" comparison is
         * exact, not an estimate. saved >= 0 always (that's the never-worse guarantee). */
        uint64_t saved = stt.zstd_alone_bytes - stt.payload_bytes;
        fprintf(stderr,
                "mkz: autocol kept %llu/%llu blocks (%.1f%%); payloads %.1f%% of stream; "
                "saved %llu bytes (%.1f%%) vs zstd alone\n",
                (unsigned long long)stt.autocol_blocks, (unsigned long long)stt.blocks,
                stt.blocks ? 100.0 * (double)stt.autocol_blocks / (double)stt.blocks : 0.0,
                stt.orig_bytes ? 100.0 * (double)stt.payload_bytes / (double)stt.orig_bytes : 0.0,
                (unsigned long long)saved,
                stt.zstd_alone_bytes ? 100.0 * (double)saved / (double)stt.zstd_alone_bytes : 0.0);
    }
    rc = 0;
done:
    if (s.fp) fclose(s.fp);
    free(s.hdr); free(s.pend); free(blk.d);
    if (fclose(out) != 0) rc = -1;
    mkz_free_entries(ents, nent);
    return rc;
}

/* ============================ EXTRACT ============================ */

struct frdr { FILE *f; uint64_t pos, size; };
static int fr_exact(struct frdr *r, uint8_t *buf, size_t n) {
    if (n == 0) return 0;
    if (fread(buf, 1, n, r->f) != n) return -1;
    r->pos += n; return 0;
}
static int fr_u8(struct frdr *r, uint8_t *out) {
    int c = fgetc(r->f);
    if (c == EOF) return -1;
    r->pos++; *out = (uint8_t)c; return 0;
}
static int fr_uv(struct frdr *r, uint64_t *out) {
    uint64_t v = 0; int sh = 0;
    for (;;) {
        int c = fgetc(r->f);
        if (c == EOF) return -1;
        r->pos++;
        v |= (uint64_t)(c & 0x7f) << sh;
        if (!(c & 0x80)) { *out = v; return 0; }
        sh += 7;
        if (sh >= 64) return -1;
    }
}

/* Streaming sink: accepts decoded entry-stream bytes and writes files/dirs under dest as the
 * bytes arrive. Buffers only headers (file content streams straight to disk). */
struct sink {
    const char *dest;
    int state;            /* 0 = header, 1 = body */
    struct sbuf buf;      /* unconsumed bytes (headers + carryover) */
    FILE *fp; uint64_t remaining;
    int verbose;
};

static int sink_drain(struct sink *s) {
    size_t i = 0;
    int ret = -1;
    for (;;) {
        if (s->state == 1) {                              /* writing a file body */
            if (s->remaining == 0) {
                if (s->fp) { if (fclose(s->fp) != 0) { s->fp = NULL; goto out; } s->fp = NULL; }
                s->state = 0;
                continue;
            }
            size_t avail = s->buf.len - i;
            if (avail == 0) break;
            size_t take = (s->remaining < avail) ? (size_t)s->remaining : avail;
            if (fwrite(s->buf.d + i, 1, take, s->fp) != take) goto out;
            i += take; s->remaining -= take;
        } else {                                          /* parsing an entry header */
            const uint8_t *p = s->buf.d + i; size_t avail = s->buf.len - i;
            if (avail == 0) break;
            uint8_t tag = p[0];
            uint64_t plen; size_t adv;
            int uv = uv_slice(p + 1, avail - 1, &plen, &adv);
            if (uv == -1) break;                          /* need more bytes */
            if (uv == -2) goto out;                        /* malformed */
            if (plen >= 2048) goto out;                    /* path too long (mkz_safe_join cap) */
            size_t off1 = 1 + adv;
            if (plen > (uint64_t)(avail - off1)) break;    /* path bytes not all here yet */
            const uint8_t *rel = p + off1; size_t rel_len = (size_t)plen;
            char path[MKZ_PATHBUF];
            if (mkz_safe_join(s->dest, rel, rel_len, path, sizeof path)) goto out;

            if (tag == 1) {                                /* directory */
                if (mkz_mkdir_p(path)) goto out;
                if (s->verbose) fprintf(stderr, "x %.*s\n", (int)rel_len, (const char *)rel);
                i += off1 + rel_len;
            } else if (tag == 0) {                         /* file */
                uint64_t fsz; size_t adv2;
                int uv2 = uv_slice(p + off1 + rel_len, avail - off1 - rel_len, &fsz, &adv2);
                if (uv2 == -1) break;
                if (uv2 == -2) goto out;
                char parent[MKZ_PATHBUF];
                size_t pn = strlen(path);
                memcpy(parent, path, pn + 1);
                char *slash = strrchr(parent, '/');
                if (slash) { *slash = '\0'; if (parent[0] && mkz_mkdir_p(parent)) goto out; }
                FILE *fp = fopen(path, "wb");
                if (!fp) goto out;
                if (s->verbose) fprintf(stderr, "x %.*s\n", (int)rel_len, (const char *)rel);
                i += off1 + rel_len + adv2;
                s->fp = fp; s->remaining = fsz; s->state = 1;
            } else {
                goto out;                                  /* bad tag */
            }
        }
    }
    ret = 0;
out:
    if (i) { memmove(s->buf.d, s->buf.d + i, s->buf.len - i); s->buf.len -= i; }
    return ret;
}

static int sink_feed(struct sink *s, const uint8_t *data, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t take = (n - off < MKZ_CHUNK) ? (n - off) : MKZ_CHUNK;
        if (sb_push(&s->buf, data + off, take)) return -1;
        if (sink_drain(s)) return -1;
        off += take;
    }
    return 0;
}

/* Move every entry under src into dst: directories are merged (mkz_mkdir_p + recurse),
 * files are renamed over (atomic replace; staging lives under dest, same filesystem).
 * Emptied source dirs are removed with rmdir (refuses non-empty). 0 / -1. */
static int move_tree(const char *src, const char *dst) {
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e; int rc = 0;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char from[MKZ_PATHBUF], to[MKZ_PATHBUF];
        if ((size_t)snprintf(from, sizeof from, "%s/%s", src, e->d_name) >= sizeof from ||
            (size_t)snprintf(to, sizeof to, "%s/%s", dst, e->d_name) >= sizeof to) { rc = -1; break; }
        struct stat st;
        if (lstat(from, &st)) { rc = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            if (mkz_mkdir_p(to) || move_tree(from, to)) { rc = -1; break; }
            rmdir(from);
        } else {
            if (rename(from, to)) { rc = -1; break; }
        }
    }
    closedir(d);
    return rc;
}

int mkz_extract_stream(const char *archive, const char *dest, int verbose) {
    FILE *f = fopen(archive, "rb");
    if (!f) return -1;
    struct frdr r = { f, 0, 0 };
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    r.size = (uint64_t)sz;

    if (mkz_mkdir_p(dest)) { fclose(f); return -1; }

    /* Stream entries into a staging dir under dest first; only after the SHA-256 trailer
     * verifies do we merge-move staging into dest (see move_tree below). A failure before
     * that point leaves dest exactly as it was; the caller only ever sees "nothing was
     * placed" for a failure that happens here. */
    char staging[MKZ_PATHBUF];
    if ((size_t)snprintf(staging, sizeof staging, "%s/.mkz-partial.%ld",
                         dest, (long)getpid()) >= sizeof staging) { fclose(f); return -1; }
    if (mkz_mkdir_p(staging)) { fclose(f); return -1; }

    struct mkz_sha256_ctx sha; mkz_sha256_init(&sha);
    struct sink sink = {0};
    sink.dest = staging; sink.verbose = verbose;
    int rc = -1;

    uint8_t magic[4];
    if (fr_exact(&r, magic, 4) || memcmp(magic, "PAS1", 4) != 0) goto done;
    for (;;) {
        uint8_t tag;
        if (fr_u8(&r, &tag)) goto done;
        if (tag == 0) break;
        if (tag != 1) goto done;
        uint8_t flags;
        if (fr_u8(&r, &flags)) goto done;
        uint64_t orig_len, comp_len;
        if (fr_uv(&r, &orig_len) || fr_uv(&r, &comp_len)) goto done;
        if (comp_len > r.size - r.pos) goto done;          /* payload must fit the rest of the file */

        uint8_t *payload = (uint8_t *)malloc(comp_len ? (size_t)comp_len : 1);
        if (!payload) goto done;
        if (fr_exact(&r, payload, (size_t)comp_len)) { free(payload); goto done; }

        uint8_t *block = NULL; size_t block_len = 0;
        int de = mkz_pas1_decode_block(payload, (size_t)comp_len, flags, orig_len, &block, &block_len);
        free(payload);
        if (de) goto done;

        mkz_sha256_update(&sha, block, block_len);
        int fe = sink_feed(&sink, block, block_len);
        free(block);
        if (fe) goto done;
    }

    {
        uint8_t want[32], got[32];
        if (fr_exact(&r, want, 32)) goto done;
        if (r.pos != r.size) goto done;                    /* trailing garbage */
        mkz_sha256_final(&sha, got);
        if (memcmp(want, got, 32) != 0) goto done;          /* SHA-256 mismatch: nothing placed (entries only ever reached staging) */
        if (sink.state != 0 || sink.fp != NULL || sink.buf.len != 0) goto done; /* truncated entry */
    }

    /* SHA-256 verified: place the staged data into dest. move_tree failing here is a
     * placement failure, NOT "nothing was placed" - some entries may already be in dest
     * by the time it errors out (e.g. a pre-existing plain file where the archive has a
     * directory, disk-full, permissions), so it gets its own, different message below. */
    if (move_tree(staging, dest)) {
        fprintf(stderr, "mkz: extraction FAILED while placing verified data into %s; "
                        "a PARTIAL merge may have occurred. Remaining staged data left "
                        "for inspection at %s\n", dest, staging);
        goto cleanup;
    }
    if (rmdir(staging) != 0)   /* empty after the merge; never removes content */
        fprintf(stderr, "mkz: warning: could not remove staging dir %s after extract: %s\n",
                staging, strerror(errno));
    rc = 0;
    goto cleanup;
done:
    fprintf(stderr, "mkz: extraction FAILED; nothing was placed in %s. "
                    "Partial data left for inspection at %s\n", dest, staging);
cleanup:
    if (sink.fp) fclose(sink.fp);
    free(sink.buf.d);
    fclose(f);
    return rc;
}
