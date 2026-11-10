#include "storage_internal.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PROFILE_BYTES = 172, FRAME_HEADER = 304, FRAME_HEADER_V2 = 308, VERSION = 1 };
enum { FRAME_HEADER_V3 = FRAME_HEADER_V2 + 4 * FILM_TONE_COUNT };
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "Film format requires binary32");

static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (i * 8));
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= (uint32_t)p[i] << (i * 8);
    return n;
}

static void put_float(uint8_t *p, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, 4);
    put32(p, bits);
}

static float get_float(const uint8_t *p)
{
    uint32_t bits = get32(p);
    float value;
    memcpy(&value, &bits, 4);
    return value;
}

static void put_hsl(uint8_t *p, FilmHsl hsl)
{
    put_float(p, hsl.hue);
    put_float(p + 4, hsl.saturation);
    put_float(p + 8, hsl.lightness);
}

static FilmHsl get_hsl(const uint8_t *p)
{
    return (FilmHsl){get_float(p), get_float(p + 4), get_float(p + 8)};
}

static void put_profile(uint8_t *p, const FilmProfile *profile)
{
    put32(p, profile->input);
    put32(p + 4, profile->output);
    put_float(p + 8, profile->strength);
    put_float(p + 12, profile->exposure);
    put_hsl(p + 16, profile->global);
    for (unsigned i = 0; i < 12; ++i) put_hsl(p + 28 + i * 12, profile->zones[i]);
}

static FilmProfile get_profile(const uint8_t *p)
{
    FilmProfile profile = {.input = get32(p), .output = get32(p + 4),
        .strength = get_float(p + 8), .exposure = get_float(p + 12), .global = get_hsl(p + 16)};
    for (unsigned i = 0; i < 12; ++i) profile.zones[i] = get_hsl(p + 28 + i * 12);
    return profile;
}

static uint32_t crc32(uint32_t crc, const uint8_t *data, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return crc;
}

FilmStatus film_write_begin(FilmWrite *write, const char *path)
{
    if (!path || !*path) return FILM_INVALID;
    size_t length = strlen(path);
    char *temporary = malloc(length + sizeof ".tmp.XXXXXX");
    if (!temporary) return FILM_NOMEM;
    memcpy(temporary, path, length);
    memcpy(temporary + length, ".tmp.XXXXXX", sizeof ".tmp.XXXXXX");
    int fd = mkstemp(temporary);
    if (fd < 0) { free(temporary); return FILM_IO; }
    FILE *file = fdopen(fd, "wb");
    if (!file) { close(fd); remove(temporary); free(temporary); return FILM_IO; }
    *write = (FilmWrite){file, temporary, path};
    return FILM_OK;
}

FilmStatus film_write_end(FilmWrite *write, FilmStatus status)
{
    if (fclose(write->file) != 0 && status == FILM_OK) status = FILM_IO;
    if (status == FILM_OK && rename(write->temporary, write->destination) != 0) status = FILM_IO;
    if (status != FILM_OK) remove(write->temporary);
    free(write->temporary);
    return status;
}

FilmStatus film_profile_write(const char *path, const FilmProfile *profile)
{
    FilmStatus status = film_profile_validate(profile);
    if (status != FILM_OK) return status;
    uint8_t data[PROFILE_BYTES + 12] = {'F', '3', 'P', 'R'};
    put32(data + 4, VERSION);
    put_profile(data + 8, profile);
    put32(data + 8 + PROFILE_BYTES, ~crc32(UINT32_MAX, data, 8 + PROFILE_BYTES));
    FilmWrite write;
    status = film_write_begin(&write, path);
    if (status != FILM_OK) return status;
    status = fwrite(data, 1, sizeof data, write.file) == sizeof data ? FILM_OK : FILM_IO;
    return film_write_end(&write, status);
}

static FilmStatus end_of_record(FILE *file)
{
    if (fgetc(file) != EOF) return FILM_CORRUPT;
    return ferror(file) ? FILM_IO : FILM_OK;
}

FilmStatus film_profile_read(const char *path, FilmProfile *profile)
{
    if (!path || !profile) return FILM_INVALID;
    FILE *file = fopen(path, "rb");
    if (!file) return FILM_IO;
    uint8_t data[PROFILE_BYTES + 12];
    FilmStatus status = FILM_CORRUPT;
    if (fread(data, 1, sizeof data, file) == sizeof data && !memcmp(data, "F3PR", 4)) {
        status = end_of_record(file);
        if (status == FILM_OK && get32(data + 4) != VERSION) status = FILM_UNSUPPORTED;
        if (status == FILM_OK && get32(data + 8 + PROFILE_BYTES) !=
                                 ~crc32(UINT32_MAX, data, 8 + PROFILE_BYTES)) status = FILM_CORRUPT;
        if (status == FILM_OK) {
            FilmProfile decoded = get_profile(data + 8);
            status = film_profile_validate(&decoded);
            if (status == FILM_OK) *profile = decoded;
        }
    }
    if (ferror(file)) status = FILM_IO;
    fclose(file);
    return status;
}

