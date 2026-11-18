#ifndef FILM_EXPOSURE_MATRIX_H
#define FILM_EXPOSURE_MATRIX_H
#include "raw_darkroom.h"

/* Display-referred approximation, NOT shutter timing, ISO or calibrated EV.
 * Neutral is an exact bypass of RAW scene EV, three anchors and shoulder.
 * Shot is derived from the last native YUV preview, never from an uncalibrated
 * assumption that CAMU_SetExposure(+1) means +1 EV. */
typedef struct {
    float ev, shadows, midtones, highlights, shoulder;
} FilmExposureCurve;

typedef struct {
    FilmExposureCurve neutral, shot;
    float preview_linear[3]; /* low/median/high linear Y, sampled pre-shutter */
    unsigned id;
    uint32_t source_crc;     /* binds this sidecar to exact original pixels */
    int native_level;        /* original CAMU compensation, not EV */
    unsigned exposure_mode;  /* 0 native AE, 1 manual */
    bool preview_valid, fitted;
} FilmExposureMatrix;

FilmExposureMatrix film_exposure_matrix_default(void);
bool film_exposure_curve_valid(const FilmExposureCurve *curve);
FilmStatus film_exposure_matrix_fit(FilmExposureMatrix *matrix, const FilmRawScene *scene);
FilmStatus film_exposure_curve_apply(const FilmExposureCurve *, FilmRecipe *, FilmDevelopOptions *);

/* Atomic F3EM/v1 derived sidecar. DNG/RAW bytes are never modified.
 * A missing sidecar returns FILM_IO; invalid/mismatched data returns CORRUPT.
 * The caller must verify source_crc from the selected original/pair index. */
FilmStatus film_exposure_matrix_write(const char *directory,const FilmExposureMatrix *matrix);
FilmStatus film_exposure_matrix_read(const char *directory,unsigned id,uint32_t source_crc,FilmExposureMatrix *out);
#endif
