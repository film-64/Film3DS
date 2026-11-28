#include "debug.h"
#include "raw_darkroom.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

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
    unsigned line;
    FilmStatus status = film_lut_load_path(path, lut, &line);
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

static FilmStatus calibration_rgb(const char *source, const char *target)
{
    FilmFrame frame = {0};
    FilmImage image = {0};
    FilmStatus status = film_frame_read(source, &frame);
    if (status == FILM_OK) status = film_rgb24(&frame.source, frame.recipe.yuv, &image);
    if (status == FILM_OK) {
        FILE *file = fopen(target, "wbx");
        if (!file) status = FILM_IO;
        else {
            size_t bytes = film_image_bytes(image.width, image.height, image.format);
            if (fprintf(file, "P6\n%u %u\n255\n", image.width, image.height) < 0 ||
                fwrite(image.pixels, 1, bytes, file) != bytes) status = FILM_IO;
            if (fclose(file)) status = FILM_IO;
            if (status != FILM_OK) remove(target);
        }
    }
    film_image_free(&image);
    film_frame_free(&frame);
    return status;
}

static FilmStatus neutral_wb(char **argv)
{
    unsigned roi[4];
    for(unsigned i=0;i<4;++i){
        const char *word=argv[4+i];char *end;
        if(!*word||strspn(word,"0123456789")!=strlen(word))return FILM_INVALID;
        errno=0;unsigned long n=strtoul(word,&end,10);
        if(errno||*end||n>UINT_MAX)return FILM_INVALID;
        roi[i]=(unsigned)n;
    }
    FilmRawIsp isp={0};FILE *profile=fopen(argv[3],"rb");
    if(!profile)return FILM_IO;
    FilmStatus status=film_raw_profile_load("film-raw-profile-load/v1",profile,&isp);
    if(fclose(profile)&&status==FILM_OK)status=FILM_IO;
    FilmFrame frame={0};FilmRawScene scene={0};
    FilmDevelopOptions options={.raw_isp=&isp};
    if(status==FILM_OK)status=film_frame_read(argv[2],&frame);
    if(status==FILM_OK)status=film_raw_frame_scene(&frame,&options,&scene);
    FilmNeutralPatch patch={roi[0],roi[1],roi[2],roi[3],.01f,.95f};
    FilmNeutralBalance result={0};
    if(status==FILM_OK)status=film_raw_scene_neutral(&scene,&patch,&result);
    if(status==FILM_OK)printf("{\"kelvin\":%u,\"red\":%d,\"blue\":%d,\"accepted\":%u,\"rejected\":%u,\"residual_stops\":%.8g,\"spread_stops\":%.8g}\n",
        result.balance.kelvin,result.balance.red,result.balance.blue,result.accepted,result.rejected,
        result.residual_stops,result.spread_stops);
    film_raw_scene_free(&scene);film_frame_free(&frame);return status;
}

int main(int argc, char **argv)
{
    FilmStatus status;
    if (argc == 8 && !strcmp(argv[1], "neutral-wb")) {
        status = neutral_wb(argv);
    } else if ((argc == 4 || argc == 6) && !strcmp(argv[1], "develop")) {
        status = develop(argc, argv);
    } else if (argc == 4 && !strcmp(argv[1], "calibration-rgb")) {
        status = calibration_rgb(argv[2], argv[3]);
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
                        "filmctl calibration-rgb input.f3s out.ppm\n"
                        "filmctl neutral-wb input.f3s calibration.f3isp x y width height\n"
                        "filmctl profile look.f3p\nfilmctl bench look.cube\n");
        return 2;
    }
    if (status != FILM_OK) fprintf(stderr, "%s\n", film_status_name(status));
    return status != FILM_OK;
}
