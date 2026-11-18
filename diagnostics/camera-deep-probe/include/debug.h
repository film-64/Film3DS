#ifndef FILM_DEBUG_H
#define FILM_DEBUG_H
#include "film.h"
#include "ui.h"

#define FILM_DEBUG_LINE 768
typedef struct {
    FilmStatus (*init)(void *context, FilmPixelFormat format);
    FilmStatus (*capture)(void *context, FilmFrame *frame);
    uint64_t (*microseconds)(void *context);
    uint32_t (*device_result)(void *context);
} FilmDebugOps;

typedef struct {
    FilmDebugOps ops;
    void *context;
    const char *jpeg_path;
    const char *source_path;
    const FilmLut *lut;
    FilmFrame frame;
    FilmImage developed;
    FilmUi ui;
    bool ready;
    FilmStatus last_error;
    uint64_t elapsed_us;
} FilmDebug;

void film_debug_execute(FilmDebug *debug, const char *line, char reply[FILM_DEBUG_LINE]);
void film_debug_serve(FilmDebug *debug, FILE *input, FILE *output);
void film_debug_clear(FilmDebug *debug);
void film_log(FILE *log, const char *operation, FilmStatus status, uint32_t device_result);
#endif
