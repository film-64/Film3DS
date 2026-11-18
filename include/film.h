#ifndef FILM_H
#define FILM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    FILM_OK, FILM_INVALID, FILM_UNSUPPORTED, FILM_IO, FILM_NOMEM,
    FILM_CORRUPT, FILM_STATE, FILM_DEVICE
} FilmStatus;

typedef enum { FILM_YUYV = 1, FILM_RGB565_LE, FILM_RGB24, FILM_RAW10_RGGB } FilmPixelFormat;
typedef enum { FILM_601_FULL, FILM_601_LIMITED, FILM_709_FULL, FILM_709_LIMITED } FilmYuv;
typedef enum { FILM_4_3, FILM_3_2, FILM_1_1, FILM_16_9 } FilmAspect;
typedef enum { FILM_NATIVE, FILM_150, FILM_200 } FilmScale;
typedef enum { FILM_BILINEAR, FILM_BICUBIC } FilmFilter;
typedef enum { FILM_GRAIN_FINE, FILM_GRAIN_MEDIUM, FILM_GRAIN_COARSE, FILM_GRAIN_COUNT } FilmGrain;
typedef enum { FILM_SRGB, FILM_LINEAR_SRGB, FILM_VENDOR_LOG, FILM_REC709, FILM_FLOG2C } FilmTransfer;

typedef struct { float r, g, b; } FilmRgb;
typedef struct {
    float y_gain, y_offset, r_cr, g_cb, g_cr, b_cb;
} FilmYuvTransform;
typedef struct { float hue, saturation, lightness; } FilmHsl;
typedef struct {
    FilmTransfer input, output;
    float strength, exposure;
    FilmHsl global, zones[12];
} FilmProfile;

typedef struct {
    unsigned size;
    FilmRgb min, max;
    FilmRgb *samples;
    bool trilinear;
    bool shadow_shaper;
} FilmLut;

typedef struct {
    const FilmLut *lut;
    const FilmProfile *profile;
    float exposure_gain;
    bool has_hsl;
} FilmGrade;

typedef enum {
    FILM_TONE_EV, FILM_TONE_BLACK, FILM_TONE_MID, FILM_TONE_HIGH,
    FILM_TONE_SHADOWS, FILM_TONE_SHOULDER, FILM_TONE_SATURATION, FILM_TONE_COUNT
} FilmToneControl;

typedef struct { float value[FILM_TONE_COUNT]; } FilmTone;
typedef struct { unsigned kelvin; int red, blue; } FilmWhiteBalance;

typedef struct {
    FilmGrade grade;
    FilmTone tone;
    FilmTone input_tone;
    float output_curve[4][4];
    float post_gain;
    float input_gain;
    bool active, input_active, output_active;
    float white_balance[3][3];
    bool white_balance_active;
} FilmDevelopGrade;

#define FILM_WARM_LUT_ID "builtin-warm"

typedef struct {
    unsigned width, height;
    FilmPixelFormat format;
    uint8_t *pixels;
} FilmImage;

typedef struct { unsigned x, y, width, height; } FilmRect;
typedef struct { unsigned width, height; uint16_t *pixels; } FilmDevelopCanvas;
typedef struct { float rows[3][3]; } FilmCameraColor;
typedef struct {
    float code[9], rms[9], correlation[9];
} FilmGrainProfile;
struct FilmRawIsp;
struct FilmLayeredGrain;
struct FilmGrainTexture;
struct FilmRecipe;
struct FilmDevelopOptions;
typedef FilmStatus (*FilmCanvasRenderer)(const char *,const FilmDevelopCanvas *,
    const struct FilmRecipe *,const struct FilmDevelopOptions *,FilmImage *);
typedef FilmStatus (*FilmLayeredGrainRenderer)(const char *,FilmImage *,
    const struct FilmLayeredGrain *,const FilmGrainProfile *,const struct FilmGrainTexture *);
typedef struct FilmDevelopOptions {
    bool adaptive_grain;
    FilmGrain grain;
    const FilmCameraColor *camera_color;
    const FilmGrainProfile *grain_profile;
    bool halation;
    bool layered_grain;
    float grain_size,grain_shadow,grain_mid,grain_highlight;
    const struct FilmGrainTexture *grain_texture;
    FilmLayeredGrainRenderer grain_renderer;
    FilmCanvasRenderer output_renderer;
    float chroma_denoise;
    const struct FilmRawIsp *raw_isp; /* explicit image calibration; borrowed */
    float raw_shadows,raw_midtones,raw_highlights; /* scene EV offsets, -2.5..2.5 */
    float raw_shoulder,raw_local,raw_local_limit; /* late knee (0=off), strength, EV bound */
} FilmDevelopOptions;
FilmStatus film_darkroom_denoise(const char *,FilmRgb *,unsigned,unsigned,float);

typedef struct FilmRecipe {
    FilmAspect aspect;
    FilmScale scale;
    FilmFilter filter;
    FilmYuv yuv;
    uint32_t grain_seed;
    float grain_amount;
    float base_saturation;
    unsigned jpeg_quality;
    FilmProfile profile;
    char lut_id[64];
    FilmTone tone;
    /* v4: independent user adjustments; old tone remains a post-LUT legacy trim. */
    FilmTone input_tone;
    int sharpness;
    /* v5: final user H/M/S, independent of the legacy v4 input curves. */
    FilmTone output_tone;
    /* v6: 0 K is original; 0 percent is the legacy/default 100% frame. */
    FilmWhiteBalance white_balance;
    unsigned crop_percent, crop_x, crop_y;
    bool crop_positioned;
} FilmRecipe;

