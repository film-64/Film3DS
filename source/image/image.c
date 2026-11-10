#include "film.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FILM_MAX_SIDE 4096u

const char *film_status_name(FilmStatus status)
{
    static const char *const names[] = {"ok", "invalid", "unsupported", "io", "no_memory",
                                       "corrupt", "bad_state", "device"};
    return (unsigned)status < sizeof names / sizeof *names ? names[status] : "unknown";
}

size_t film_image_bytes(unsigned w, unsigned h, FilmPixelFormat format)
{
    if (!w || !h || w > FILM_MAX_SIDE || h > FILM_MAX_SIDE) return 0;
    if (format == FILM_YUYV && w % 2) return 0;
    switch (format) {
    case FILM_YUYV:
    case FILM_RGB565_LE: return (size_t)w * h * 2;
    case FILM_RGB24: return (size_t)w * h * 3;
    default: return 0;
    }
}

FilmStatus film_image_alloc(FilmImage *out, unsigned w, unsigned h, FilmPixelFormat format)
{
    size_t bytes = film_image_bytes(w, h, format);
    if (!out || !bytes) return FILM_INVALID;
    uint8_t *pixels = malloc(bytes);
    if (!pixels) return FILM_NOMEM;
    *out = (FilmImage){w, h, format, pixels};
    return FILM_OK;
}

void film_image_free(FilmImage *image)
{
    free(image->pixels);
    *image = (FilmImage){0};
}

static float unit(float x) { return fminf(fmaxf(x, 0), 1); }

FilmYuvTransform film_yuv_transform(FilmYuv matrix, float saturation)
{
    bool limited = matrix == FILM_601_LIMITED || matrix == FILM_709_LIMITED;
    bool hd = matrix == FILM_709_FULL || matrix == FILM_709_LIMITED;
    float chroma = (1 + saturation) / (limited ? 224.0f : 255.0f);
    return (FilmYuvTransform){1 / (limited ? 219.0f : 255.0f), limited ? 16 : 0,
        (hd ? 1.5748f : 1.402f) * chroma, -(hd ? 0.187324f : 0.344136f) * chroma,
        -(hd ? 0.468124f : 0.714136f) * chroma, (hd ? 1.8556f : 1.772f) * chroma};
}

static FilmRgb decode_pixel(const FilmImage *image, unsigned x, unsigned y, const FilmYuvTransform *t)
{
    size_t index = (size_t)y * image->width + x;
    const uint8_t *p = image->pixels;
    if (image->format == FILM_RGB24)
        return (FilmRgb){p[index * 3] / 255.0f, p[index * 3 + 1] / 255.0f, p[index * 3 + 2] / 255.0f};
    if (image->format == FILM_RGB565_LE) {
        unsigned word = p[index * 2] | (unsigned)p[index * 2 + 1] << 8;
        return (FilmRgb){(word >> 11) / 31.0f, ((word >> 5) & 63) / 63.0f, (word & 31) / 31.0f};
    }
    p += (index & ~(size_t)1) * 2;
    float luma = (p[(x % 2) * 2] - t->y_offset) * t->y_gain;
    int cb = p[1] - 128, cr = p[3] - 128;
    return (FilmRgb){luma + t->r_cr * cr, luma + t->g_cb * cb + t->g_cr * cr,
                     luma + t->b_cb * cb};
}

FilmRgb film_decode_pixel(const FilmImage *image, unsigned x, unsigned y, FilmYuv matrix)
{
    FilmYuvTransform t = film_yuv_transform(matrix, 0);
    FilmRgb rgb = decode_pixel(image, x, y, &t);
    return (FilmRgb){unit(rgb.r), unit(rgb.g), unit(rgb.b)};
}

static uint8_t quantize(float value)
{
    return (uint8_t)(unit(value) * 255 + 0.5f);
}

static bool valid_image(const FilmImage *image)
{
    return image && image->pixels && film_image_bytes(image->width, image->height, image->format);
}

