#ifndef FILM_NATIVE_MEDIA_H
#define FILM_NATIVE_MEDIA_H
#include "film.h"

/* Borrowed encoded members, excluding MP-entry padding; caller keeps input alive.
 * No pixel decoding,
 * filesystem access, allocation, camera calls or original-file modification. */
typedef struct {
    size_t offset, bytes;
    unsigned width, height;
} FilmNativeJpeg;

typedef struct {
    unsigned count;
    FilmNativeJpeg images[2];
} FilmNativeMedia;

FilmStatus film_native_media_inspect(const char *source, const uint8_t *data,
                                    size_t bytes, FilmNativeMedia *out);
#endif
