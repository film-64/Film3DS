#ifndef FILM_RAW_DARKROOM_H
#define FILM_RAW_DARKROOM_H
#include "film.h"
typedef enum { FILM_RGGB, FILM_BGGR, FILM_GRBG, FILM_GBRG } FilmCfa;
typedef struct { unsigned x,y; } FilmRawBadPixel;
typedef struct { unsigned width,height; const uint16_t *samples; } FilmRaw10View;
typedef struct FilmRawIsp {
    FilmCfa cfa;
    float black[4],white,wb[3],ccm[9],exposure_ev,shoulder;
    /* Linear output-space flare offset and opt-in local exposure. */
    float ccm_offset[3],local_strength,local_limit_ev;
    /* log2 gains: 1,x,y,x*x,x*y,y*y; coordinates normalized to [-1,1]. */
    float lens_shading[3][6];
    const FilmRawBadPixel *bad_pixels;
    unsigned bad_pixel_count;
} FilmRawIsp;
/* Prepared RGB is float, with the sRGB transfer applied. Primaries are correct
 * only when the caller supplies an appropriate sensor-to-working-space CCM. */
typedef struct { unsigned width,height; FilmRgb *pixels; } FilmRawPrepared;
typedef struct { unsigned width,height; FilmRgb *pixels; } FilmRawScene;
/* Explicit neutral patch in the ungraded, calibrated scene; coordinates are
 * full-frame pixels. Bounds are linear working RGB, not sensor clipping flags. */
typedef struct {
    unsigned x,y,width,height;
    float minimum,maximum;
} FilmNeutralPatch;
typedef struct {
    FilmWhiteBalance balance;
    unsigned accepted,rejected;
    float residual_stops,spread_stops;
} FilmNeutralBalance;
/* Replaces recipe.white_balance (never adds to it). Fits existing controls:
 * kelvin=0 or 1000..10000 in 100 K steps, red/blue=-9..9. Failure leaves out
 * unchanged. Requires >=9 valid pixels, >=75% coverage, chromatic spread <=.15
 * stop and fitted channel residual <=.08 stop. Inputs remain immutable. */
FilmStatus film_raw_scene_neutral(const FilmRawScene *,const FilmNeutralPatch *,FilmNeutralBalance *);
FilmRawIsp film_raw_isp_default(void);
FilmStatus film_raw_profile_load(const char *,FILE *,FilmRawIsp *);
FilmStatus film_raw_scene_prepare(const char *,const FilmRaw10View *,const FilmRawIsp *,FilmRawScene *);
void film_raw_scene_free(FilmRawScene *);
/* Reversible half-resolution linear RGB view for interactive darkroom only.
 * Never modifies the cached source scene or replaces final export pixels. */
FilmStatus film_raw_scene_preview_half(const FilmRawScene *,FilmRawScene *);
FilmStatus film_raw_frame_scene(const FilmFrame *,const FilmDevelopOptions *,FilmRawScene *);
FilmStatus film_raw_scene_grade(const char *,const FilmRawScene *,const FilmRecipe *,const FilmLut *,const FilmDevelopOptions *,FilmDevelopCanvas *);
FilmStatus film_raw_scene_probe(const char *,const char *,const char *,const char *,const FilmRecipe *,const FilmDevelopOptions *);
FilmStatus film_raw10_unpack(const char *source,const uint8_t *wire,size_t bytes,
    unsigned width,unsigned height,uint16_t *samples,size_t capacity);
FilmStatus film_raw_bad_pixel_clean(const char *,uint16_t *,unsigned,unsigned);
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
