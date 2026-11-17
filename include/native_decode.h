#ifndef FILM_NATIVE_DECODE_H
#define FILM_NATIVE_DECODE_H
#include "native_media.h"

/* Synchronous single-member RGB24 decoding into caller-owned rows. Decode each
 * MPO member explicitly. The caller keeps encoded input and output alive and
 * displays output only on FILM_OK. On failure dimensions are zero; the pixel
 * buffer may contain partial output. Invalid dimension-pointer aliasing is
 * rejected without writes. No Exif orientation or sensor-RAW decoding.
 * Link the verified libjpeg build with no backing store (jmemnobs). */
FilmStatus film_native_jpeg_decode(const char *source, const uint8_t *data, size_t bytes,
                                  unsigned member, uint8_t *pixels, size_t stride,
                                  size_t capacity, unsigned *width, unsigned *height);
#endif
