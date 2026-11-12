#include "film.h"

FilmStatus film_mock_capture(FilmFrame *frame)
{
    if (!frame) return FILM_INVALID;
    FilmStatus status = film_image_alloc(&frame->source, 640, 480, FILM_YUYV);
    if (status != FILM_OK) return status;
    for (unsigned y = 0; y < 480; ++y) {
        for (unsigned x = 0; x < 640; x += 2) {
            uint8_t *p = frame->source.pixels + ((size_t)y * 640 + x) * 2;
            p[0] = 42 + 164 * x / 639;
            p[1] = 84 + 88 * y / 479;
            p[2] = 42 + 164 * (x + 1) / 639;
            p[3] = 80 + 96 * x / 639;
        }
    }
    frame->camera = 0;
    return FILM_OK;
}
