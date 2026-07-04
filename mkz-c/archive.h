/* archive.h — tar-style entry-stream unpack for mkz (C port).
 *
 * The entry stream (the byte stream a PAS1 archive carries) is a flat concatenation of
 * entries: [tag u8: 0=file/1=dir][uvarint pathlen][path][file only: uvarint size + content].
 * Extract recreates the tree under `dest`. Paranoid: bounds-checked, rejects path traversal
 * (absolute paths, "..", ".", embedded NUL).
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef MKZ_ARCHIVE_H
#define MKZ_ARCHIVE_H

#include <stdint.h>
#include <stddef.h>

/* Extract entry stream `es[0..es_len]` into files/dirs under `dest`.
 * Returns 0 on success, -1 on any malformed / unsafe-path / IO error. */
int mkz_archive_extract(const uint8_t *es, size_t es_len, const char *dest, int verbose);

/* Build a flat entry stream from `paths` (regular files / directories), mallocs *es
 * (caller frees). Mirrors the Rust create order: each directory is emitted before its
 * name-sorted children, rel paths normalized (leading root / "." / ".." dropped).
 * Symlinks and special files are skipped. Returns 0 on success, -1 on IO error / bad
 * argument / path too long. In-memory (holds the whole stream). */
int mkz_archive_build(const char *const *paths, size_t npaths, int verbose,
                      uint8_t **es, size_t *es_len);

/* One archive entry. `rel` is the normalized stored path (always set); `abs` is the
 * on-disk path for files (NULL for dirs); `size` is the file size (0 for dirs). */
struct mkz_entry { int is_dir; char *rel; char *abs; uint64_t size; };

/* Walk `paths` into a flat entry list in canonical order (dir before name-sorted children).
 * mallocs *out (free with mkz_free_entries). Used by the streaming create source so file
 * contents are read lazily, one at a time. Returns 0 / -1. */
int mkz_collect_entries(const char *const *paths, size_t npaths, struct mkz_entry **out, size_t *n);
void mkz_free_entries(struct mkz_entry *e, size_t n);

/* Path-safety helpers shared with the streaming sink. mkz_mkdir_p = mkdir -p; mkz_safe_join
 * validates a stored rel path (rejects absolute / "." / ".." / empty / embedded NUL) and
 * writes dest/rel into out. Both return 0 / -1. */
int mkz_mkdir_p(const char *path);
int mkz_safe_join(const char *dest, const uint8_t *rel, size_t rel_len, char *out, size_t out_sz);

#endif /* MKZ_ARCHIVE_H */
