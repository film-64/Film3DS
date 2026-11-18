#ifndef FILM_AE_REGION_H
#define FILM_AE_REGION_H
#include "film.h"

/* Native VGA sensor coordinates, independent of the 400x240 display. The
 * rectangle is the actual requested CAMU AE window, not an overlay guess. */
typedef struct {
    unsigned x,y,width,height;
} FilmAeRegion;
enum { FILM_AE_VGA_WIDTH=640, FILM_AE_VGA_HEIGHT=480, FILM_AE_LEVELS=5 };
FilmAeRegion film_ae_region_default(void);
bool film_ae_region_valid(FilmAeRegion r);
bool film_ae_region_is_full(FilmAeRegion r);
FilmAeRegion film_ae_region_resize(FilmAeRegion r,int direction);
FilmAeRegion film_ae_region_move(FilmAeRegion r,int dx,int dy);
#endif
