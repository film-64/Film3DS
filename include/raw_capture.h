#ifndef FILM_RAW_CAPTURE_H
#define FILM_RAW_CAPTURE_H
#include "film.h"
typedef struct {
    void *context;
    FilmStatus (*read)(void *,bool,uint16_t,uint16_t *);
    FilmStatus (*write)(void *,bool,uint16_t,uint16_t);
    FilmStatus (*lock)(void *);
    FilmStatus (*frame)(void *,bool,bool);
    FilmStatus (*idle)(void *);
    FilmStatus (*restore_auto)(void *);
    void (*trace)(void *,const char *,uint16_t,uint16_t,uint16_t,FilmStatus);
    FilmStatus (*native_frame)(void *);
} FilmRawCaptureIo;
bool film_raw_qa_token_valid(const char *);
FilmStatus film_raw_capture_transaction(const char *,FilmRawCaptureIo);
#endif
