#include "platform_develop.h"

#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdlib.h>

enum { TILE = 256, CORE = 240, PAD = 2, SAMPLE_LIMIT = 2 };

static unsigned clamp_pixel(int index, unsigned size)
{
    return index < 0 ? 0 : (unsigned)index >= size ? size - 1 : (unsigned)index;
}

static unsigned texture_index(unsigned x, unsigned y)
{
    unsigned row = TILE - 1 - y;
    unsigned tile = (x >> 3) + (row >> 3) * (TILE / 8);
    unsigned morton = (x & 1) | ((row & 1) << 1) | ((x & 2) << 1) |
                      ((row & 2) << 2) | ((x & 4) << 2) | ((row & 4) << 3);
    return (tile * 64 + morton) * 4;
}

static void upload_tile(C3D_Tex *texture, const FilmImage *image, unsigned x0, unsigned y0)
{
    uint8_t *texture_data = texture->data;
    for (unsigned y = 0; y < TILE; ++y) {
        unsigned sy = clamp_pixel((int)(y0 + y) - PAD, image->height);
        for (unsigned x = 0; x < TILE; ++x) {
            unsigned sx = clamp_pixel((int)(x0 + x) - PAD, image->width);
            const uint8_t *src = image->pixels + ((size_t)sy * image->width + sx) * 3;
            uint8_t *dst = texture_data + texture_index(x, y);
            dst[0] = 255;
            dst[1] = src[2];
            dst[2] = src[1];
            dst[3] = src[0];
        }
    }
    C3D_TexFlush(texture);
}

static float reference_sample(const FilmImage *image, unsigned x, unsigned y, unsigned factor, unsigned channel)
{
    float px = (x + .5f) * 2 / factor - .5f, py = (y + .5f) * 2 / factor - .5f;
    int ix = (int)floorf(px), iy = (int)floorf(py);
    float fx = px - ix, fy = py - iy;
    unsigned left = clamp_pixel(ix, image->width), right = clamp_pixel(ix + 1, image->width);
    unsigned top = clamp_pixel(iy, image->height), bottom = clamp_pixel(iy + 1, image->height);
    const uint8_t *p = image->pixels + channel;
    float a = p[((size_t)top * image->width + left) * 3] * (1 - fx) +
              p[((size_t)top * image->width + right) * 3] * fx;
    float b = p[((size_t)bottom * image->width + left) * 3] * (1 - fx) +
              p[((size_t)bottom * image->width + right) * 3] * fx;
    return a * (1 - fy) + b * fy;
}

static const uint8_t *tile_pixel(const uint8_t *data, unsigned side, unsigned x, unsigned y, bool flipped)
{
    return data + ((size_t)(flipped ? side - 1 - y : y) * side + x) * 4;
}

typedef struct { unsigned max, sum; } TileError;

static TileError tile_error(const FilmImage *image, const uint8_t *data, unsigned factor,
                            FilmRect region, bool flipped)
{
    TileError error = {0};
    unsigned side = TILE * factor / 2, pad = PAD * factor / 2;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 3; ++col) {
            unsigned x = (region.width - 1) * col / 2, y = (region.height - 1) * row / 2;
            const uint8_t *sample = tile_pixel(data, side, pad + x, pad + y, flipped);
            for (unsigned c = 0; c < 3; ++c) {
                int expected = (int)(reference_sample(image, region.x + x, region.y + y, factor, c) + .5f);
                unsigned difference = abs(sample[3 - c] - expected);
                if (difference > error.max) error.max = difference;
                error.sum += difference;
            }
        }
    return error;
}

