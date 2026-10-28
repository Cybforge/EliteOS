#ifndef ZENITH_INFLATE_H
#define ZENITH_INFLATE_H

#include <kernel.h>

/* 0 on success, -1 corrupt data, -2 output too small, -3 out of memory */
int inflate_raw(const uint8_t *src, size_t n, uint8_t *out, size_t cap, size_t *out_len);
int zlib_inflate(const uint8_t *src, size_t n, uint8_t *out, size_t cap, size_t *out_len);

#endif
