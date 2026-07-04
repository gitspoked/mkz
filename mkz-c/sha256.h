/* sha256.h — minimal one-shot SHA-256 (FIPS 180-4) for mkz, so libzstd is the only
 * hard external dep. Public-domain-style implementation.
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef MKZ_SHA256_H
#define MKZ_SHA256_H

#include <stdint.h>
#include <stddef.h>

/* Compute SHA-256 of `data[0..len]` into `out` (32 bytes). */
void mkz_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* Incremental SHA-256 — for streaming, so a multi-GB archive is hashed block-by-block
 * without ever holding it in memory. mkz_sha256 above is just init+update+final. */
struct mkz_sha256_ctx {
    uint32_t state[8];
    uint8_t buf[64];
    size_t buflen;
    uint64_t total;
};
void mkz_sha256_init(struct mkz_sha256_ctx *c);
void mkz_sha256_update(struct mkz_sha256_ctx *c, const uint8_t *data, size_t len);
void mkz_sha256_final(struct mkz_sha256_ctx *c, uint8_t out[32]);

#endif /* MKZ_SHA256_H */
