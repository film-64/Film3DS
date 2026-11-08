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

FilmRgb film_decode_pixel(const FilmImage *image, unsigned x, unsigned y, FilmYuv matrix)
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
    bool limited = matrix == FILM_601_LIMITED || matrix == FILM_709_LIMITED;
    bool hd = matrix == FILM_709_FULL || matrix == FILM_709_LIMITED;
    float luma = (p[(x % 2) * 2] - (limited ? 16 : 0)) / (limited ? 219.0f : 255.0f);
    float cb = (p[1] - 128) / (limited ? 224.0f : 255.0f);
    float cr = (p[3] - 128) / (limited ? 224.0f : 255.0f);
    return (FilmRgb){unit(luma + (hd ? 1.5748f : 1.402f) * cr),
                     unit(luma - (hd ? 0.187324f : 0.344136f) * cb -
                                 (hd ? 0.468124f : 0.714136f) * cr),
                     unit(luma + (hd ? 1.8556f : 1.772f) * cb)};
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
    for (unsigned y = 0; y < source->height; ++y) {
        uint8_t *pixel = out->pixels + (size_t)y * out->width * 3;
        for (unsigned x = 0; x < source->width; ++x, pixel += 3) {
            FilmRgb c = film_decode_pixel(source, x, y, yuv);
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

FilmStatus film_resize(const FilmImage *src, unsigned w, unsigned h, FilmFilter filter, FilmImage *out)
{
    if (!valid_image(src) || src->format != FILM_RGB24 || !out || out == src ||
        (unsigned)filter > FILM_BICUBIC) return FILM_INVALID;
    FilmImage resized = {0};
    FilmStatus status = film_image_alloc(&resized, w, h, FILM_RGB24);
    if (status != FILM_OK) return status;
    if (w == src->width && h == src->height) {
        memcpy(resized.pixels, src->pixels, (size_t)w * h * 3);
        *out = resized;
        return FILM_OK;
    }
    Kernel *kernel = malloc(((size_t)w + h) * sizeof *kernel);
    if (!kernel) { film_image_free(&resized); return FILM_NOMEM; }
    Kernel *kx = kernel, *ky = kernel + w;
    unsigned taps = filter == FILM_BICUBIC ? 4 : 2;
    make_kernels(kx, src->width, w, taps);
    make_kernels(ky, src->height, h, taps);
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            float rgb[3] = {0};
            for (unsigned j = 0; j < taps; ++j) {
                const uint8_t *row = src->pixels + (size_t)ky[y].position[j] * src->width * 3;
                for (unsigned i = 0; i < taps; ++i) {
                    const uint8_t *pixel = row + kx[x].position[i] * 3;
                    float weight = ky[y].weight[j] * kx[x].weight[i];
                    for (unsigned c = 0; c < 3; ++c) rgb[c] += pixel[c] * weight;
                }
            }
            uint8_t *dest = resized.pixels + ((size_t)y * w + x) * 3;
            for (unsigned c = 0; c < 3; ++c) dest[c] = (uint8_t)(fminf(fmaxf(rgb[c], 0), 255) + 0.5f);
        }
    }
    free(kernel);
    *out = resized;
    return FILM_OK;
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
        p->jpeg_quality < 1 || p->jpeg_quality > 100 ||
        !memchr(p->lut_id, 0, sizeof p->lut_id)) return FILM_INVALID;
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

FilmStatus film_develop(const FilmFrame *frame, const FilmLut *lut, FilmImage *out)
{
    if (!frame || !valid_image(&frame->source) || !out || out == &frame->source) return FILM_INVALID;
    const FilmRecipe *p = &frame->recipe;
    FilmStatus status = film_recipe_validate(p);
    if (status != FILM_OK) return status;
    if (p->lut_id[0] && !lut && p->profile.strength) return FILM_STATE;
    if (lut && (!lut->samples || (lut->size != 17 && lut->size != 33 && lut->size != 65)))
        return FILM_INVALID;
    unsigned w, h;
    FilmRect rect;
    const FilmImage *source = &frame->source;
    if (film_scaled_dims(source->width, source->height, p->scale, &w, &h) != FILM_OK ||
        film_crop_rect(w, h, p->aspect, &rect) != FILM_OK) return FILM_INVALID;
    FilmImage graded = {0};
    status = film_image_alloc(&graded, source->width, source->height, FILM_RGB24);
    if (status != FILM_OK) return status;
    for (unsigned y = 0; y < source->height; ++y) {
        uint8_t *pixel = graded.pixels + (size_t)y * source->width * 3;
        for (unsigned x = 0; x < source->width; ++x, pixel += 3) {
            FilmRgb c = film_grade(film_decode_pixel(source, x, y, p->yuv), lut, &p->profile);
            pixel[0] = quantize(c.r);
            pixel[1] = quantize(c.g);
            pixel[2] = quantize(c.b);
        }
    }
    FilmImage scaled = graded;
    if (w != graded.width || h != graded.height) {
        status = film_resize(&graded, w, h, p->filter, &scaled);
        film_image_free(&graded);
        if (status != FILM_OK) return status;
    }
    add_grain(&scaled, p->grain_seed, p->grain_amount);
    for (unsigned y = 0; y < rect.height; ++y) {
        size_t from = ((size_t)(rect.y + y) * w + rect.x) * 3;
        memmove(scaled.pixels + (size_t)y * rect.width * 3, scaled.pixels + from, (size_t)rect.width * 3);
    }
    scaled.width = rect.width;
    scaled.height = rect.height;
    *out = scaled;
    return FILM_OK;
}

void film_frame_free(FilmFrame *frame)
{
    film_image_free(&frame->source);
    *frame = (FilmFrame){0};
}
