#ifndef FILM_HARDWARE_EXPOSURE_H
#define FILM_HARDWARE_EXPOSURE_H
#include "film.h"
#include "film_runtime_contract.h"

/* Runtime state is local to the camera. Never serialize it into a recipe. */
typedef struct {
    void *context;
    FilmStatus (*read)(void *, bool, uint16_t, uint16_t *);
    FilmStatus (*write)(void *, bool, uint16_t, uint16_t);
    void (*delay)(void *, unsigned);
} FilmHardwareIo;

typedef struct {
    FilmHardwareIo io;
    uint16_t saved[12], line, fine, dgain, seq, awb;
    unsigned remaining, phase;
    int step;
    bool saved_valid, active, ready, failed;
} FilmHardwareExposure;

FilmStatus film_hardware_begin(FilmHardwareExposure *hw, FilmHardwareIo io, const char *source);
FilmStatus film_hardware_step(FilmHardwareExposure *hw, int step, const char *source);
FilmStatus film_hardware_frame(FilmHardwareExposure *hw);
FilmStatus film_hardware_verify(FilmHardwareExposure *hw);
FilmStatus film_hardware_restore(FilmHardwareExposure *hw);
#endif
