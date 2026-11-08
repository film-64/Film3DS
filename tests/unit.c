#include "debug.h"
#include "film.h"
#include "ui.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void close_to(float actual, float expected, float tolerance)
{
    if (fabsf(actual - expected) > tolerance) fprintf(stderr, "%g != %g\n", actual, expected);
    assert(isfinite(actual) && fabsf(actual - expected) <= tolerance);
}

static FILE *fixture_file(void)
{
    char name[] = "build/fixture.XXXXXX";
    int fd = mkstemp(name);
    assert(fd >= 0);
    assert(unlink(name) == 0);
    FILE *file = fdopen(fd, "w+b");
    assert(file);
    return file;
}

static FilmStatus load_text(const char *text, FilmLut *lut, unsigned *line)
{
    FILE *file = fixture_file();
    assert(file);
    fputs(text, file);
    rewind(file);
    FilmStatus status = film_lut_load(file, lut, line);
    fclose(file);
    return status;
}

static void parser_errors(void)
{
    FilmLut lut = {0};
    unsigned line;
    assert(load_text("LUT_1D_SIZE 17\n", &lut, &line) == FILM_UNSUPPORTED && line == 1);
    assert(load_text("LUT_3D_SIZE 32\n", &lut, NULL) == FILM_UNSUPPORTED);
    assert(load_text("LUT_3D_SIZE 17\n0 0 0\n", &lut, NULL) == FILM_CORRUPT);
    assert(load_text("LUT_3D_SIZE 17\n0 nan 0\n", &lut, &line) == FILM_CORRUPT && line == 2);
    assert(load_text("LUT_3D_SIZE 17\n0 1e100 0\n", &lut, NULL) == FILM_CORRUPT);
    assert(load_text("LUT_3D_SIZE 17\n0 0x1p0 0\n", &lut, NULL) == FILM_CORRUPT);
    assert(load_text("LUT_3D_SIZE 17\n0 0 0 extra\n", &lut, NULL) == FILM_CORRUPT);
    assert(load_text("LUT_3D_SIZE 17\nLUT_3D_SIZE 17\n", &lut, NULL) == FILM_CORRUPT);
    assert(load_text("DOMAIN_MIN 0 0 0\nLUT_3D_INPUT_RANGE 0 1\n", &lut, NULL) == FILM_CORRUPT);
    assert(load_text("TITLE \"broken\n", &lut, NULL) == FILM_CORRUPT);
    assert(!lut.samples);
}

static FilmLut create_lut(unsigned n, unsigned kind)
{
    FILE *file = fixture_file();
    assert(file);
    fprintf(file, "# synthetic\r\nTITLE \"test #1\" # comment\r\nLUT_3D_SIZE %u\n", n);
    if (kind == 0) fprintf(file, "DOMAIN_MIN -1 -1 -1\nDOMAIN_MAX 1 1 1\n");
    if (kind == 1) fprintf(file, "LUT_3D_INPUT_RANGE 0 1\n");
    for (unsigned b = 0; b < n; ++b)
        for (unsigned g = 0; g < n; ++g)
            for (unsigned r = 0; r < n; ++r) {
                float x = r / (float)(n - 1), y = g / (float)(n - 1), z = b / (float)(n - 1);
                if (kind == 1) fprintf(file, "%.9f %.9f %.9f\n", x*y, y*z, z*x);
                else fprintf(file, "%.9f %.9f %.9f\n", x, y, z);
            }
    rewind(file);
    FilmLut lut = {0};
    assert(film_lut_load(file, &lut, NULL) == FILM_OK);
    assert(lut.size == n);
    fclose(file);
    return lut;
}

static float product(float base_a, float a, float base_b, float b, float step)
{
    return base_a * base_b + step * (base_a * b + base_b * a) + step * step * fminf(a, b);
}

