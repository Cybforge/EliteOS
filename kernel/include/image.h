#ifndef ZENITH_IMAGE_H
#define ZENITH_IMAGE_H

#include <gfx.h>

/* PNG, baseline JPEG and BMP; NULL if the data is not understood */
surface_t *image_decode(const void *data, size_t n);
surface_t *image_load(const char *path);
bool       image_is_supported(const char *name);

#endif