static const char *finish_readback(C3D_RenderTarget *target, uint8_t *readback, unsigned side, bool drawn)
{
    if (drawn) {
        u32 dims = GX_BUFFER_DIM(side, side);
        C2D_Flush();
        C3D_SyncDisplayTransfer(target->frameBuf.colorBuf, dims, (u32 *)readback, dims,
                               GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                               GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8));
    }
    C3D_FrameEnd(0);
    if (!drawn) return "draw";
    // A blocking frame begin waits for this whole GX queue, including
    // readback. A pending screen-transfer PPF cannot satisfy that wait.
    if (!C3D_FrameBegin(0)) return "queue wait";
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    return R_FAILED(GSPGPU_InvalidateDataCache(readback, side * side * 4)) ? "cache readback" : NULL;
}

static const Tex3DS_SubTexture tile_texture = {TILE, TILE, 0, 1, 1, 0};

static const char *render_tile(C3D_Tex *texture, C3D_RenderTarget *target, uint8_t *readback, unsigned side)
{
    if (R_FAILED(GSPGPU_FlushDataCache(readback, side * side * 4))) return "cache flush";
    if (!C3D_FrameBegin(0)) return "frame begin";
    C2D_TargetClear(target, 0);
    C2D_SceneBegin(target);
    // Rebind because Citro2D caches the texture pointer across uploads.
    C3D_TexBind(0, texture);
    float scale = side / (float)TILE;
    bool drawn = C2D_DrawImageAt((C2D_Image){texture, &tile_texture}, 0, 0, 0, NULL, scale, scale);
    return finish_readback(target, readback, side, drawn);
}

