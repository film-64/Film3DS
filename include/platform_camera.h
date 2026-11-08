#ifndef FILM_PLATFORM_CAMERA_H
#define FILM_PLATFORM_CAMERA_H
#include "film.h"

typedef struct {
    bool initialized;
    unsigned transfer;
    FilmPixelFormat format;
    uint32_t last_result;
} FilmCamera;

FilmStatus film_camera_open(FilmCamera *camera, FilmPixelFormat format);
FilmStatus film_camera_capture(FilmCamera *camera, FilmFrame *frame);
void film_camera_close(FilmCamera *camera);
FilmStatus film_y2r_convert(const FilmImage *image, FilmYuv matrix, FilmImage *out, uint32_t *result);
#endif