FilmStatus film_rgb24(const FilmImage *source, FilmYuv yuv, FilmImage *out)
{
    if (!valid_image(source) || !out || out == source || (unsigned)yuv > FILM_709_LIMITED)
        return FILM_INVALID;
    FilmStatus status = film_image_alloc(out, source->width, source->height, FILM_RGB24);
    if (status != FILM_OK) return status;
    FilmYuvTransform t = film_yuv_transform(yuv, 0);
    for (unsigned y = 0; y < source->height; ++y) {
        uint8_t *pixel = out->pixels + (size_t)y * out->width * 3;
        for (unsigned x = 0; x < source->width; ++x, pixel += 3) {
            FilmRgb c = decode_pixel(source, x, y, &t);
            pixel[0] = quantize(c.r);
            pixel[1] = quantize(c.g);
            pixel[2] = quantize(c.b);
        }
    }
    return FILM_OK;
}

FilmStatus film_crop_rect(unsigned w, unsigned h, FilmAspect aspect, FilmRect *out)
{
    static const unsigned ratios[][2] = {{4, 3}, {3, 2}, {1, 1}, {16, 9}};
    if (!out || (unsigned)aspect > FILM_16_9) return FILM_INVALID;
    unsigned a = ratios[aspect][0], b = ratios[aspect][1];
    unsigned multiple = w / a < h / b ? w / a : h / b;
    if (!multiple) return FILM_INVALID;
    unsigned width = multiple * a, height = multiple * b;
    *out = (FilmRect){(w - width) / 2, (h - height) / 2, width, height};
    return FILM_OK;
}

FilmStatus film_scaled_dims(unsigned w, unsigned h, FilmScale scale, unsigned *ow, unsigned *oh)
{
    static const unsigned numerators[] = {2, 3, 4};
    if (!ow || !oh || (unsigned)scale > FILM_200 || !film_image_bytes(w, h, FILM_RGB24))
        return FILM_INVALID;
    unsigned width = w * numerators[scale] / 2, height = h * numerators[scale] / 2;
    if (!film_image_bytes(width, height, FILM_RGB24)) return FILM_INVALID;
    *ow = width;
    *oh = height;
    return FILM_OK;
}

typedef struct {
    unsigned position[4];
    float weight[4];
} Kernel;

static float cubic(float x)
{
    x = fabsf(x);
    if (x <= 1) return ((1.5f * x - 2.5f) * x) * x + 1;
    if (x < 2) return ((-0.5f * x + 2.5f) * x - 4) * x + 2;
    return 0;
}

static void make_kernels(Kernel *out, unsigned from, unsigned to, unsigned taps)
{
    for (unsigned i = 0; i < to; ++i) {
        float location = (i + 0.5f) * ((float)from / to) - 0.5f;
        int start = (int)floorf(location) - (taps == 4);
        for (unsigned t = 0; t < taps; ++t) {
            int at = start + (int)t;
            out[i].position[t] = at < 0 ? 0 : at >= (int)from ? from - 1 : (unsigned)at;
            float delta = location - at;
            out[i].weight[t] = taps == 4 ? cubic(delta) : 1 - fabsf(delta);
        }
    }
}

enum { HALATION_CELL = 4 };

static float *make_halation(const FilmImage *source, const FilmYuvTransform *transform,
                            const FilmCameraColor *color, unsigned width, unsigned height)
{
    size_t count = (size_t)width * height;
    float *mask = malloc(count * sizeof *mask * 2);
    if (!mask) return NULL;
    float *scratch = mask + count;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            unsigned x0 = x * HALATION_CELL, y0 = y * HALATION_CELL;
            float energy = 0;
            unsigned pixels = 0;
            for (unsigned dy = 0; dy < HALATION_CELL && y0 + dy < source->height; ++dy)
                for (unsigned dx = 0; dx < HALATION_CELL && x0 + dx < source->width; ++dx) {
                    FilmRgb rgb = decode_pixel(source, x0 + dx, y0 + dy, transform);
                    rgb = film_camera_color_apply(rgb, color);
                    energy += fmaxf(film_srgb_linear_luma(rgb) - .65f, 0);
                    ++pixels;
                }
            mask[(size_t)y * width + x] = energy / pixels;
        }
    static const float weight[] = {.0625f, .25f, .375f, .25f, .0625f};
    for (unsigned axis = 0; axis < 2; ++axis) {
        const float *input = axis ? scratch : mask;
        float *output = axis ? mask : scratch;
        unsigned length = axis ? height : width;
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                float value = 0;
                for (int i = -2; i <= 2; ++i) {
                    int at = (int)(axis ? y : x) + i;
                    unsigned clamped = at < 0 ? 0 : (unsigned)at >= length ? length - 1 : (unsigned)at;
                    size_t index = axis ? (size_t)clamped * width + x : (size_t)y * width + clamped;
                    value += weight[i + 2] * input[index];
                }
                output[(size_t)y * width + x] = value;
            }
    }
    return mask;
}

