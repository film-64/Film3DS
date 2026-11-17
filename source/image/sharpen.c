#include "film.h"

#include <stdlib.h>
#include <string.h>

static unsigned edge(int index, unsigned size)
{
    return index < 0 ? 0 : (unsigned)index >= size ? size - 1 : (unsigned)index;
}

static int luma(const uint8_t *rgb)
{
    return (54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2] + 128) / 256;
}

FilmStatus film_sharpen(FilmImage *image, int level)
{
    if (!image || !image->pixels || image->format != FILM_RGB24 ||
        !film_image_bytes(image->width, image->height, image->format) || level < -5 || level > 5)
        return FILM_INVALID;
    if (!level) return FILM_OK;
    size_t bytes = film_image_bytes(image->width, image->height, FILM_RGB24);
    uint8_t *result = malloc(bytes);
    if (!result) return FILM_NOMEM;
    for (unsigned y = 0; y < image->height; ++y) {
        for (unsigned x = 0; x < image->width; ++x) {
            int blur[3] = {0};
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    unsigned sx = edge((int)x + dx, image->width);
                    unsigned sy = edge((int)y + dy, image->height);
                    const uint8_t *p = image->pixels + ((size_t)sy * image->width + sx) * 3;
                    int weight = (dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1);
                    for (unsigned c = 0; c < 3; ++c) blur[c] += weight * p[c];
                }
            uint8_t blurred[3];
            for (unsigned c = 0; c < 3; ++c) blurred[c] = (blur[c] + 8) / 16;
            size_t index = ((size_t)y * image->width + x) * 3;
            const uint8_t *original = image->pixels + index;
            int detail = luma(original) - luma(blurred);
            for (unsigned c = 0; c < 3; ++c) {
                int delta = level > 0 ? detail : original[c] - blurred[c];
                float value = original[c] + level * delta / 5.0f;
                result[index + c] = value < 0 ? 0 : value > 255 ? 255 : (uint8_t)(value + .5f);
            }
        }
    }
    memcpy(image->pixels, result, bytes);
    free(result);
    return FILM_OK;
}
