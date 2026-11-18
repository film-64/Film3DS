#ifndef FILM_STORAGE_INTERNAL_H
#define FILM_STORAGE_INTERNAL_H

#include "film.h"

typedef struct { FILE *file; char *temporary; const char *destination; } FilmWrite;
FilmStatus film_write_begin(FilmWrite *write, const char *path);
FilmStatus film_write_end(FilmWrite *write, FilmStatus status);

#endif
