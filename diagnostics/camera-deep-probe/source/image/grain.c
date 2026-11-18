#include "film.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t grain_hash(uint32_t seed, unsigned x, unsigned y)
{
    uint32_t v = seed ^ (x * UINT32_C(0x9e3779b9)) ^ (y * UINT32_C(0x85ebca6b));
    v = (v ^ (v >> 16)) * UINT32_C(0x7feb352d);
    v = (v ^ (v >> 15)) * UINT32_C(0x846ca68b);
    return v ^ (v >> 16);
}

static int particle(uint32_t value, unsigned occupancy)
{
    return (value >> 24) < occupancy ? (int)(value & 255) - (int)((value >> 8) & 255) : 0;
}

FilmStatus film_grain_profile_load(FILE *file, FilmGrainProfile *out)
{
    if (!file || !out) return FILM_INVALID;
    FilmGrainProfile profile;
    char magic[6];
    if (fscanf(file, "%5s", magic) != 1 || strcmp(magic, "F3G1")) return FILM_CORRUPT;
    for (unsigned i = 0; i < 9; ++i) {
        float code, rms, corr;
        if (fscanf(file, "%f%f%f", &code, &rms, &corr) != 3 ||
            !(code >= 0 && code <= 255 && (!i || code > profile.code[i - 1])) ||
            !(rms >= 0 && rms <= 32) || !(fabsf(corr) <= .95f)) return FILM_CORRUPT;
        profile.code[i] = code;
        profile.rms[i] = rms;
        profile.correlation[i] = corr;
    }
    if (fscanf(file, "%5s", magic) != EOF) return FILM_CORRUPT;
    if (ferror(file)) return FILM_IO;
    *out = profile;
    return FILM_OK;
}

static unsigned pixel_luma(const uint8_t *p)
{
    return (54u * p[0] + 183u * p[1] + 19u * p[2] + 128) >> 8;
}

static void add_delta(uint8_t *p, int delta)
{
    for (unsigned c = 0; c < 3; ++c) {
        int code = p[c] + delta;
        p[c] = code < 0 ? 0 : code > 255 ? 255 : code;
    }
}

FilmStatus film_grain_measured(FilmImage *image, uint32_t seed, float amount, const FilmGrainProfile *profile)
{
    if (!amount) return FILM_OK;
    float *columns = calloc(image->width, sizeof *columns);
    if (!columns) return FILM_NOMEM;
    struct { float rms, corr, innovation; } table[256];
    unsigned lower = 0;
    for (unsigned i = 0; i < 256; ++i) {
        while (lower < 7 && i > profile->code[lower + 1]) ++lower;
        unsigned upper = lower + 1;
        float t = (i - profile->code[lower]) / (profile->code[upper] - profile->code[lower]);
        t = fminf(fmaxf(t, 0), 1);
        float corr = profile->correlation[lower] * (1 - t) + profile->correlation[upper] * t;
        table[i].corr = corr;
        table[i].innovation = sqrtf(1 - corr * corr);
        table[i].rms = amount * (profile->rms[lower] * (1 - t) + profile->rms[upper] * t);
    }
    for (unsigned y = 0; y < image->height; ++y) {
        float row = 0;
        uint8_t *p = image->pixels + (size_t)y * image->width * 3;
        for (unsigned x = 0; x < image->width; ++x, p += 3) {
            unsigned luma = pixel_luma(p);
            float corr = table[luma].corr, gain = table[luma].innovation;
            uint32_t rand = grain_hash(seed, x, y);
            float white = ((int)(rand & 255) - (int)((rand >> 8) & 255)) * .0095684f;
            row = x ? corr * row + gain * white : white;
            columns[x] = y ? corr * columns[x] + gain * row : row;
            add_delta(p, (int)roundf(columns[x] * table[luma].rms));
        }
    }
    free(columns);
    return FILM_OK;
}

void film_grain_adaptive(FilmImage *image, uint32_t seed, float amount, FilmScale scale, FilmGrain grain)
{
    if (!amount) return;
    // Periods are expressed at the actual export resolution.
    static const unsigned periods[FILM_GRAIN_COUNT][3] = {{2, 3, 4}, {3, 5, 6}, {5, 8, 10}};
    static const int fine_weight[] = {3, 2, 1}, coarse_weight[] = {2, 3, 4};
    unsigned period = periods[grain][scale];
    unsigned area = period * period;
    uint32_t cluster_seed = seed ^ UINT32_C(0xd1b54a35);
    int amplitude[256];
    for (int luma = 0; luma < 256; ++luma) {
        int headroom = luma < 255 - luma ? luma : 255 - luma;
        amplitude[luma] = (int)(amount * (headroom < 24 ? headroom : 24) * 10 + .5f);
    }
    for (unsigned y = 0; y < image->height; ++y) {
        unsigned cy = y / period, fy = y % period;
        uint8_t *pixel = image->pixels + (size_t)y * image->width * 3;
        uint32_t tl = grain_hash(cluster_seed, 0, cy), tr = grain_hash(cluster_seed, 1, cy);
        uint32_t bl = grain_hash(cluster_seed, 0, cy + 1), br = grain_hash(cluster_seed, 1, cy + 1);
        for (unsigned x = 0, cx = 0, fx = 0; x < image->width; ++x, pixel += 3) {
            unsigned luma = pixel_luma(pixel);
            unsigned dark = 255 - luma;
            int fine = particle(grain_hash(seed, x, y), 28 + dark * 150 / 255);
            unsigned occupied = 7 + dark * 105 / 255;
            int upper = particle(tl, occupied) * (int)(period - fx) + particle(tr, occupied) * (int)fx;
            int lower = particle(bl, occupied) * (int)(period - fx) + particle(br, occupied) * (int)fx;
            int coarse = (upper * (int)(period - fy) + lower * (int)fy) / (int)area;
            int delta = (fine * fine_weight[grain] + coarse * coarse_weight[grain]) * amplitude[luma];
            delta = (delta + (delta < 0 ? -4096 : 4096)) / 8192;
            add_delta(pixel, delta);
            if (++fx == period) {
                fx = 0;
                ++cx;
                tl = tr;
                bl = br;
                tr = grain_hash(cluster_seed, cx + 1, cy);
                br = grain_hash(cluster_seed, cx + 1, cy + 1);
            }
        }
    }
}