static float halation_energy(const float *mask, unsigned width, unsigned height, unsigned x, unsigned y)
{
    float sx = fmaxf((x + .5f) / HALATION_CELL - .5f, 0);
    float sy = fmaxf((y + .5f) / HALATION_CELL - .5f, 0);
    unsigned left = (unsigned)sx, top = (unsigned)sy;
    unsigned right = left + (left + 1 < width), bottom = top + (top + 1 < height);
    float fx = sx - left, fy = sy - top;
    float a = mask[(size_t)top * width + left] * (1 - fx) + mask[(size_t)top * width + right] * fx;
    float b = mask[(size_t)bottom * width + left] * (1 - fx) + mask[(size_t)bottom * width + right] * fx;
    return .18f * (a * (1 - fy) + b * fy);
}

static FilmStatus resize_rgb(const void *source, unsigned sw, unsigned sh, unsigned w, unsigned h,
                             FilmFilter filter, bool high_precision, FilmImage *out)
{
    FilmImage resized = {0};
    FilmStatus status = film_image_alloc(&resized, w, h, FILM_RGB24);
    if (status != FILM_OK) return status;
    unsigned taps = filter == FILM_BICUBIC ? 4 : 2;
    Kernel *kernel = malloc(((size_t)w + h) * sizeof *kernel);
    if (!kernel) { film_image_free(&resized); return FILM_NOMEM; }
    Kernel *kx = kernel, *ky = kernel + w;
    make_kernels(kx, sw, w, taps);
    make_kernels(ky, sh, h, taps);
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            float rgb[3] = {0};
            for (unsigned j = 0; j < taps; ++j) {
                size_t row = (size_t)ky[y].position[j] * sw * 3;
                for (unsigned i = 0; i < taps; ++i) {
                    size_t index = row + kx[x].position[i] * 3;
                    float weight = ky[y].weight[j] * kx[x].weight[i];
                    if (high_precision) {
                        const uint16_t *pixel = (const uint16_t *)source + index;
                        for (unsigned c = 0; c < 3; ++c) rgb[c] += pixel[c] * weight;
                    } else {
                        const uint8_t *pixel = (const uint8_t *)source + index;
                        for (unsigned c = 0; c < 3; ++c) rgb[c] += pixel[c] * weight;
                    }
                }
            }
            uint8_t *dest = resized.pixels + ((size_t)y * w + x) * 3;
            for (unsigned c = 0; c < 3; ++c)
                dest[c] = high_precision ? quantize(rgb[c] * (1.0f / 65535)) :
                          (uint8_t)(fminf(fmaxf(rgb[c], 0), 255) + 0.5f);
        }
    }
    free(kernel);
    *out = resized;
    return FILM_OK;
}

FilmStatus film_resize(const FilmImage *src, unsigned w, unsigned h, FilmFilter filter, FilmImage *out)
{
    if (!valid_image(src) || src->format != FILM_RGB24 || !out || out == src ||
        (unsigned)filter > FILM_BICUBIC) return FILM_INVALID;
    if (w != src->width || h != src->height)
        return resize_rgb(src->pixels, src->width, src->height, w, h, filter, false, out);
    FilmStatus status = film_image_alloc(out, w, h, FILM_RGB24);
    if (status == FILM_OK) memcpy(out->pixels, src->pixels, (size_t)w * h * 3);
    return status;
}

