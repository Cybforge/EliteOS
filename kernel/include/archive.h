#ifndef ZENITH_ARCHIVE_H
#define ZENITH_ARCHIVE_H

#include <kernel.h>

/* gzip (RFC 1952): kmalloc'd result with a terminating NUL, NULL if damaged or larger than max */
uint8_t *gunzip_alloc(const uint8_t *data, size_t n, size_t *out_len, size_t max);

/*
 * Unpack a tar archive (ustar or GNU, files and directories) below `dest`.
 * Names that would leave `dest` (absolute paths, "..") are refused. The
 * optional callback sees every path before it is written and can skip it.
 */
int tar_extract(const uint8_t *tar, size_t len, const char *dest, char *err, size_t errn, int *nfiles);

/* the file named `name` inside a tar archive (no copy), NULL if absent */
const uint8_t *tar_find(const uint8_t *tar, size_t len, const char *name, size_t *size);

#endif
