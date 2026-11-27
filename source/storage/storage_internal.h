#ifndef FILM_STORAGE_INTERNAL_H
#define FILM_STORAGE_INTERNAL_H

#include "roll.h"

typedef struct { FILE *file; char *temporary; const char *destination; } FilmWrite;
FilmStatus film_write_begin(FilmWrite *write, const char *path);
FilmStatus film_write_end(FilmWrite *write, FilmStatus status);
uint32_t film_roll_source_crc(const FilmImage *);
FilmStatus film_roll_pair_publish_with_crc(const char *,const FilmFrame *,const FilmRawScene *,
    const char *,unsigned,uint32_t);

#endif
