#include "debug.h"
#include "film.h"
#include "platform_camera.h"
#include "ui.h"

#include <assert.h>
#include <float.h>
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
    if (kind == 1 || kind == 3) fprintf(file, "LUT_3D_INPUT_RANGE 0 1\n");
    if (kind == 3) fprintf(file, "# FILM3DS_TRILINEAR\n");
    if (kind == 4) fprintf(file, "# FILM3DS_SHAPER_DARK_1_5\n");
    for (unsigned b = 0; b < n; ++b)
        for (unsigned g = 0; g < n; ++g)
            for (unsigned r = 0; r < n; ++r) {
                float x = r / (float)(n - 1), y = g / (float)(n - 1), z = b / (float)(n - 1);
                if (kind == 1 || kind == 3) fprintf(file, "%.9f %.9f %.9f\n", x*y, y*z, z*x);
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
    FilmLut shaped = create_lut(17, 4);
    assert(shaped.shadow_shaper);
    c = film_lut_sample(&shaped, (FilmRgb){.125f, 0, 1});
    close_to(c.r, .25f, 1e-6f);
    assert(c.g == 0 && c.b == 1);
    film_lut_free(&shaped);
    FilmLut trilinear = create_lut(17, 3);
    assert(trilinear.trilinear);
    c = film_lut_sample(&trilinear, (FilmRgb){.024f, .028f, .058f});
    close_to(c.r, .024f * .028f, 1e-6f);
    close_to(c.g, .028f * .058f, 1e-6f);
    close_to(c.b, .058f * .024f, 1e-6f);
    film_lut_free(&trilinear);
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
    p.input = FILM_FLOG2C;
    p.output = FILM_SRGB;
    assert(film_profile_validate(&p) == FILM_OK);
    c = film_grade((FilmRgb){0, 0, 0}, &identity, &p);
    close_to(c.r, 0.092864f, 1e-6f);
    c = film_grade((FilmRgb){.461356f, .461356f, .461356f}, &identity, &p);
    close_to(c.r, 400.0f / 1023, 0.0001f);
    close_to(c.g, c.r, 1e-6f);
    close_to(c.b, c.r, 1e-6f);
    c = film_grade((FilmRgb){1, 0, 0}, &identity, &p);
    close_to(c.r, .499100f, 1e-6f);
    close_to(c.g, .322007f, 1e-6f);
    close_to(c.b, .191368f, 1e-6f);
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

static void source_saturation(void)
{
    FilmFrame frame = {.recipe = film_default_recipe()};
    assert(film_image_alloc(&frame.source, 4, 3, FILM_YUYV) == FILM_OK);
    for (unsigned i = 0; i < 24; i += 4)
        memcpy(frame.source.pixels + i, (uint8_t[]){128, 92, 128, 173}, 4);
    frame.recipe.base_saturation = -1;
    char name[] = "build/source-saturation.XXXXXX";
    int fd = mkstemp(name);
    assert(fd >= 0 && close(fd) == 0);
    assert(film_frame_write(name, &frame) == FILM_OK);
    FilmFrame reopened = {0};
    assert(film_frame_read(name, &reopened) == FILM_OK && unlink(name) == 0);
    assert(reopened.recipe.base_saturation == -1);
    assert(!memcmp(frame.source.pixels, reopened.source.pixels, 24));
    FilmImage out = {0}, neutral = {0};
    frame.recipe.base_saturation = 0;
    assert(film_develop(&frame, NULL, &neutral) == FILM_OK);
    const FilmRgb baseline = film_decode_pixel(&neutral, 0, 0, FILM_601_FULL);
    float luma = film_srgb_linear_luma(baseline);
    for (int saturation = -1; saturation <= 1; saturation += 2) {
        reopened.recipe.base_saturation = saturation;
        assert(film_develop(&reopened, NULL, &out) == FILM_OK);
        FilmRgb pixel = film_decode_pixel(&out, 0, 0, FILM_601_FULL);
        close_to(film_srgb_linear_luma(pixel), luma, .004f);
        if (saturation < 0) assert(pixel.r == pixel.g && pixel.g == pixel.b);
        else assert(pixel.r > baseline.r && pixel.b < baseline.b);
        film_image_free(&out);
    }
    film_image_free(&neutral);
    film_frame_free(&reopened);
    film_frame_free(&frame);
}

static void camera_metering(void)
{
    uint8_t pixels[16 * 12 * 2];
    for (unsigned i = 0; i < 8 * 12; ++i) {
        uint8_t y = i % 8 >= 2 && i % 8 < 6 && i / 8 >= 3 && i / 8 < 9 ? 200 : 40;
        memcpy(pixels + 4 * i, (uint8_t[]){y, 128, y, 128}, 4);
    }
    FilmImage frame = {16, 12, FILM_YUYV, pixels};
    FilmCameraStability window = {0};
    FilmMeterReading average = film_camera_sample_preview(&window, &frame, FILM_601_FULL, FILM_4_3, FILM_METER_AVERAGE);
    FilmMeterReading center = film_camera_sample_preview(&window, &frame, FILM_601_FULL, FILM_4_3, FILM_METER_CENTER);
    float dark_code = 40 / 255.0f, bright_code = 200 / 255.0f;
    float edge = film_srgb_linear_luma((FilmRgb){dark_code, dark_code, dark_code});
    float middle = film_srgb_linear_luma((FilmRgb){bright_code, bright_code, bright_code});
    close_to(average.luma, .75f * edge + .25f * middle, 1e-6f);
    close_to(center.luma, (3 * edge + 4 * middle) / 7, 1e-6f);
    assert(!film_camera_stability_read(&window).ready);
    film_camera_stability_push(&window, (FilmCameraColorStats){40, 128, 128, center.luma});
    film_camera_stability_push(&window, (FilmCameraColorStats){40, 128, 128, center.luma});
    FilmCameraStabilityResult stable = film_camera_stability_read(&window);
    assert(stable.ready && stable.exposure && stable.white_balance && !stable.metering);
    film_camera_stability_push(&window, (FilmCameraColorStats){43, 130, 128, center.luma});
    stable = film_camera_stability_read(&window);
    assert(!stable.exposure && !stable.white_balance && stable.metering);
    FilmMeterReading square = film_camera_sample_preview(&window, &frame, FILM_601_FULL, FILM_1_1, FILM_METER_AVERAGE);
    close_to(square.luma, (2 * edge + middle) / 3, 1e-6f);

    FilmExposureMeter meter = {0};
    FilmMeterReading gray = {.18f, 0};
    assert(film_camera_meter_step(&meter, 0, gray, 0) == 0);
    assert(film_camera_meter_step(&meter, 0, gray, 1) == 1);
    meter = (FilmExposureMeter){0};
    assert(film_camera_meter_step(&meter, 0, gray, -1) == -1);
    meter = (FilmExposureMeter){0};
    assert(film_camera_meter_step(&meter, FILM_EXPOSURE_MAX, gray, 6) == FILM_EXPOSURE_MAX);
    assert(film_camera_meter_step(&meter, 0, (FilmMeterReading){.13f, 0}, 0) == 1);
    assert(film_camera_meter_step(&meter, 1, (FilmMeterReading){.35f, .03f}, 0) == 0);
    assert(film_camera_meter_step(&meter, 0, (FilmMeterReading){.13f, 0}, 0) == 0);
    assert(film_camera_meter_step(&meter, 0, (FilmMeterReading){.04f, 0}, 0) == 1);
}

static void darkroom_tone(void)
{
    FilmRgb samples[8];
    for (unsigned i = 0; i < 8; ++i) samples[i] = (FilmRgb){1.25f, 1.25f, 1.25f};
    FilmLut lut = {.size = 2, .max = {1, 1, 1}, .samples = samples};
    FilmRecipe recipe = film_default_recipe();
    FilmRgb gray = {.4f, .4f, .4f};
    const float levels[] = {0, .5f, 1};
    for (unsigned i = 0; i < 3; ++i) {
        recipe.profile.strength = levels[i];
        recipe.tone.value[FILM_TONE_EV] = -2;
        FilmDevelopGrade grade = film_develop_grade_prepare(&lut, &recipe);
        FilmRgb out = film_develop_grade_pixel(gray, &grade);
        float mixed = gray.r + (1.25f - gray.r) * levels[i];
        close_to(film_srgb_linear_luma(out),
                 .25f * film_srgb_linear_luma((FilmRgb){mixed, mixed, mixed}), 2e-6f);
    }
    recipe = film_default_recipe();
    recipe.tone.value[FILM_TONE_BLACK] = -.8f;
    recipe.tone.value[FILM_TONE_MID] = 1;
    recipe.tone.value[FILM_TONE_HIGH] = -1;
    recipe.tone.value[FILM_TONE_SHADOWS] = .8f;
    recipe.tone.value[FILM_TONE_SHOULDER] = .6f;
    FilmDevelopGrade grade = film_develop_grade_prepare(NULL, &recipe);
    float previous = 0;
    for (unsigned i = 0; i <= 200; ++i) {
        float step = i / 100.0f;
        FilmRgb out = film_develop_grade_pixel((FilmRgb){step, step, step}, &grade);
        assert(out.r >= previous && out.r <= 1);
        if (i > 100) assert(out.r > previous);
        previous = out.r;
    }

    recipe = film_default_recipe();
    FilmRgb color = {.86f, .37f, .08f};
    float y = film_srgb_linear_luma(color);
    for (int i = -1; i <= 1; ++i) {
        recipe.tone.value[FILM_TONE_SATURATION] = i;
        grade = film_develop_grade_prepare(NULL, &recipe);
        FilmRgb out = film_develop_grade_pixel(color, &grade);
        close_to(film_srgb_linear_luma(out), y, 2e-6f);
        if (i == -1) { close_to(out.r, out.g, 1e-6f); close_to(out.g, out.b, 1e-6f); }
        if (i == 1) assert(out.r > color.r && out.b >= -1e-6f && out.b < color.b);
    }
    for (unsigned i = 0; i < 8; ++i) samples[i] = (FilmRgb){FLT_MAX, -FLT_MAX, .5f};
    grade = film_develop_grade_prepare(&lut, &recipe);
    FilmRgb out = film_develop_grade_pixel(color, &grade);
    assert(isfinite(out.r) && isfinite(out.g) && isfinite(out.b));
}

static void darkroom_recipe(void)
{
    FilmFrame frame = {.recipe = film_default_recipe()};
    assert(film_image_alloc(&frame.source, 4, 3, FILM_YUYV) == FILM_OK);
    memset(frame.source.pixels, 128, 24);
    strcpy(frame.recipe.lut_id, "original.cube");
    frame.recipe.profile.strength = .35f;
    frame.recipe.profile.exposure = .25f;
    frame.recipe.profile.global.hue = 2;
    frame.recipe.profile.zones[11].lightness = -.1f;
    frame.recipe.tone.value[FILM_TONE_EV] = -.75f;
    frame.recipe.tone.value[FILM_TONE_SHOULDER] = .4f;
    frame.recipe.base_saturation = .2f;
    char name[] = "build/darkroom-recipe.XXXXXX";
    int fd = mkstemp(name);
    assert(fd >= 0 && close(fd) == 0);
    assert(film_frame_write(name, &frame) == FILM_OK);
    FilmFrame reopened = {0};
    assert(film_frame_read(name, &reopened) == FILM_OK && unlink(name) == 0);
    assert(!memcmp(frame.source.pixels, reopened.source.pixels, 24));
    assert(!memcmp(&frame.recipe.tone, &reopened.recipe.tone, sizeof(FilmTone)));
    assert(reopened.recipe.base_saturation == .2f && reopened.recipe.profile.strength == .35f);
    FilmRecipe edit = reopened.recipe;
    FilmProfile preset = {.input = FILM_REC709, .strength = .8f, .exposure = -.5f};
    film_ui_select_look(&edit, &reopened.recipe, "replacement.cube", &preset);
    assert(edit.profile.input == FILM_REC709 && edit.profile.strength == .8f);
    assert(edit.profile.exposure == .25f && edit.profile.global.hue == 2);
    assert(edit.profile.zones[11].lightness == -.1f && edit.tone.value[FILM_TONE_EV] == -.75f);
    film_ui_select_look(&edit, &reopened.recipe, "original.cube", &preset);
    assert(edit.profile.input == FILM_SRGB && edit.profile.strength == .35f);
    edit.tone.value[FILM_TONE_EV] = NAN;
    assert(film_recipe_validate(&edit) == FILM_INVALID);
    film_frame_free(&reopened);
    film_frame_free(&frame);
}

static void adaptive_grain(void)
{
    enum { WIDTH = 128, HEIGHT = 96 };
    FilmImage original = {0}, first = {0}, second = {0};
    assert(film_image_alloc(&original, WIDTH, HEIGHT, FILM_RGB24) == FILM_OK);
    const unsigned levels[] = {48, 128, 224};
    size_t bytes = WIDTH * HEIGHT * 3;
    for (unsigned y = 0; y < HEIGHT; ++y)
        memset(original.pixels + y * WIDTH * 3, levels[y / (HEIGHT / 3)], WIDTH * 3);
    assert(film_resize(&original, WIDTH, HEIGHT, FILM_BILINEAR, &first) == FILM_OK);
    assert(film_resize(&original, WIDTH, HEIGHT, FILM_BILINEAR, &second) == FILM_OK);
    film_grain_adaptive(&first, 42, 0, FILM_NATIVE, FILM_GRAIN_MEDIUM);
    assert(!memcmp(first.pixels, original.pixels, bytes));
    film_grain_adaptive(&first, 42, .5f, FILM_NATIVE, FILM_GRAIN_MEDIUM);
    film_grain_adaptive(&second, 42, .5f, FILM_NATIVE, FILM_GRAIN_MEDIUM);
    assert(!memcmp(first.pixels, second.pixels, bytes));
    unsigned changed[3] = {0};
    for (unsigned y = 0; y < HEIGHT; ++y)
        for (unsigned x = 0; x < WIDTH; ++x) {
            const uint8_t *p = first.pixels + ((size_t)y * WIDTH + x) * 3;
            unsigned band = y / (HEIGHT / 3);
            assert(p[0] == p[1] && p[1] == p[2]);
            changed[band] += p[0] != levels[band];
        }
    assert(changed[0] > changed[1] && changed[1] > changed[2] && changed[2] > 0);
    memcpy(first.pixels, original.pixels, bytes);
    memcpy(second.pixels, original.pixels, bytes);
    film_grain_adaptive(&first, 42, .5f, FILM_NATIVE, FILM_GRAIN_FINE);
    film_grain_adaptive(&second, 42, .5f, FILM_NATIVE, FILM_GRAIN_COARSE);
    assert(memcmp(first.pixels, second.pixels, bytes));
    film_image_free(&second);
    film_image_free(&first);
    film_image_free(&original);
}

static void camera_color(void)
{
    FILE *file = fixture_file();
    fputs("0 0 1\n0 1 0\n1 0 0\n", file);
    rewind(file);
    FilmCameraColor matrix;
    assert(film_camera_color_load(file, &matrix) == FILM_OK);
    fclose(file);
    FilmRgb mapped = film_camera_color_apply((FilmRgb){.1f, .3f, .7f}, &matrix);
    close_to(mapped.r, .7f, 1e-6f);
    close_to(mapped.g, .3f, 1e-6f);
    close_to(mapped.b, .1f, 1e-6f);

    FilmFrame frame = {.recipe = film_default_recipe()};
    assert(film_image_alloc(&frame.source, 4, 4, FILM_RGB24) == FILM_OK);
    for (unsigned i = 0; i < 48; i += 3) {
        frame.source.pixels[i] = 11;
        frame.source.pixels[i + 1] = 103;
        frame.source.pixels[i + 2] = 202;
    }
    frame.recipe.scale = FILM_200;
    const FilmDevelopOptions options = {.camera_color = &matrix, .adaptive_grain = true, .grain = FILM_GRAIN_FINE};
    FilmImage out = {0};
    assert(film_develop_with_options(&frame, NULL, &options, &out) == FILM_OK);
    assert(out.width == 8 && out.height == 6);
    for (unsigned i = 0; i < out.width * out.height * 3; i += 3) {
        assert(out.pixels[i] == 202 && out.pixels[i + 1] == 103 && out.pixels[i + 2] == 11);
    }
    film_image_free(&out);
    film_frame_free(&frame);
}

static void measured_grain(void)
{
    FILE *file = fixture_file();
    fputs("F3G1\n", file);
    for (unsigned i = 0; i < 9; ++i) fprintf(file, "%u 4 .30\n", 16 + i * 28);
    rewind(file);
    FilmGrainProfile profile;
    assert(film_grain_profile_load(file, &profile) == FILM_OK);
    fclose(file);
    FilmImage img = {0};
    assert(film_image_alloc(&img, 256, 128, FILM_RGB24) == FILM_OK);
    memset(img.pixels, 128, img.width * img.height * 3);
    assert(film_grain_measured(&img, 42, 1, &profile) == FILM_OK);
    double variance = 0, adjacent = 0;
    for (unsigned y = 0; y < img.height; ++y)
        for (unsigned x = 0; x < img.width; ++x) {
            const uint8_t *p = img.pixels + ((size_t)y * img.width + x) * 3;
            int delta = (int)p[0] - 128;
            variance += delta * delta;
            if (x) adjacent += delta * ((int)p[-3] - 128);
            assert(p[0] == p[1] && p[1] == p[2]);
        }
    close_to(sqrt(variance / (img.width * img.height)), 4, .25f);
    close_to(adjacent / variance, .3f, .035f);
    film_image_free(&img);

    const char *const paths[] = {"romfs/grain/fine.f3g", "romfs/grain/medium.f3g", "romfs/grain/coarse.f3g"};
    float previous_correlation = 0;
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        file = fopen(paths[i], "r");
        assert(file && film_grain_profile_load(file, &profile) == FILM_OK);
        fclose(file);
        assert(profile.correlation[4] > previous_correlation);
        assert(profile.rms[4] > profile.rms[0] && profile.rms[4] > profile.rms[8]);
        previous_correlation = profile.correlation[4];
    }
}