static bool resize_gpu(const FilmImage *image, unsigned w, unsigned h, unsigned factor, FilmImage *out,
                       FilmResizeQuality *quality)
{
    unsigned side = TILE * factor / 2, pad = PAD * factor / 2;
    FilmImage scaled = {0};
    if (film_image_alloc(&scaled, w, h, FILM_RGB24) != FILM_OK) {
        quality->gpu_error = "output alloc";
        return false;
    }
    C3D_Tex texture;
    bool tex_ready = C3D_TexInit(&texture, TILE, TILE, GPU_RGBA8);
    C3D_RenderTarget *target = C3D_RenderTargetCreate(side, side, GPU_RB_RGBA8, -1);
    uint8_t *readback = linearMemAlign(side * side * 4, 0x80);
    const char *error = !tex_ready ? "texture alloc" : !target ? "target alloc" : !readback ? "readback alloc" : NULL;
    if (!error) {
        C3D_TexSetFilter(&texture, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    for (unsigned y0 = 0; !error && y0 < image->height; y0 += CORE)
        for (unsigned x0 = 0; !error && x0 < image->width; x0 += CORE) {
            FilmRect region = {x0 * factor / 2, y0 * factor / 2, CORE * factor / 2, CORE * factor / 2};
            if (region.width > w - region.x) region.width = w - region.x;
            if (region.height > h - region.y) region.height = h - region.y;
            upload_tile(&texture, image, x0, y0);
            error = render_tile(&texture, target, readback, side);
            if (error) break;
            bool flipped = false;
            TileError sample = tile_error(image, readback, factor, region, false);
            if (sample.max > SAMPLE_LIMIT) {
                TileError reverse = tile_error(image, readback, factor, region, true);
                flipped = reverse.max <= SAMPLE_LIMIT || reverse.sum < sample.sum;
                if (flipped) sample = reverse;
            }
            ++quality->tiles;
            if (sample.max > quality->sample_max) quality->sample_max = sample.max;
            for (unsigned y = 0; y < region.height; ++y) {
                const uint8_t *src = tile_pixel(readback, side, pad, pad + y, flipped);
                uint8_t *dst = scaled.pixels + ((size_t)(region.y + y) * w + region.x) * 3;
                for (unsigned x = 0; x < region.width; ++x, src += 4, dst += 3) {
                    dst[0] = src[3];
                    dst[1] = src[2];
                    dst[2] = src[1];
                }
            }
        }
    C3D_TexBind(0, NULL);
    if (target) C3D_RenderTargetDelete(target);
    if (tex_ready) C3D_TexDelete(&texture);
    if (readback) linearFree(readback);
    if (!error) *out = scaled;
    else film_image_free(&scaled);
    quality->gpu_error = error;
    return !error;
}

FilmStatus film_3ds_resize_quality(const FilmImage *image, unsigned factor, FilmResizeQuality *quality)
{
    if (!image || !quality || (factor != 3 && factor != 4) ||
        (factor == 3 && (image->width % 2 || image->height % 2))) return FILM_INVALID;
    *quality = (FilmResizeQuality){0};
    unsigned w = image->width * factor / 2, h = image->height * factor / 2;
    FilmImage cpu = {0}, gpu = {0};
    u64 start = svcGetSystemTick();
    FilmStatus status = film_resize(image, w, h, FILM_BILINEAR, &cpu);
    quality->cpu_us = (svcGetSystemTick() - start) * UINT64_C(1000000) / SYSCLOCK_ARM11;
    if (status != FILM_OK) return status;
    start = svcGetSystemTick();
    bool rendered = resize_gpu(image, w, h, factor, &gpu, quality);
    quality->gpu_us = (svcGetSystemTick() - start) * UINT64_C(1000000) / SYSCLOCK_ARM11;
    if (rendered) {
        uint32_t histogram[256] = {0};
        uint64_t sum = 0, count = (uint64_t)w * h * 3;
        unsigned span = CORE * factor / 2;
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                size_t i = ((size_t)y * w + x) * 3;
                bool seam = (x && (x % span == 0 || (x + 1) % span == 0)) ||
                            (y && (y % span == 0 || (y + 1) % span == 0));
                bool over_four = false;
                for (unsigned c = 0; c < 3; ++c) {
                    unsigned error = abs((int)cpu.pixels[i + c] - gpu.pixels[i + c]);
                    sum += error;
                    ++histogram[error];
                    if (error > quality->max_error) {
                        quality->max_error = error;
                        quality->peak_x = x;
                        quality->peak_y = y;
                    }
                    if (seam && error > quality->seam_error) quality->seam_error = error;
                    over_four |= error > 4;
                }
                quality->over_four += over_four;
            }
        uint64_t cumulative = 0, cutoff = (count * 99 + 99) / 100;
        for (unsigned i = 0; i < 256; ++i) {
            cumulative += histogram[i];
            if (cumulative >= cutoff) { quality->p99_error = i; break; }
        }
        quality->mean_error = sum / (double)count;
        quality->gpu_accepted = quality->sample_max <= SAMPLE_LIMIT &&
                                quality->p99_error <= 1 && quality->max_error <= 2 && quality->seam_error <= 2;
    }
    film_image_free(&gpu);
    film_image_free(&cpu);
    return FILM_OK;
}

static unsigned over(unsigned background, unsigned foreground, unsigned alpha)
{
    return (background * (255 - alpha) + foreground * alpha + 127) / 255;
}

const char *film_3ds_layer_quality(unsigned side, FilmLayerQuality *quality)
{
    enum { LAYERS = 3 };
    if (side != TILE && side != 2 * TILE) return "unsupported size";
    unsigned bytes = side * side * 4, scale = side / TILE;
    C3D_Tex tex[LAYERS];
    uint8_t *pixels[LAYERS];
    unsigned ready = 0;
    C3D_RenderTarget *target = C3D_RenderTargetCreate(side, side, GPU_RB_RGBA8, -1);
    uint8_t *readback = linearMemAlign(bytes, 0x80);
    uint8_t *reference = malloc(side * side * 3);
    const char *error = "layer alloc";
    *quality = (FilmLayerQuality){0};
    if (!target || !readback || !reference) goto cleanup;
    for (; ready < LAYERS; ++ready) {
        if (!C3D_TexInit(&tex[ready], TILE, TILE, GPU_RGBA8)) goto cleanup;
        pixels[ready] = tex[ready].data;
        C3D_TexSetFilter(&tex[ready], GPU_NEAREST, GPU_NEAREST);
    }
    u64 start = svcGetSystemTick();
    for (unsigned y = 0; y < TILE; ++y) {
        for (unsigned x = 0; x < TILE; ++x) {
            unsigned i = texture_index(x, y);
            unsigned halo = (unsigned)abs((int)x - 160) + (unsigned)abs((int)y - 80);
            unsigned grain = ((x * 73u) ^ (y * 137u) ^ (x * y * 19u)) & 255;
            uint8_t shade = grain & 1 ? 255 : 0;
            const uint8_t colors[LAYERS][4] = {
                {255, (x + y) / 2, y, x},
                {halo < 80 ? 80 - halo : 0, 40, 98, 255},
                {(grain >> 4) * (255 - (x + y) / 2) / 255, shade, shade, shade}
            };
            for (unsigned j = 0; j < LAYERS; ++j)
                for (unsigned c = 0; c < 4; ++c) pixels[j][i + c] = colors[j][c];
        }
    }
    for (unsigned j = 0; j < LAYERS; ++j) C3D_TexFlush(&tex[j]);
    quality->prepare_us = (svcGetSystemTick() - start) * UINT64_C(1000000) / SYSCLOCK_ARM11;
    start = svcGetSystemTick();
    for (unsigned y = 0; y < side; ++y)
        for (unsigned x = 0; x < side; ++x) {
            unsigned i = texture_index(x / scale, y / scale);
            for (unsigned c = 0; c < 3; ++c) {
                unsigned value = pixels[0][i + 3 - c];
                for (unsigned j = 1; j < LAYERS; ++j)
                    value = over(value, pixels[j][i + 3 - c], pixels[j][i]);
                reference[(y * side + x) * 3 + c] = value;
            }
        }
    quality->cpu_us = (svcGetSystemTick() - start) * UINT64_C(1000000) / SYSCLOCK_ARM11;
    start = svcGetSystemTick();
    error = "layer frame setup";
    if (R_FAILED(GSPGPU_FlushDataCache(readback, bytes)) || !C3D_FrameBegin(0)) goto cleanup;
    C2D_TargetClear(target, 0);
    C2D_SceneBegin(target);
    bool drawn = true;
    for (unsigned j = 0; j < LAYERS; ++j)
        drawn &= C2D_DrawImageAt((C2D_Image){&tex[j], &tile_texture}, 0, 0, 0, NULL, scale, scale);
    error = finish_readback(target, readback, side, drawn);
    quality->gpu_us = (svcGetSystemTick() - start) * UINT64_C(1000000) / SYSCLOCK_ARM11;
    if (!error) {
        uint32_t histogram[2][256] = {{0}};
        unsigned sum[2] = {0};
        for (unsigned y = 0; y < side; ++y)
            for (unsigned x = 0; x < side; ++x)
                for (unsigned flip = 0; flip < 2; ++flip) {
                    const uint8_t *got = tile_pixel(readback, side, x, y, flip);
                    const uint8_t *want = reference + (y * side + x) * 3;
                    for (unsigned c = 0; c < 3; ++c) {
                        unsigned delta = abs(got[3 - c] - want[c]);
                        ++histogram[flip][delta];
                        sum[flip] += delta;
                    }
                }
        unsigned flip = sum[1] < sum[0], count = 0;
        unsigned channels = side * side * 3, cutoff = (channels * 99 + 99) / 100;
        quality->mean_error = sum[flip] / (double)channels;
        for (unsigned i = 0; i < 256; ++i) {
            if (count < cutoff) quality->p99_error = i;
            count += histogram[flip][i];
            if (histogram[flip][i]) quality->max_error = i;
        }
    }
cleanup:
    C3D_TexBind(0, NULL);
    if (target) C3D_RenderTargetDelete(target);
    while (ready) C3D_TexDelete(&tex[--ready]);
    if (readback) linearFree(readback);
    free(reference);
    return error;
}
