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
} FilmRawCaptureIo;
/* raw_received means the retain callback copied a stopped, checked DMA frame
 * into independent owned storage. raw_valid also requires the post-frame hold.
 * A restoration error never makes an unsafe DMA buffer publishable. */
typedef struct {
    bool raw_received, raw_valid, camera_restored;
    FilmStatus capture_status, restore_status;
} FilmRawCaptureResult;
FilmStatus film_raw_capture_transaction_result(const char *,FilmRawCaptureIo,FilmRawCaptureResult *);
bool film_raw_qa_token_valid(const char *);
FilmStatus film_raw_capture_transaction(const char *,FilmRawCaptureIo);
#endif
