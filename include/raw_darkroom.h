#ifndef FILM_RAW_DARKROOM_H
#define FILM_RAW_DARKROOM_H
#include "film.h"
typedef enum { FILM_RGGB, FILM_BGGR, FILM_GRBG, FILM_GBRG } FilmCfa;
typedef struct { unsigned x,y; } FilmRawBadPixel;
typedef struct { unsigned width,height; const uint16_t *samples; } FilmRaw10View;
typedef struct {
    FilmCfa cfa;
    float black[4],white,wb[3],ccm[9],exposure_ev,shoulder;
    const FilmRawBadPixel *bad_pixels;
    unsigned bad_pixel_count;
} FilmRawIsp;
/* Prepared RGB is float, with the sRGB transfer applied. Primaries are correct
 * only when the caller supplies an appropriate sensor-to-working-space CCM. */
typedef struct { unsigned width,height; FilmRgb *pixels; } FilmRawPrepared;
FilmRawIsp film_raw_isp_default(void);
FilmStatus film_raw10_unpack(const char *source,const uint8_t *wire,size_t bytes,
    unsigned width,unsigned height,uint16_t *samples,size_t capacity);
/* Output structs must be zero-initialized and own no live allocation. Inputs
 * remain caller-owned and immutable; release successful outputs with the
 * matching free function before reusing them. Outputs must not alias inputs. */
FilmStatus film_raw_isp_prepare(const char *source,const FilmRaw10View *raw,
    const FilmRawIsp *params,FilmRawPrepared *out);
void film_raw_prepared_free(FilmRawPrepared *prepared);
FilmStatus film_raw_import(const char *source,const char *input,const char *output);
FilmStatus film_raw_frame_prepare(const FilmFrame *,const FilmDevelopOptions *,FilmRawPrepared *);
FilmStatus film_raw_prepared_halation(FilmRawPrepared *);
FilmStatus film_raw_export_pair(const FilmFrame *,const FilmLut *,const FilmDevelopOptions *,const char *,const char *);
FilmStatus film_raw_grade_canvas(const char *source,const FilmRawPrepared *prepared,
    const FilmRecipe *recipe,const FilmLut *lut,FilmDevelopCanvas *out);
#endif