static void lut_math(void)
{
    const unsigned sizes[] = {17, 33, 65};
    const float fractions[][3] = {{.2f,.5f,.8f}, {.2f,.8f,.5f}, {.5f,.2f,.8f},
        {.5f,.8f,.2f}, {.8f,.2f,.5f}, {.8f,.5f,.2f}, {.5f,.5f,.5f}};
    for (unsigned i = 0; i < 3; ++i) {
        FilmLut nonlinear = create_lut(sizes[i], 1);
        float s = 1.0f / (sizes[i] - 1);
        for (unsigned j = 0; j < sizeof fractions / sizeof *fractions; ++j) {
            const float *f = fractions[j];
            FilmRgb result = film_lut_sample(&nonlinear, (FilmRgb){(2+f[0])*s, (3+f[1])*s, (4+f[2])*s});
            close_to(result.r, product(2*s, f[0], 3*s, f[1], s), 1e-6f);
            close_to(result.g, product(3*s, f[1], 4*s, f[2], s), 1e-6f);
            close_to(result.b, product(4*s, f[2], 2*s, f[0], s), 1e-6f);
        }
        FilmRgb edge = film_lut_sample(&nonlinear, (FilmRgb){2, 1, -1});
        assert(edge.r == 1 && edge.g == 0 && edge.b == 0);
        film_lut_free(&nonlinear);
    }
    FilmLut domain = create_lut(17, 0);
    FilmRgb c = film_lut_sample(&domain, (FilmRgb){-0.5f, 0, 0.5f});
    assert(c.r == .25f && c.g == .5f && c.b == .75f);
    film_lut_free(&domain);
    FilmLut identity = create_lut(65, 2);
    FilmProfile p = film_default_recipe().profile;
    p.input = p.output = FILM_LINEAR_SRGB;
    c = film_grade((FilmRgb){.25f,.5f,.75f}, &identity, &p);
    close_to(c.r, .25f, 1e-6f);
    close_to(c.g, .5f, 1e-6f);
    close_to(c.b, .75f, 1e-6f);
    p.input = p.output = FILM_REC709;
    c = film_grade((FilmRgb){.25f,.5f,.75f}, &identity, &p);
    close_to(c.r, .25f, 1e-6f);
    close_to(c.g, .5f, 1e-6f);
    close_to(c.b, .75f, 1e-6f);
    p.input = FILM_VENDOR_LOG;
    assert(film_profile_validate(&p) == FILM_UNSUPPORTED);
    film_lut_free(&identity);
}

static void hsl(void)
{
    FilmProfile p = film_default_recipe().profile;
    p.zones[0].hue = 120;
    FilmRgb c = film_grade((FilmRgb){1,0,0}, NULL, &p);
    close_to(c.g, 1, 1e-6f);
    close_to(c.r, 0, 1e-6f);
    c = film_grade((FilmRgb){0,1,0}, NULL, &p);
    assert(c.g == 1 && c.r == 0 && c.b == 0);
    // 345 degrees lies halfway between zones 11 and 0.
    c = film_grade((FilmRgb){1,0,.25f}, NULL, &p);
    close_to(c.g, .75f, 1e-6f);
    p.global.saturation = -1;
    c = film_grade((FilmRgb){1,0,0}, NULL, &p);
    assert(c.r == .5f && c.g == .5f && c.b == .5f);
    c = film_grade((FilmRgb){.3f,.3f,.3f}, NULL, &p);
    assert(c.r == .3f && c.g == .3f && c.b == .3f);
}

static void yuv(void)
{
    uint8_t bytes[] = {16,128,235,128, 81,90,81,240};
    FilmImage source = {4,1,FILM_YUYV,bytes}, converted = {0};
    assert(film_rgb24(&source, FILM_601_LIMITED, &converted) == FILM_OK);
    const uint8_t expected[] = {0,0,0,255,255,255,254,0,0,254,0,0};
    assert(!memcmp(converted.pixels, expected, sizeof expected));
    film_image_free(&converted);
    FilmRgb c = film_decode_pixel(&source, 1, 0, FILM_601_FULL);
    close_to(c.r, 235 / 255.0f, 1e-6f);
    c = film_decode_pixel(&source, 2, 0, FILM_709_LIMITED);
    assert(c.r == 1 && c.g > .09f && c.b < .01f);
    uint8_t packed[] = {0,248,224,7,31,0};
    source = (FilmImage){3,1,FILM_RGB565_LE,packed};
    assert(film_rgb24(&source, FILM_601_FULL, &converted) == FILM_OK);
    assert(!memcmp(converted.pixels, (uint8_t[]){255,0,0,0,255,0,0,0,255}, 9));
    film_image_free(&converted);
    assert(film_image_bytes(3, 2, FILM_YUYV) == 0);
}

