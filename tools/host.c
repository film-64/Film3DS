#include "debug.h"

#include <inttypes.h>
#include <string.h>
#include <time.h>

static uint64_t ticks(void *context)
{
    (void)context;
    struct timespec stamp;
    clock_gettime(CLOCK_MONOTONIC, &stamp);
    return (uint64_t)stamp.tv_sec * 1000000 + stamp.tv_nsec / 1000;
}

static FilmStatus mock_init(void *context, FilmPixelFormat format)
{
    (void)context;
    return format == FILM_YUYV ? FILM_OK : FILM_UNSUPPORTED;
}

static FilmStatus mock_capture(void *context, FilmFrame *frame)
{
    (void)context;
    return film_mock_capture(frame);
}

static FilmStatus load_lut(const char *path, FilmLut *lut)
{
    FILE *file = fopen(path, "rb");
    if (!file) return FILM_IO;
    unsigned line;
    FilmStatus status = film_lut_load(file, lut, &line);
    fclose(file);
    if (status != FILM_OK) fprintf(stderr, "%s:%u %s\n", path, line, film_status_name(status));
    return status;
}

static FilmStatus develop(int argc, char **argv)
{
    FilmFrame frame = {0};
    FilmImage image = {0};
    FilmLut lut = {0};
    FilmStatus status = film_frame_read(argv[2], &frame);
    if (status == FILM_OK && argc == 6) {
        status = film_profile_read(argv[5], &frame.recipe.profile);
        if (status == FILM_OK) status = load_lut(argv[4], &lut);
    }
    if (status == FILM_OK) status = film_develop(&frame, lut.samples ? &lut : NULL, &image);
    if (status == FILM_OK) status = film_jpeg_write(argv[3], &image, frame.recipe.jpeg_quality);
    film_image_free(&image);
    film_frame_free(&frame);
    film_lut_free(&lut);
    return status;
}

static FilmStatus bench(const char *path)
{
    FilmLut lut = {0};
    FilmStatus status = load_lut(path, &lut);
    if (status != FILM_OK) return status;
    float checksum = 0;
    const unsigned samples = 640 * 480 * 16;
    uint64_t start = ticks(NULL);
    for (unsigned i = 0; i < samples; ++i) {
        FilmRgb in = {(i % 1021) / 1020.0f, (i % 769) / 768.0f, (i % 509) / 508.0f};
        FilmRgb out = film_lut_sample(&lut, in);
        checksum += out.r + out.g + out.b;
    }
    uint64_t time = ticks(NULL) - start;
    printf("%u^3  %u samples  %" PRIu64 " us  %.2f MPix/s  checksum=%.1f\n",
           lut.size, samples, time, time ? (double)samples / time : 0, checksum);
    film_lut_free(&lut);
    return FILM_OK;
}

int main(int argc, char **argv)
{
    FilmStatus status;
    if ((argc == 4 || argc == 6) && !strcmp(argv[1], "develop")) {
        status = develop(argc, argv);
    } else if (argc == 3 && !strcmp(argv[1], "bench")) {
        status = bench(argv[2]);
    } else if (argc == 3 && !strcmp(argv[1], "profile")) {
        FilmProfile profile = film_default_recipe().profile;
        status = film_profile_write(argv[2], &profile);
    } else if ((argc == 4 || argc == 6) && !strcmp(argv[1], "mock")) {
        FilmDebug d = {.ops = {mock_init, mock_capture, ticks, NULL},
                       .jpeg_path = argv[2], .source_path = argv[3], .ui = film_ui_new(3)};
        FilmLut lut = {0};
        status = FILM_OK;
        if (argc == 6) {
            status = film_profile_read(argv[5], &d.ui.recipe.profile);
            if (status == FILM_OK) status = load_lut(argv[4], &lut);
            if (status == FILM_OK) {
                d.lut = &lut;
                const char *name = strrchr(argv[4], '/');
                snprintf(d.ui.recipe.lut_id, sizeof d.ui.recipe.lut_id, "%s", name ? name + 1 : argv[4]);
            }
        }
        if (status == FILM_OK) film_debug_serve(&d, stdin, stdout);
        film_debug_clear(&d);
        film_lut_free(&lut);
    } else {
        fprintf(stderr, "filmctl mock out.jpg out.f3s [look.cube look.f3p]\n"
                        "filmctl develop input.f3s out.jpg [look.cube look.f3p]\n"
                        "filmctl profile look.f3p\nfilmctl bench look.cube\n");
        return 2;
    }
    if (status != FILM_OK) fprintf(stderr, "%s\n", film_status_name(status));
    return status != FILM_OK;
}
