/* pas1.c — PAS1 stream container for mkz (C port). RAW blocks (zstd) + SHA-256 trailer.
 * Format-identical to the Rust mkz container. The decoder is the security surface:
 * every untrusted length/offset is bounds- and overflow-checked before use.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#include "pas1.h"
#include "sha256.h"
#include "autocol.h"
#include <zstd.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t MAGIC[4] = {'P', 'A', 'S', '1'};

/* Sanity cap on a single block's declared decompressed size (anti-DoS / decompression bomb). */
#define MKZ_MAX_BLOCK_ORIG (1ull << 34) /* 16 GiB */

/* ---- growable byte buffer ---- */
struct buf {
    uint8_t *d;
    size_t len, cap;
};
static int buf_reserve(struct buf *b, size_t extra) {
    if (extra <= b->cap - b->len) return 0;
    size_t need = b->len + extra;
    if (need < b->len) return -1; /* size_t overflow */
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < need) {
        size_t d2 = ncap * 2;
        if (d2 < ncap) return -1;
        ncap = d2;
    }
    uint8_t *nd = (uint8_t *)realloc(b->d, ncap);
    if (!nd) return -1;
    b->d = nd;
    b->cap = ncap;
    return 0;
}
static int buf_push(struct buf *b, const uint8_t *p, size_t n) {
    if (buf_reserve(b, n)) return -1;
    if (n) memcpy(b->d + b->len, p, n);
    b->len += n;
    return 0;
}
static int buf_byte(struct buf *b, uint8_t x) { return buf_push(b, &x, 1); }
static int buf_uvarint(struct buf *b, uint64_t n) {
    uint8_t tmp[10];
    int i = 0;
    for (;;) {
        uint8_t byte = (uint8_t)(n & 0x7f);
        n >>= 7;
        if (n) {
            tmp[i++] = byte | 0x80;
        } else {
            tmp[i++] = byte;
            break;
        }
    }
    return buf_push(b, tmp, (size_t)i);
}

/* ---- paranoid uvarint read from untrusted input ---- */
static int read_uvarint(const uint8_t *in, size_t in_len, size_t *pos, uint64_t *out) {
    uint64_t val = 0;
    int shift = 0;
    for (;;) {
        if (*pos >= in_len) return -1; /* truncated */
        uint8_t b = in[*pos];
        (*pos)++;
        val |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = val;
            return 0;
        }
        shift += 7;
        if (shift >= 64) return -1; /* overflow */
    }
}

/* Decompress ONE zstd frame of a-priori-unknown size (Rust's encode_all does not pledge the
 * content size into the frame header) into a malloc'd buffer, bomb-capped. 0 / -1. */
static int zstd_grow(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len) {
    ZSTD_DStream *ds = ZSTD_createDStream();
    if (!ds) return -1;
    ZSTD_initDStream(ds);
    struct buf o = {0};
    ZSTD_inBuffer in = { src, src_len, 0 };
    uint8_t chunk[1 << 16];
    int rc = -1;
    for (;;) {
        ZSTD_outBuffer ob = { chunk, sizeof chunk, 0 };
        size_t r = ZSTD_decompressStream(ds, &ob, &in);
        if (ZSTD_isError(r)) goto done;
        if (o.len + ob.pos > MKZ_MAX_BLOCK_ORIG) goto done; /* bomb cap */
        if (buf_push(&o, chunk, ob.pos)) goto done;
        if (r == 0) break;                                  /* frame complete */
        if (ob.pos == 0 && in.pos == in.size) goto done;    /* truncated / no progress */
    }
    *out = o.d; *out_len = o.len; o.d = NULL; rc = 0;
done:
    ZSTD_freeDStream(ds);
    free(o.d);
    return rc;
}

/* zstd-compress one buffer at `level` into a malloc'd buffer. 0 / -1. */
static int zstd_one(const uint8_t *src, size_t n, int level, uint8_t **out, size_t *out_len) {
    size_t bound = ZSTD_compressBound(n);
    uint8_t *buf = (uint8_t *)malloc(bound ? bound : 1);
    if (!buf) return -1;
    size_t csz = ZSTD_compress(buf, bound, src, n, level);
    if (ZSTD_isError(csz)) { free(buf); return -1; }
    *out = buf; *out_len = csz;
    return 0;
}

/* Encode ONE block: raw-zstd baseline + autocol pre-pass behind the never-worse gate (keep
 * autocol only when it round-trips via the C decoder AND zstd's strictly smaller than raw).
 * Sets *flags (bit0 = autocol) and mallocs *payload (caller frees). Shared by the in-memory
 * and streaming compressors so the gate lives in exactly one place. 0 / -1. */
int mkz_pas1_encode_block_stats(const uint8_t *data, size_t len, int level,
                                uint8_t *flags, uint8_t **payload, size_t *payload_len,
                                struct mkz_pas1_stats *st) {
    uint8_t *craw = NULL; size_t craw_len = 0;
    if (zstd_one(data, len, level, &craw, &craw_len)) return -1;

    uint8_t fl = 0;
    uint8_t *chosen = craw; size_t chosen_len = craw_len;

    uint8_t *ac = NULL; size_t ac_len = 0;
    if (mkz_autocol_encode(data, len, &ac, &ac_len) == 0) {
        uint8_t *chk = NULL; size_t chk_len = 0;
        if (mkz_autocol_decode(ac, ac_len, &chk, &chk_len) == 0
            && chk_len == len && (len == 0 || memcmp(chk, data, len) == 0)) {
            uint8_t *cac = NULL; size_t cac_len = 0;
            if (zstd_one(ac, ac_len, level, &cac, &cac_len) == 0) {
                if (cac_len < craw_len) { fl = 1; chosen = cac; chosen_len = cac_len; free(craw); }
                else free(cac);
            }
        }
        free(chk);
    }
    free(ac);

    if (st) {
        st->blocks++;
        st->autocol_blocks += (uint64_t)(fl & 1);
        st->orig_bytes += (uint64_t)len;
        st->payload_bytes += (uint64_t)chosen_len;
        st->zstd_alone_bytes += (uint64_t)craw_len;
    }

    *flags = fl; *payload = chosen; *payload_len = chosen_len;
    return 0;
}