FilmStatus film_frame_write(const char *path, const FilmFrame *frame)
{
    if (!frame || !frame->source.pixels) return FILM_INVALID;
    FilmStatus status = film_recipe_validate(&frame->recipe);
    if (status != FILM_OK) return status;
    const FilmImage *image = &frame->source;
    const FilmRecipe *r = &frame->recipe;
    size_t bytes = film_image_bytes(image->width, image->height, image->format);
    unsigned w, h;
    FilmRect rect;
    if (!bytes || film_scaled_dims(image->width, image->height, r->scale, &w, &h) != FILM_OK ||
        film_crop_rect(w, h, r->aspect, &rect) != FILM_OK) return FILM_INVALID;
    unsigned version = r->base_saturation != 0 ? 2 : VERSION;
    for (unsigned i = 0; i < FILM_TONE_COUNT; ++i)
        if (r->tone.value[i] != 0) version = 3;
    size_t header_size = version == 3 ? FRAME_HEADER_V3 : version == 2 ? FRAME_HEADER_V2 : FRAME_HEADER;
    uint8_t header[FRAME_HEADER_V3] = {'F', '3', 'S', 'R'};
    put32(header + 4, version);
    put32(header + 8, header_size);
    put32(header + 12, (uint32_t)bytes);
    put32(header + 16, image->width);
    put32(header + 20, image->height);
    put32(header + 24, image->format);
    put32(header + 28, (uint32_t)frame->captured_at);
    put32(header + 32, (uint32_t)(frame->captured_at >> 32));
    put32(header + 36, frame->exposure_us);
    put32(header + 40, frame->iso);
    put32(header + 44, frame->camera);
    header[48] = r->aspect;
    header[49] = r->scale;
    header[50] = r->filter;
    header[51] = r->yuv;
    put32(header + 52, r->grain_seed);
    put_float(header + 56, r->grain_amount);
    put32(header + 60, r->jpeg_quality);
    memcpy(header + 64, r->lut_id, strlen(r->lut_id) + 1);
    put_profile(header + 128, &r->profile);
    if (version >= 2) put_float(header + FRAME_HEADER - 4, r->base_saturation);
    if (version == 3)
        for (unsigned i = 0; i < FILM_TONE_COUNT; ++i)
            put_float(header + FRAME_HEADER_V2 - 4 + 4 * i, r->tone.value[i]);
    uint32_t crc = crc32(UINT32_MAX, header, header_size - 4);
    put32(header + header_size - 4, ~crc32(crc, image->pixels, bytes));
    FilmWrite write;
    status = film_write_begin(&write, path);
    if (status != FILM_OK) return status;
    if (fwrite(header, 1, header_size, write.file) != header_size ||
        fwrite(image->pixels, 1, bytes, write.file) != bytes) status = FILM_IO;
    return film_write_end(&write, status);
}

static FilmStatus read_frame(FILE *file, FilmFrame *frame)
{
    uint8_t h[FRAME_HEADER_V3];
    if (fread(h, 1, 8, file) != 8 || memcmp(h, "F3SR", 4)) return FILM_CORRUPT;
    uint32_t version = get32(h + 4);
    if (version < VERSION || version > 3) return FILM_UNSUPPORTED;
    size_t header_size = version == 3 ? FRAME_HEADER_V3 : version == 2 ? FRAME_HEADER_V2 : FRAME_HEADER;
    if (fread(h + 8, 1, header_size - 8, file) != header_size - 8 ||
        get32(h + 8) != header_size) return FILM_CORRUPT;
    unsigned w = get32(h + 16), height = get32(h + 20);
    FilmPixelFormat format = get32(h + 24);
    size_t bytes = film_image_bytes(w, height, format);
    if (!bytes || bytes != get32(h + 12)) return FILM_CORRUPT;

    FilmStatus status = film_image_alloc(&frame->source, w, height, format);
    if (status != FILM_OK) return status;
    if (fread(frame->source.pixels, 1, bytes, file) != bytes) return FILM_CORRUPT;
    status = end_of_record(file);
    if (status != FILM_OK) return status;
    uint32_t crc = crc32(UINT32_MAX, h, header_size - 4);
    if (get32(h + header_size - 4) != ~crc32(crc, frame->source.pixels, bytes)) return FILM_CORRUPT;

    frame->captured_at = get32(h + 28) | (uint64_t)get32(h + 32) << 32;
    frame->exposure_us = get32(h + 36);
    frame->iso = get32(h + 40);
    frame->camera = get32(h + 44);
    FilmRecipe *r = &frame->recipe;
    r->aspect = h[48]; r->scale = h[49]; r->filter = h[50]; r->yuv = h[51];
    r->grain_seed = get32(h + 52);
    r->grain_amount = get_float(h + 56);
    if (version >= 2) r->base_saturation = get_float(h + FRAME_HEADER - 4);
    if (version == 3)
        for (unsigned i = 0; i < FILM_TONE_COUNT; ++i)
            r->tone.value[i] = get_float(h + FRAME_HEADER_V2 - 4 + 4 * i);
    r->jpeg_quality = get32(h + 60);
    memcpy(r->lut_id, h + 64, sizeof r->lut_id);
    r->profile = get_profile(h + 128);
    status = film_recipe_validate(r);
    if (status != FILM_OK) return status;
    unsigned scaled_w, scaled_h;
    FilmRect rect;
    if (film_scaled_dims(w, height, r->scale, &scaled_w, &scaled_h) != FILM_OK ||
        film_crop_rect(scaled_w, scaled_h, r->aspect, &rect) != FILM_OK) return FILM_CORRUPT;
    return FILM_OK;
}

FilmStatus film_frame_read(const char *path, FilmFrame *out)
{
    if (!path || !out) return FILM_INVALID;
    FILE *file = fopen(path, "rb");
    if (!file) return FILM_IO;
    FilmFrame frame = {0};
    FilmStatus status = read_frame(file, &frame);
    if (ferror(file)) status = FILM_IO;
    fclose(file);
    if (status == FILM_OK) *out = frame;
    else film_frame_free(&frame);
    return status;
}