static void halation(void)
{
    FilmFrame frame = {.recipe = film_default_recipe()};
    assert(film_image_alloc(&frame.source, 48, 36, FILM_RGB24) == FILM_OK);
    memset(frame.source.pixels, 24, 48 * 36 * 3);
    for (unsigned y = 16; y < 20; ++y)
        memset(frame.source.pixels + (y * 48 + 20) * 3, 255, 4 * 3);
    FilmImage image = {0};
    const FilmDevelopOptions options = {.halation = true};
    assert(film_develop_with_options(&frame, NULL, &options, &image) == FILM_OK);
    assert(image.pixels[0] == 24);
    const uint8_t *near = image.pixels + (18 * 48 + 24) * 3;
    assert(near[0] > near[1] && near[1] >= near[2] && near[2] >= 24);
    film_image_free(&image);
    film_frame_free(&frame);
}

static void deferred_quantization(void)
{
    FilmFrame frame = {.recipe = film_default_recipe()};
    assert(film_image_alloc(&frame.source, 4, 3, FILM_RGB24) == FILM_OK);
    for (unsigned y = 0; y < 3; ++y)
        for (unsigned x = 0; x < 4; ++x)
            memset(frame.source.pixels + (y * 4 + x) * 3, x < 2 ? 0 : 255, 3);
    FilmLut lut = create_lut(17, 2);
    for (unsigned i = 0; i < 17 * 17 * 17; ++i) {
        float value = .2f + .002f * lut.samples[i].r;
        lut.samples[i] = (FilmRgb){value, value, value};
    }
    frame.recipe.scale = FILM_200;
    FilmImage legacy = {0}, precise = {0};
    const FilmDevelopOptions options = {0};
    assert(film_develop(&frame, &lut, &legacy) == FILM_OK);
    assert(film_develop_with_options(&frame, &lut, &options, &precise) == FILM_OK);
    assert(legacy.pixels[4 * 3] == 52 && precise.pixels[4 * 3] == 51);
    film_image_free(&precise);
    film_image_free(&legacy);
    film_lut_free(&lut);
    film_frame_free(&frame);
}