int mkz_pas1_encode_block(const uint8_t *data, size_t len, int level,
                          uint8_t *flags, uint8_t **payload, size_t *payload_len) {
    return mkz_pas1_encode_block_stats(data, len, level, flags, payload, payload_len, NULL);
}

/* Decode ONE block payload back to its original bytes. zstd-decompress (streaming, since the
 * frame content size isn't pledged), then autocol-decode if flags&1, then verify the result
 * length equals the declared orig_len. mallocs *out (caller frees). 0 / -1. The decoder is
 * the security surface: orig_len is bomb-capped here; the caller bounds comp_len/payload_len
 * against the actual input. */
int mkz_pas1_decode_block(const uint8_t *payload, size_t payload_len, uint8_t flags,
                          uint64_t orig_len, uint8_t **out, size_t *out_len) {
    if (orig_len > MKZ_MAX_BLOCK_ORIG) return -1;
    uint8_t *backend = NULL; size_t blen = 0;
    if (zstd_grow(payload, payload_len, &backend, &blen)) return -1;
    if (flags & 1) {
        uint8_t *plain = NULL; size_t plain_len = 0;
        int de = mkz_autocol_decode(backend, blen, &plain, &plain_len);
        free(backend);
        if (de || plain_len != orig_len) { free(plain); return -1; }
        *out = plain; *out_len = plain_len;
    } else {
        if (blen != orig_len) { free(backend); return -1; }
        *out = backend; *out_len = blen;
    }
    return 0;
}

int mkz_pas1_compress(const uint8_t *data, size_t len, int zstd_level, size_t block_size,
                      uint8_t **out, size_t *out_len) {
    if (block_size == 0) block_size = 1u << 20;
    struct buf b = {0};
    if (buf_push(&b, MAGIC, 4)) goto fail;

    size_t off = 0;
    while (off < len) {
        /* line-aligned block: take up to block_size, then extend through the next '\n'
         * (so a block holds whole lines — the autocol transform needs that). Matches the
         * Rust read_block. */
        size_t blk = (len - off < block_size) ? (len - off) : block_size;
        size_t end = off + blk;
        if (end < len && data[end - 1] != '\n')
            while (end < len && data[end - 1] != '\n') end++;
        size_t blen = end - off;

        uint8_t flags = 0, *payload = NULL; size_t payload_len = 0;
        if (mkz_pas1_encode_block(data + off, blen, zstd_level, &flags, &payload, &payload_len)) goto fail;
        int e = buf_byte(&b, 1) || buf_byte(&b, flags)
                || buf_uvarint(&b, (uint64_t)blen) || buf_uvarint(&b, (uint64_t)payload_len)
                || buf_push(&b, payload, payload_len);
        free(payload);
        if (e) goto fail;
        off = end;
    }
    if (buf_byte(&b, 0)) goto fail; /* end-of-blocks */
    uint8_t digest[32];
    mkz_sha256(len ? data : (const uint8_t *)"", len, digest);
    if (buf_push(&b, digest, 32)) goto fail;

    *out = b.d;
    *out_len = b.len;
    return 0;
fail:
    free(b.d);
    return -1;
}

int mkz_pas1_decompress(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    if (in_len < 4 || memcmp(in, MAGIC, 4) != 0) return -1;
    size_t pos = 4;
    struct buf ob = {0};

    for (;;) {
        if (pos >= in_len) goto fail;       /* need a tag byte */
        uint8_t tag = in[pos++];
        if (tag == 0) break;                /* end-of-blocks */
        if (tag != 1) goto fail;            /* unknown tag */
        if (pos >= in_len) goto fail;       /* need flags */
        uint8_t flags = in[pos++];

        uint64_t orig_len, comp_len;
        if (read_uvarint(in, in_len, &pos, &orig_len)) goto fail;
        if (read_uvarint(in, in_len, &pos, &comp_len)) goto fail;
        if (comp_len > in_len - pos) goto fail;           /* payload must fit remaining input */

        uint8_t *block = NULL; size_t block_len = 0;
        if (mkz_pas1_decode_block(in + pos, (size_t)comp_len, flags, orig_len, &block, &block_len)) goto fail;
        pos += (size_t)comp_len;
        int e = buf_push(&ob, block, block_len);
        free(block);
        if (e) goto fail;
    }

    /* trailer: exactly a 32-byte SHA-256 of the reconstructed original */
    if (in_len - pos != 32) goto fail;
    uint8_t digest[32];
    mkz_sha256(ob.len ? ob.d : (const uint8_t *)"", ob.len, digest);
    if (memcmp(digest, in + pos, 32) != 0) goto fail; /* refuse corrupt output */

    *out = ob.d;
    *out_len = ob.len;
    return 0;
fail:
    free(ob.d);
    return -1;
}