FilmRecipe film_default_recipe(void)
{
    return (FilmRecipe){.profile = {.strength = 1}, .jpeg_quality = 92, .grain_seed = 1};
}

FilmStatus film_recipe_validate(const FilmRecipe *p)
{
    if (!p || (unsigned)p->aspect > FILM_16_9 || (unsigned)p->scale > FILM_200 ||
        (unsigned)p->filter > FILM_BICUBIC || (unsigned)p->yuv > FILM_709_LIMITED ||
        !isfinite(p->grain_amount) || p->grain_amount < 0 || p->grain_amount > 1 ||
        !isfinite(p->base_saturation) || fabsf(p->base_saturation) > 1 ||
        p->jpeg_quality < 1 || p->jpeg_quality > 100 ||
        !memchr(p->lut_id, 0, sizeof p->lut_id)) return FILM_INVALID;
    for (unsigned i = 0; i < FILM_TONE_COUNT; ++i)
        if (!isfinite(p->tone.value[i]) || p->tone.value[i] != film_tone_limit(i, p->tone.value[i]))
            return FILM_INVALID;
    return film_profile_validate(&p->profile);
}

static void add_grain(FilmImage *image, uint32_t seed, float amount)
{
    if (!amount) return;
    for (size_t i = 0; i < (size_t)image->width * image->height * 3; i += 3) {
        seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
        int a = (int)(seed >> 24);
        seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
        int b = (int)(seed >> 24);
        float noise = (a + b - 255) * amount * 0.25f;
        for (unsigned c = 0; c < 3; ++c)
            image->pixels[i + c] = quantize((image->pixels[i + c] + noise) / 255);
    }
}

static FilmStatus warm_lut(FilmLut *lut)
{
    enum { SIZE = 17 };
    *lut = (FilmLut){.size = SIZE, .max = {1, 1, 1},
                     .samples = malloc(SIZE * SIZE * SIZE * sizeof(FilmRgb))};
    if (!lut->samples) return FILM_NOMEM;
    FilmRgb *sample = lut->samples;
    for (unsigned b = 0; b < SIZE; ++b)
        for (unsigned g = 0; g < SIZE; ++g)
            for (unsigned r = 0; r < SIZE; ++r) {
                float red = r / 16.0f, green = g / 16.0f, blue = b / 16.0f;
                *sample++ = (FilmRgb){red + 0.28f * red * (1 - red),
                                      green + 0.06f * green * (1 - green),
                                      blue - 0.26f * blue * (1 - blue)};
            }
    return FILM_OK;
}

