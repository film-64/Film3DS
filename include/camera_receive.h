#ifndef FILM_CAMERA_RECEIVE_H
#define FILM_CAMERA_RECEIVE_H
#include "film.h"
typedef struct {
    void *context;
    FilmStatus (*wait)(void *,unsigned);
    FilmStatus (*finished)(void *,bool *);
    uint64_t (*now_ms)(void *);
    void (*delay)(void *,unsigned);
} FilmCameraReceiveIo;
FilmStatus film_camera_receive_complete(const char *,FilmCameraReceiveIo);
#endif