static void ui(void)
{
    FilmUi state = film_ui_new(0);
    assert(film_ui_key(&state, UI_ACCEPT) == UI_CAPTURE);
    film_ui_key(&state, UI_LEFT);
    assert(state.recipe.aspect == FILM_16_9);
    film_ui_touch(&state, 270, 113);
    assert(state.recipe.scale == FILM_200);
    film_ui_key(&state, UI_DOWN);
    assert(state.recipe.scale == FILM_NATIVE);
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
    source_saturation(); puts("PASS v2 source saturation, unchanged YUYV and neutral development");
    camera_metering(); puts("PASS camera metering weights, EV compensation, hysteresis and stability");
    darkroom_tone(); puts("PASS post-LUT EV, monotonic tone, shoulder and luminance-preserving saturation");
    darkroom_recipe(); puts("PASS v3 recipe round trip and saved-look restoration");
    adaptive_grain(); puts("PASS grain density follows luminance, seed is repeatable, zero is exact");
    camera_color(); puts("PASS camera input matrix and high-precision export");
    measured_grain(); puts("PASS measured grain tracks RMS and spatial correlation");
    halation(); puts("PASS highlight halation is warm and local");
    deferred_quantization(); puts("PASS darkroom resize keeps sub-code color precision");
    ui(); puts("PASS UI navigation, touch and roll selection");
}
