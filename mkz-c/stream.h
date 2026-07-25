/* stream.h - streaming create/extract for mkz (C port); bounded memory for typical text/logs.
 *
 * Both directions process the archive one block at a time: peak memory ~ one block
 * (plus the entry list, which scales with file COUNT not size). Bounded for typical
 * line-oriented input, matching the Rust mkz; pathological newline-free input is buffered
 * whole and not yet bounded. The CLI uses these; the in-memory
 * mkz_pas1_* / mkz_archive_* remain for tests and as the reference path.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef MKZ_STREAM_H
#define MKZ_STREAM_H

#include <stdint.h>
#include <stddef.h>

/* Create: walk `paths`, stream the tar-style entry bytes through line-aligned blocks (autocol
 * pre-pass + never-worse gate per block), write the PAS1 archive to `archive` with a streaming
 * SHA-256 trailer. Only one input file is open at a time. Returns 0 / -1. */
int mkz_create_stream(const char *const *paths, size_t npaths, const char *archive,
                      int level, size_t block, int verbose);

/* Extract: read PAS1 `archive` block-by-block, decode each block (zstd + autocol), stream the
 * reconstructed entries into a staging dir under `dest`, then verify the SHA-256 trailer
 * (and that the stream ended cleanly). Paranoid: the decode path bounds-checks everything and
 * the sink rejects path traversal. Returns 0 / -1; atomic: entries are written to staging, not
 * dest, so a failed check leaves dest untouched. Only after the trailer verifies is staging
 * merge-moved into dest; the staging dir (.mkz-partial.<pid>) is retained and reported on any
 * failure, including one during that final move. */
int mkz_extract_stream(const char *archive, const char *dest, int verbose);

#endif /* MKZ_STREAM_H */
