/* autocol.h - psrc-autocol transform for mkz (C port).
 *
 * Decode a psrc-autocol blob (FORMAT_VERSION 1) back to the original bytes. The blob is
 * the DECOMPRESSED content of a flags&1 PAS1 block, or the raw output of `mkz transform`.
 * Format-identical to the Rust psrc_autocol; this is the decode half (encode follows).
 *
 * The decoder parses untrusted bytes; every length/offset/index is checked.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef MKZ_AUTOCOL_H
#define MKZ_AUTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* Decode an autocol blob into the original bytes. mallocs *out (caller frees).
 * Returns 0 on success, -1 on any malformed / out-of-range / unsupported input. */
int mkz_autocol_decode(const uint8_t *blob, size_t blob_len, uint8_t **out, size_t *out_len);

/* Encode `data` into a packed autocol blob (FORMAT_VERSION 1). mallocs *out (caller frees).
 * A faithful port of Rust psrc_autocol::encode: same tokenization, skeleton grouping,
 * per-column codec selection {raw,delta,dict-ref} and packing, so the blob is byte-identical
 * to `mkz transform`. Pure and reversible (mkz_autocol_decode inverts it exactly).
 * Returns 0 on success, -1 only on allocation failure. Input is trusted (our own bytes). */
int mkz_autocol_encode(const uint8_t *data, size_t len, uint8_t **out, size_t *out_len);

#endif /* MKZ_AUTOCOL_H */