static void resize_and_dimensions(void)
{
    uint8_t p[] = {0,0,0,100,100,100};
    FilmImage src = {2,1,FILM_RGB24,p}, out = {0};
    assert(film_resize(&src, 4, 1, FILM_BILINEAR, &out) == FILM_OK);
    assert(out.pixels[0] == 0 && out.pixels[3] == 25 && out.pixels[6] == 75 && out.pixels[9] == 100);
    film_image_free(&out);
    assert(film_resize(&src, 4, 1, FILM_BICUBIC, &out) == FILM_OK);
    assert(out.pixels[0] == 0 && out.pixels[3] == 20 && out.pixels[6] == 80 && out.pixels[9] == 107);
    film_image_free(&out);
    assert(film_resize(&src, 2, 1, FILM_BICUBIC, &out) == FILM_OK);
    assert(!memcmp(out.pixels, p, sizeof p));
    film_image_free(&out);

    const unsigned expected[][4][2] = {
        {{640,480},{639,426},{480,480},{640,360}},
        {{960,720},{960,640},{720,720},{960,540}},
        {{1280,960},{1278,852},{960,960},{1280,720}}
    };
    for (unsigned scale = 0; scale < 3; ++scale)
        for (unsigned aspect = 0; aspect < 4; ++aspect) {
            unsigned w, h;
            FilmRect rect;
            assert(film_scaled_dims(640, 480, scale, &w, &h) == FILM_OK);
            assert(film_crop_rect(w, h, aspect, &rect) == FILM_OK);
            assert(rect.width == expected[scale][aspect][0] && rect.height == expected[scale][aspect][1]);
        }
}

static uint8_t grain_pixel(uint32_t *seed)
{
    *seed = *seed * UINT32_C(1664525) + UINT32_C(1013904223);
    int a = *seed >> 24;
    *seed = *seed * UINT32_C(1664525) + UINT32_C(1013904223);
    int b = *seed >> 24;
    return (uint8_t)floorf(128 + (a + b - 255) * .25f + .5f);
}

static void pipeline(void)
{
    FilmFrame frame = {.recipe = film_default_recipe()};
    assert(film_image_alloc(&frame.source, 8, 6, FILM_RGB24) == FILM_OK);
    memset(frame.source.pixels, 128, 8 * 6 * 3);
    frame.recipe.scale = FILM_200;
    frame.recipe.aspect = FILM_16_9;
    frame.recipe.grain_amount = 1;
    frame.recipe.grain_seed = 42;
    FilmImage out = {0};
    assert(film_develop(&frame, NULL, &out) == FILM_OK);
    assert(out.width == 16 && out.height == 9);
    uint32_t seed = 42;
    for (unsigned i = 0; i < 16; ++i) (void)grain_pixel(&seed);
    for (unsigned i = 0; i < 16 * 9; ++i) {
        uint8_t expected = grain_pixel(&seed);
        assert(out.pixels[i * 3] == expected && out.pixels[i * 3 + 1] == expected &&
               out.pixels[i * 3 + 2] == expected);
    }
    film_image_free(&out);
    frame.recipe.profile.global.lightness = -.25f;
    frame.recipe.grain_amount = 0;
    assert(film_develop(&frame, NULL, &out) == FILM_OK);
    assert(out.pixels[0] == 64);
    film_image_free(&out);
    film_frame_free(&frame);
}

static void ui(void)
{
    FilmUi state = film_ui_new(0);
    assert(film_ui_key(&state, UI_ACCEPT) == UI_CAPTURE);
    film_ui_key(&state, UI_LEFT);
    assert(state.recipe.aspect == FILM_16_9);
    film_ui_touch(&state, 270, 113);
    assert(state.recipe.scale == FILM_150);
    film_ui_touch(&state, 150, 215);
    assert(state.page == FILM_ROLL && film_ui_key(&state, UI_ACCEPT) == UI_NONE);
    film_ui_add_photo(&state);
    film_ui_add_photo(&state);
    assert(state.selection == 1 && state.photos == 2);
    film_ui_key(&state, UI_RIGHT);
    assert(state.selection == 1);
    film_ui_key(&state, UI_LEFT);
    film_ui_key(&state, UI_ACCEPT);
    assert(state.selection == 0 && state.page == FILM_DARKROOM);
    assert(film_ui_touch(&state, 130, 170) == UI_EXPORT);
    film_ui_key(&state, UI_BACK);
    assert(state.page == FILM_VIEW);
}

int main(void)
{
    parser_errors(); puts("PASS CUBE parser errors");
    lut_math(); puts("PASS CUBE 17/33/65 and tetrahedra");
    hsl(); puts("PASS global HSL and wrapped hue zones");
    yuv(); puts("PASS YUYV matrices and RGB565");
    resize_and_dimensions(); puts("PASS bilinear, bicubic and 12 output sizes");
    pipeline(); puts("PASS pixel-level grain order and final crop");
    ui(); puts("PASS UI navigation, touch and roll selection");
}
