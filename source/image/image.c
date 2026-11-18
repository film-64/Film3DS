#include "film.h"
#include "film_runtime_contract.h"
#include "raw_darkroom.h"
#include "layered_grain.h"

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
    case FILM_RAW10_RGGB: return w>=4 && h>=4 && !(w&1) && !(h&1) && (size_t)w*h<=FILM_RAW_MAX_PIXELS ? (size_t)w*h*2 : 0;
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
    if(source->format==FILM_RAW10_RGGB) {
        FilmFrame frame={.source=*source,.recipe=film_default_recipe()};
        FilmDevelopCanvas canvas={0};
        FilmStatus s=film_develop_canvas(&frame,NULL,NULL,&canvas);
        if(s==FILM_OK)s=film_develop_canvas_render(&canvas,&frame.recipe,NULL,true,out);
        film_develop_canvas_free(&canvas);return s;
    }
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
        p->jpeg_quality < 1 || p->jpeg_quality > 100 || p->sharpness < -5 || p->sharpness > 5 ||
        !memchr(p->lut_id, 0, sizeof p->lut_id)) return FILM_INVALID;
    for (unsigned i = 0; i < FILM_TONE_COUNT; ++i)
        if (!isfinite(p->tone.value[i]) || p->tone.value[i] != film_tone_limit(i, p->tone.value[i]))
            return FILM_INVALID;
    for (unsigned i = 0; i < FILM_TONE_COUNT; ++i) {
        float value = p->input_tone.value[i];
        if (!isfinite(value) || value != film_tone_limit(i, value)) return FILM_INVALID;
        if (i != FILM_TONE_EV && i != FILM_TONE_MID && i != FILM_TONE_HIGH &&
            i != FILM_TONE_SHADOWS && value != 0) return FILM_INVALID;
        value = p->output_tone.value[i];
        if (!isfinite(value) || value != film_tone_limit(i, value)) return FILM_INVALID;
        if (i != FILM_TONE_MID && i != FILM_TONE_HIGH && i != FILM_TONE_SHADOWS && value != 0)
            return FILM_INVALID;
    }
    if ((p->white_balance.kelvin && (p->white_balance.kelvin<1000 || p->white_balance.kelvin>10000)) ||
        p->white_balance.red < -9 || p->white_balance.red > 9 || p->white_balance.blue < -9 || p->white_balance.blue > 9 ||
        p->crop_percent>100 || (!p->crop_positioned && (p->crop_x || p->crop_y))) return FILM_INVALID;
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