typedef struct {
    FilmImage source;
    FilmRecipe recipe;
    uint64_t captured_at;
    uint32_t exposure_us, iso;
    uint32_t camera;
} FilmFrame;

const char *film_status_name(FilmStatus status);
FilmStatus film_lut_load(FILE *file, FilmLut *out, unsigned *error_line);
void film_lut_free(FilmLut *lut);
FilmRgb film_lut_sample(const FilmLut *lut, FilmRgb color);
FilmStatus film_profile_validate(const FilmProfile *profile);
FilmGrade film_grade_prepare(const FilmLut *lut, const FilmProfile *profile);
FilmRgb film_grade_pixel(FilmRgb color, const FilmGrade *grade);
FilmRgb film_grade(FilmRgb color, const FilmLut *lut, const FilmProfile *profile);
FilmRgb film_fit_chroma(FilmRgb color, float luma, float gain);
FilmDevelopGrade film_develop_grade_prepare(const FilmLut *lut, const FilmRecipe *recipe);
FilmRgb film_develop_grade_pixel(FilmRgb color, const FilmDevelopGrade *grade);
float film_tone_limit(FilmToneControl control, float value);
FilmStatus film_camera_color_load(FILE *file, FilmCameraColor *out);
FilmRgb film_camera_color_apply(FilmRgb code, const FilmCameraColor *color);
float film_srgb_linear_luma(FilmRgb code);
FilmRgb film_srgb_add_halation(FilmRgb code, float energy);
FilmStatus film_white_balance_matrix(FilmWhiteBalance balance, float out[3][3]);
FilmRgb film_white_balance_apply(FilmRgb code, const float matrix[3][3]);
FilmRgb film_white_balance_swatch(int red, int blue);
FilmStatus film_blackbody_white(unsigned kelvin, float xyz[3]);
FilmStatus film_profile_write(const char *path, const FilmProfile *profile);
FilmStatus film_profile_read(const char *path, FilmProfile *profile);

size_t film_image_bytes(unsigned width, unsigned height, FilmPixelFormat format);
FilmStatus film_image_alloc(FilmImage *out, unsigned width, unsigned height, FilmPixelFormat format);
void film_image_free(FilmImage *image);
FilmRgb film_decode_pixel(const FilmImage *image, unsigned x, unsigned y, FilmYuv yuv);
FilmYuvTransform film_yuv_transform(FilmYuv matrix, float saturation);
FilmStatus film_rgb24(const FilmImage *source, FilmYuv yuv, FilmImage *out);
FilmStatus film_crop_rect(unsigned width, unsigned height, FilmAspect aspect, FilmRect *out);
FilmStatus film_scaled_dims(unsigned width, unsigned height, FilmScale scale, unsigned *w, unsigned *h);
FilmStatus film_resize(const FilmImage *source, unsigned width, unsigned height, FilmFilter filter, FilmImage *out);
FilmStatus film_sharpen(FilmImage *image, int level);
FilmStatus film_composition_rect(unsigned width, unsigned height, const FilmRecipe *recipe, FilmRect *out);
FilmStatus film_composition_percent(FilmRecipe *recipe, unsigned width, unsigned height, int direction);
FilmStatus film_composition_pan(FilmRecipe *recipe, unsigned width, unsigned height, int dx, int dy);
void film_composition_reset(FilmRecipe *recipe);
FilmStatus film_recipe_validate(const FilmRecipe *recipe);
FilmRecipe film_default_recipe(void);
FilmStatus film_develop(const FilmFrame *frame, const FilmLut *lut, FilmImage *out);
FilmStatus film_develop_with_options(const FilmFrame *frame, const FilmLut *lut,
                                     const FilmDevelopOptions *options, FilmImage *out);
FilmStatus film_develop_canvas(const FilmFrame *frame, const FilmLut *lut,
                               const FilmDevelopOptions *options, FilmDevelopCanvas *out);
void film_develop_canvas_free(FilmDevelopCanvas *canvas);
FilmStatus film_develop_canvas_render(const FilmDevelopCanvas *canvas, const FilmRecipe *recipe,
                                      const FilmDevelopOptions *options, bool full_preview, FilmImage *out);
FilmStatus film_develop_canvas_render_pair(const FilmDevelopCanvas *,const FilmRecipe *,
    const FilmDevelopOptions *,FilmImage *,FilmImage *);
void film_grain_adaptive(FilmImage *image, uint32_t seed, float amount, FilmScale scale, FilmGrain grain);
FilmStatus film_grain_profile_load(FILE *file, FilmGrainProfile *out);
FilmStatus film_grain_measured(FilmImage *image, uint32_t seed, float amount, const FilmGrainProfile *profile);

FilmStatus film_dng_write(const char *source,const char *path,const FilmFrame *frame);
FilmStatus film_dng_read(const char *source,FILE *file,FilmFrame *frame);
FilmStatus film_frame_write(const char *path, const FilmFrame *frame);
FilmStatus film_frame_read(const char *path, FilmFrame *frame);
FilmStatus film_jpeg_write(const char *path, const FilmImage *image, unsigned quality);
void film_frame_free(FilmFrame *frame);

FilmStatus film_mock_capture(FilmFrame *frame);
#endif
