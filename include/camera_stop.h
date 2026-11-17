#ifndef FILM_CAMERA_STOP_H
#define FILM_CAMERA_STOP_H
#include "film.h"
typedef struct {
    void *context;
    FilmStatus (*stop)(void *);
    FilmStatus (*clear)(void *);
    FilmStatus (*busy)(void *,bool *);
    void (*delay)(void *,unsigned);
} FilmCameraStopIo;
FilmStatus film_camera_stop_transaction(const char *,FilmCameraStopIo,bool *dma_safe);
#endif
