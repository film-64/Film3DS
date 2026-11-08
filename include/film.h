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

typedef enum { FILM_YUYV = 1, FILM_RGB565_LE, FILM_RGB24 } FilmPixelFormat;
typedef enum { FILM_601_FULL, FILM_601_LIMITED, FILM_709_FULL, FILM_709_LIMITED } FilmYuv;
typedef enum { FILM_4_3, FILM_3_2, FILM_1_1, FILM_16_9 } FilmAspect;
typedef enum { FILM_NATIVE, FILM_150, FILM_200 } FilmScale;
typedef enum { FILM_BILINEAR, FILM_BICUBIC } FilmFilter;
typedef enum { FILM_SRGB, FILM_LINEAR_SRGB, FILM_VENDOR_LOG, FILM_REC709 } FilmTransfer;

typedef struct { float r, g, b; } FilmRgb;
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
} FilmLut;

typedef struct {
    unsigned width, height;
    FilmPixelFormat format;
    uint8_t *pixels;
} FilmImage;

typedef struct { unsigned x, y, width, height; } FilmRect;

typedef struct {
    FilmAspect aspect;
    FilmScale scale;
    FilmFilter filter;
    FilmYuv yuv;
    uint32_t grain_seed;
    float grain_amount;
    unsigned jpeg_quality;
    FilmProfile profile;
    char lut_id[64];
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
FilmRgb film_grade(FilmRgb color, const FilmLut *lut, const FilmProfile *profile);
FilmStatus film_profile_write(const char *path, const FilmProfile *profile);
FilmStatus film_profile_read(const char *path, FilmProfile *profile);

size_t film_image_bytes(unsigned width, unsigned height, FilmPixelFormat format);
FilmStatus film_image_alloc(FilmImage *out, unsigned width, unsigned height, FilmPixelFormat format);
void film_image_free(FilmImage *image);
FilmRgb film_decode_pixel(const FilmImage *image, unsigned x, unsigned y, FilmYuv yuv);
FilmStatus film_rgb24(const FilmImage *source, FilmYuv yuv, FilmImage *out);
FilmStatus film_crop_rect(unsigned width, unsigned height, FilmAspect aspect, FilmRect *out);
FilmStatus film_scaled_dims(unsigned width, unsigned height, FilmScale scale, unsigned *w, unsigned *h);
FilmStatus film_resize(const FilmImage *source, unsigned width, unsigned height, FilmFilter filter, FilmImage *out);
FilmStatus film_recipe_validate(const FilmRecipe *recipe);
FilmRecipe film_default_recipe(void);
FilmStatus film_develop(const FilmFrame *frame, const FilmLut *lut, FilmImage *out);

FilmStatus film_frame_write(const char *path, const FilmFrame *frame);
FilmStatus film_frame_read(const char *path, FilmFrame *frame);
FilmStatus film_jpeg_write(const char *path, const FilmImage *image, unsigned quality);
void film_frame_free(FilmFrame *frame);

FilmStatus film_mock_capture(FilmFrame *frame);
#endif