static FilmStatus develop(const FilmFrame *frame, const FilmLut *lut,
                          const FilmDevelopOptions *options, FilmImage *out, FilmDevelopCanvas *canvas)
{
    if (!film_runtime_source_valid("film-darkroom/v1")) return FILM_STATE;
    if (!frame || !valid_image(&frame->source) || (!out && !canvas) || out == &frame->source) return FILM_INVALID;
    const FilmRecipe *p = &frame->recipe;
    FilmStatus status = film_recipe_validate(p);
    if (status != FILM_OK) return status;
    if (options && options->adaptive_grain && (unsigned)options->grain >= FILM_GRAIN_COUNT) return FILM_INVALID;
    bool builtin = !lut && p->profile.strength && !strcmp(p->lut_id, FILM_WARM_LUT_ID);
    if (p->lut_id[0] && !lut && p->profile.strength && !builtin) return FILM_STATE;
    if (lut && (!lut->samples || (lut->size < 2 || lut->size > 256)))
        return FILM_INVALID;
    unsigned w, h;
    FilmRect rect;
    const FilmImage *source = &frame->source;
    if (film_scaled_dims(source->width, source->height, p->scale, &w, &h) != FILM_OK ||
        film_crop_rect(w, h, p->aspect, &rect) != FILM_OK) return FILM_INVALID;
    FilmImage graded = {0};
    bool resized = w != source->width || h != source->height;
    uint16_t *high_precision = NULL;
    if ((options && resized) || canvas) {
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
    FilmRgb *clean=NULL;
    if(options&&options->chroma_denoise!=0) {
        size_t count=(size_t)source->width*source->height;
        clean=malloc(count*sizeof *clean);
        if(!clean)status=FILM_NOMEM;
        else {
            for(unsigned y=0;y<source->height;++y)for(unsigned x=0;x<source->width;++x){FilmRgb c=decode_pixel(source,x,y,&t);
                if(camera_color)c=film_camera_color_apply(c,camera_color);
                if(halo)c=film_srgb_add_halation(c,halation_energy(halo,mask_w,mask_h,x,y));
                clean[(size_t)y*source->width+x]=c;
            }
            status=film_darkroom_denoise("film-darkroom-denoise/v1",clean,source->width,source->height,options->chroma_denoise);
        }
        if(status!=FILM_OK){free(clean);free(halo);film_lut_free(&generated);film_image_free(&graded);free(high_precision);return status;}
    }
    for (unsigned y = 0; y < source->height; ++y) {
        size_t index = (size_t)y * source->width * 3;
        for (unsigned x = 0; x < source->width; ++x, index += 3) {
            FilmRgb c;
            if(clean)c=clean[(size_t)y*source->width+x];
            else {
                c=decode_pixel(source,x,y,&t);
                if(camera_color)c=film_camera_color_apply(c,camera_color);
                if(halo)c=film_srgb_add_halation(c,halation_energy(halo,mask_w,mask_h,x,y));
            }
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
    free(clean);free(halo);
    film_lut_free(&generated);
    if (canvas) {
        *canvas = (FilmDevelopCanvas){source->width, source->height, high_precision};
        return FILM_OK;
    }
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
    status = film_sharpen(&scaled, p->sharpness);
    if (status != FILM_OK) { film_image_free(&scaled); return status; }
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

FilmStatus film_raw_prepared_halation(FilmRawPrepared *prepared)
{
    if(!film_runtime_source_valid("film-app-raw-canvas/v1"))return FILM_STATE;
    if(!prepared||!prepared->pixels)return FILM_INVALID;
    FilmImage mask={0};FilmStatus status=film_image_alloc(&mask,prepared->width,prepared->height,FILM_RGB24);
    if(status!=FILM_OK)return status;
    for(size_t i=0;i<(size_t)prepared->width*prepared->height;++i){FilmRgb c=prepared->pixels[i];
        mask.pixels[3*i]=quantize(c.r);mask.pixels[3*i+1]=quantize(c.g);mask.pixels[3*i+2]=quantize(c.b);}
    unsigned w=(mask.width+HALATION_CELL-1)/HALATION_CELL,h=(mask.height+HALATION_CELL-1)/HALATION_CELL;
    FilmYuvTransform t=film_yuv_transform(FILM_601_FULL,0);float *halo=make_halation(&mask,&t,NULL,w,h);
    film_image_free(&mask);if(!halo)return FILM_NOMEM;
    for(unsigned y=0;y<prepared->height;++y)for(unsigned x=0;x<prepared->width;++x){size_t i=(size_t)y*prepared->width+x;
        prepared->pixels[i]=film_srgb_add_halation(prepared->pixels[i],halation_energy(halo,w,h,x,y));}
    free(halo);return FILM_OK;
}

FilmStatus film_develop_canvas(const FilmFrame *frame, const FilmLut *lut,
                               const FilmDevelopOptions *options, FilmDevelopCanvas *out)
{
    if (!out) return FILM_INVALID;
    if(frame && frame->source.format==FILM_RAW10_RGGB) {
        if(!film_runtime_source_valid("film-app-raw-canvas/v1"))return FILM_STATE;
        FilmRawScene scene={0};FilmStatus s=film_raw_frame_scene(frame,options,&scene);
        if(s==FILM_OK)s=film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&frame->recipe,lut,options,out);
        film_raw_scene_free(&scene);return s;
    }
    return develop(frame,lut,options,NULL,out);
}

static FilmStatus layered_render(FilmImage *image,const FilmRecipe *p,const FilmDevelopOptions *o,unsigned reference_width)
{
    if(!o->grain_profile||!o->grain_renderer)return FILM_STATE;
    FilmLayeredGrain grain={.amount=p->grain_amount,.size=o->grain_size,
        .coarse=o->grain==FILM_GRAIN_FINE?.2f:o->grain==FILM_GRAIN_COARSE?.8f:.5f,
        .shadow=o->grain_shadow,.mid=o->grain_mid,.highlight=o->grain_highlight,
        .reference_width=reference_width,.seed=p->grain_seed};
    return o->grain_renderer("film-grain-layered/v1",image,&grain,o->grain_profile,o->grain_texture);
}

void film_develop_canvas_free(FilmDevelopCanvas *canvas)
{
    free(canvas->pixels);
    *canvas=(FilmDevelopCanvas){0};
}

FilmStatus film_develop_canvas_render(const FilmDevelopCanvas *canvas, const FilmRecipe *p,
                                      const FilmDevelopOptions *options, bool full_preview, FilmImage *out)
{
    if (!film_runtime_source_valid(full_preview ? "film-darkroom/v1" : "film-darkroom-save/v1")) return FILM_STATE;
    if (!canvas || !canvas->pixels || !out || !film_image_bytes(canvas->width,canvas->height,FILM_RGB24) ||
        film_recipe_validate(p)!=FILM_OK || (options && options->adaptive_grain && (unsigned)options->grain>=FILM_GRAIN_COUNT))
        return FILM_INVALID;
    if(!full_preview&&options&&options->output_renderer)
        return options->output_renderer("film-darkroom-gpu-output/v1",canvas,p,options,out);
    FilmRect crop={0,0,canvas->width,canvas->height}, target=crop;
    if (!full_preview) {
        unsigned w,h;
        if (film_composition_rect(canvas->width,canvas->height,p,&crop)!=FILM_OK ||
            film_scaled_dims(canvas->width,canvas->height,p->scale,&w,&h)!=FILM_OK ||
            film_crop_rect(w,h,p->aspect,&target)!=FILM_OK) return FILM_INVALID;
    }
    uint16_t *selected=NULL;
    const uint16_t *pixels=canvas->pixels;
    if (crop.width!=canvas->width || crop.height!=canvas->height || crop.x || crop.y) {
        selected=malloc((size_t)crop.width*crop.height*3*sizeof *selected);
        if (!selected) return FILM_NOMEM;
        for (unsigned y=0;y<crop.height;++y)
            memcpy(selected+(size_t)y*crop.width*3,
                   pixels+((size_t)(crop.y+y)*canvas->width+crop.x)*3,
                   (size_t)crop.width*3*sizeof *selected);
        pixels=selected;
    }
    FilmImage image={0};
    FilmStatus status=resize_rgb(pixels,crop.width,crop.height,target.width,target.height,p->filter,true,&image);
    free(selected);
    if (status!=FILM_OK) return status;
    status=film_sharpen(&image,p->sharpness);
    if (status==FILM_OK && options && options->layered_grain)
        status=layered_render(&image,p,options,crop.width);
    else if (status==FILM_OK && options && options->grain_profile)
        status=film_grain_measured(&image,p->grain_seed,p->grain_amount,options->grain_profile);
    else if (status==FILM_OK && options && options->adaptive_grain)
        film_grain_adaptive(&image,p->grain_seed,p->grain_amount,full_preview?FILM_NATIVE:p->scale,options->grain);
    else if (status==FILM_OK) add_grain(&image,p->grain_seed,p->grain_amount);
    if (status!=FILM_OK) { film_image_free(&image); return status; }
    *out=image;
    return FILM_OK;
}

FilmStatus film_develop_canvas_render_pair(const FilmDevelopCanvas *canvas,const FilmRecipe *recipe,
    const FilmDevelopOptions *options,FilmImage *context,FilmImage *output)
{
    if(!film_runtime_source_valid("film-darkroom-render-pair/v1"))return FILM_STATE;
    if(!canvas||!context||!output||context==output||context->pixels||output->pixels)return FILM_INVALID;
    FilmImage final={0},view={0},inset={0};
    FilmStatus s=film_develop_canvas_render(canvas,recipe,options,false,&final);
    if(s==FILM_OK)s=film_image_alloc(&view,canvas->width,canvas->height,FILM_RGB24);
    FilmRect crop={0};
    if(s==FILM_OK)s=film_composition_rect(canvas->width,canvas->height,recipe,&crop);
    if(s==FILM_OK){
        /* Context is already graded. Never rerun resize/sharpen/grain for it.
         * The composition rectangle displays the actual final output. */
        for(size_t i=0;i<(size_t)canvas->width*canvas->height*3;++i)
            view.pixels[i]=(uint8_t)((canvas->pixels[i]+128u)/257u);
        const FilmImage *patch=&final;
        if(final.width!=crop.width||final.height!=crop.height){
            s=film_resize(&final,crop.width,crop.height,FILM_BILINEAR,&inset);patch=&inset;
        }
        if(s==FILM_OK)for(unsigned y=0;y<crop.height;++y)
            memcpy(view.pixels+((size_t)(crop.y+y)*view.width+crop.x)*3,
                patch->pixels+(size_t)y*crop.width*3,(size_t)crop.width*3);
    }
    film_image_free(&inset);
    if(s==FILM_OK){*context=view;*output=final;}
    else{film_image_free(&view);film_image_free(&final);}
    return s;
}

FilmStatus film_develop_with_options(const FilmFrame *frame, const FilmLut *lut,
                                     const FilmDevelopOptions *options, FilmImage *out)
{
    if (!out || (frame && out==&frame->source)) return FILM_INVALID;
    if (frame && (frame->source.format==FILM_RAW10_RGGB || frame->recipe.crop_percent || frame->recipe.crop_positioned || (options && options->layered_grain))) {
        FilmDevelopCanvas canvas={0};
        FilmStatus status=film_develop_canvas(frame,lut,options,&canvas);
        if (status==FILM_OK) status=film_develop_canvas_render(&canvas,&frame->recipe,options,false,out);
        film_develop_canvas_free(&canvas);
        return status;
    }
    return develop(frame,lut,options,out,NULL);
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