FilmStatus film_develop_with_options(const FilmFrame *frame, const FilmLut *lut,
                                     const FilmDevelopOptions *options, FilmImage *out)
{
    if (!frame || !valid_image(&frame->source) || !out || out == &frame->source) return FILM_INVALID;
    const FilmRecipe *p = &frame->recipe;
    FilmStatus status = film_recipe_validate(p);
    if (status != FILM_OK) return status;
    if (options && options->adaptive_grain && (unsigned)options->grain >= FILM_GRAIN_COUNT) return FILM_INVALID;
    bool builtin = !lut && p->profile.strength && !strcmp(p->lut_id, FILM_WARM_LUT_ID);
    if (p->lut_id[0] && !lut && p->profile.strength && !builtin) return FILM_STATE;
    if (lut && (!lut->samples || (lut->size != 17 && lut->size != 33 && lut->size != 65)))
        return FILM_INVALID;
    unsigned w, h;
    FilmRect rect;
    const FilmImage *source = &frame->source;
    if (film_scaled_dims(source->width, source->height, p->scale, &w, &h) != FILM_OK ||
        film_crop_rect(w, h, p->aspect, &rect) != FILM_OK) return FILM_INVALID;
    FilmImage graded = {0};
    bool resized = w != source->width || h != source->height;
    uint16_t *high_precision = NULL;
    if (options && resized) {
        high_precision = malloc(film_image_bytes(source->width, source->height, FILM_RGB24) *
                                sizeof *high_precision);
        if (!high_precision) return FILM_NOMEM;
    } else {
        status = film_image_alloc(&graded, source->width, source->height, FILM_RGB24);
        if (status != FILM_OK) return status;
    }
    FilmLut generated = {0};
    if (builtin) {
        status = warm_lut(&generated);
        if (status != FILM_OK) { free(high_precision); film_image_free(&graded); return status; }
        lut = &generated;
    }
    FilmYuvTransform t = film_yuv_transform(p->yuv, 0);
    FilmRecipe grading = *p;
    if (source->format != FILM_YUYV) grading.base_saturation = 0;
    FilmDevelopGrade grade = film_develop_grade_prepare(lut, &grading);
    const FilmCameraColor *camera_color = options ? options->camera_color : NULL;
    unsigned mask_w = (source->width + HALATION_CELL - 1) / HALATION_CELL;
    unsigned mask_h = (source->height + HALATION_CELL - 1) / HALATION_CELL;
    float *halo = options && options->halation ? make_halation(source, &t, camera_color, mask_w, mask_h) : NULL;
    if (options && options->halation && !halo) {
        film_lut_free(&generated);
        film_image_free(&graded);
        free(high_precision);
        return FILM_NOMEM;
    }
    for (unsigned y = 0; y < source->height; ++y) {
        size_t index = (size_t)y * source->width * 3;
        for (unsigned x = 0; x < source->width; ++x, index += 3) {
            FilmRgb c = decode_pixel(source, x, y, &t);
            if (camera_color) c = film_camera_color_apply(c, camera_color);
            if (halo) c = film_srgb_add_halation(c, halation_energy(halo, mask_w, mask_h, x, y));
            c = film_develop_grade_pixel(c, &grade);
            if (high_precision) {
                high_precision[index] = (uint16_t)(unit(c.r) * 65535 + .5f);
                high_precision[index + 1] = (uint16_t)(unit(c.g) * 65535 + .5f);
                high_precision[index + 2] = (uint16_t)(unit(c.b) * 65535 + .5f);
            } else {
                graded.pixels[index] = quantize(c.r);
                graded.pixels[index + 1] = quantize(c.g);
                graded.pixels[index + 2] = quantize(c.b);
            }
        }
    }
    free(halo);
    film_lut_free(&generated);
    FilmImage scaled = graded;
    if (high_precision) {
        status = resize_rgb(high_precision, source->width, source->height, w, h, p->filter, true, &scaled);
        free(high_precision);
        if (status != FILM_OK) return status;
    } else if (resized) {
        status = film_resize(&graded, w, h, p->filter, &scaled);
        film_image_free(&graded);
        if (status != FILM_OK) return status;
    }
    if (options && options->grain_profile) {
        status = film_grain_measured(&scaled, p->grain_seed, p->grain_amount, options->grain_profile);
        if (status != FILM_OK) { film_image_free(&scaled); return status; }
    } else if (options && options->adaptive_grain)
        film_grain_adaptive(&scaled, p->grain_seed, p->grain_amount, p->scale, options->grain);
    else add_grain(&scaled, p->grain_seed, p->grain_amount);
    for (unsigned y = 0; y < rect.height; ++y) {
        size_t from = ((size_t)(rect.y + y) * w + rect.x) * 3;
        memmove(scaled.pixels + (size_t)y * rect.width * 3, scaled.pixels + from, (size_t)rect.width * 3);
    }
    scaled.width = rect.width;
    scaled.height = rect.height;
    *out = scaled;
    return FILM_OK;
}

FilmStatus film_develop(const FilmFrame *frame, const FilmLut *lut, FilmImage *out)
{
    return film_develop_with_options(frame, lut, NULL, out);
}

void film_frame_free(FilmFrame *frame)
{
    film_image_free(&frame->source);
    *frame = (FilmFrame){0};
}
