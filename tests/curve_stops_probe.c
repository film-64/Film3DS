/* Offline measurement driver. Link against the frozen, deployed application core.
 * No camera API, production source modification, or JPEG-loss approximation. */
#include "film.h"
#include "darkroom.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void ppm(const char *path, const FilmImage *image)
{
    FILE *out = fopen(path, "wb");
    assert(out && image->format == FILM_RGB24);
    assert(fprintf(out, "P6\n%u %u\n255\n", image->width, image->height) > 0);
    size_t bytes = film_image_bytes(image->width, image->height, image->format);
    assert(fwrite(image->pixels, 1, bytes, out) == bytes);
    assert(fclose(out) == 0);
}

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "probe OUT CUBE_OR_DASH PROFILE_OR_DASH SOURCE_F3S\n"); return 2; }
    FilmLut lut = {0};
    FilmProfile profile = film_default_recipe().profile;
    if (strcmp(argv[2], "-")) {
        FILE *file = fopen(argv[2], "rb");
        unsigned line;
        assert(file && film_lut_load(file, &lut, &line) == FILM_OK);
        fclose(file);
        assert(film_profile_read(argv[3], &profile) == FILM_OK);
    }
    FilmFrame source = {0};
    assert(film_frame_read(argv[4], &source) == FILM_OK);
    FilmYuv yuv = source.recipe.yuv;
    FilmImage decoded = {0};
    assert(film_rgb24(&source.source, yuv, &decoded) == FILM_OK);
    char path[1024];
    snprintf(path, sizeof path, "%s/input.ppm", argv[1]);
    ppm(path, &decoded);
    film_image_free(&decoded);
    snprintf(path, sizeof path, "%s/gray.csv", argv[1]);
    FILE *csv = fopen(path, "w");
    assert(csv);
    fprintf(csv, "control,level,input_code,pre_r,pre_g,pre_b,post_r,post_g,post_b,post_linear_y\n");
    const char *names[] = {"H", "M", "S"};
    const FilmDarkroomRow rows[] = {DARKROOM_HIGHLIGHTS, DARKROOM_MIDTONES, DARKROOM_SHADOWS};
    for (unsigned control = 0; control < 3; ++control) {
        for (int level = -5; level <= 5; ++level) {
            FilmRecipe recipe = film_default_recipe();
            recipe.profile = profile;
            recipe.yuv = yuv;
            for (int step = 0; step < abs(level); ++step)
                assert(film_darkroom_adjust(&recipe, rows[control], level > 0 ? 1 : -1));
            FilmDevelopGrade post = film_develop_grade_prepare(lut.samples ? &lut : NULL, &recipe);
            FilmRecipe before = recipe;
            before.profile = film_default_recipe().profile;
            FilmDevelopGrade pre = film_develop_grade_prepare(NULL, &before);
            for (unsigned code = 0; code <= 255; ++code) {
                FilmRgb input = {code / 255.f, code / 255.f, code / 255.f};
                FilmRgb a = film_develop_grade_pixel(input, &pre);
                FilmRgb b = film_develop_grade_pixel(input, &post);
                fprintf(csv, "%s,%d,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
                        names[control], level, code, a.r, a.g, a.b, b.r, b.g, b.b,
                        film_srgb_linear_luma(b));
            }
            source.recipe = recipe;
            FilmImage image = {0};
            assert(film_develop(&source, lut.samples ? &lut : NULL, &image) == FILM_OK);
            snprintf(path, sizeof path, "%s/%s_%+d.ppm", argv[1], names[control], level);
            ppm(path, &image);
            film_image_free(&image);
        }
    }
    assert(fclose(csv) == 0);
    film_frame_free(&source);
    film_lut_free(&lut);
    return 0;
}
