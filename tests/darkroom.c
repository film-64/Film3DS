#include "darkroom.h"
#include "ui.h"
#include "film_runtime_contract.h"

#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned files(const char *path)
{
    DIR *directory = opendir(path);
    assert(directory);
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) if (entry->d_name[0] != '.') ++count;
    closedir(directory);
    return count;
}

static void equal_files(const char *a, const char *b)
{
    FILE *left = fopen(a, "rb"), *right = fopen(b, "rb");
    assert(left && right);
    int x, y;
    do { x = fgetc(left); y = fgetc(right); assert(x == y); } while (x != EOF);
    assert(!ferror(left) && !ferror(right));
    fclose(left); fclose(right);
}

static void switching(void)
{
    FilmRecipe saved = film_default_recipe(), current = saved;
    strcpy(saved.lut_id, "old.cube");
    saved.profile.exposure = 2;
    saved.profile.global.hue = 30;
    saved.profile.strength = .9f;
    current.profile.exposure = 0; /* A deliberate zero must not inherit saved exposure. */
    current.profile.strength = .4f;
    current.input_tone.value[FILM_TONE_EV] = -.5f;
    current.input_tone.value[FILM_TONE_SHADOWS] = .6f;
    current.output_tone.value[FILM_TONE_HIGH] = -.8f;
    current.tone.value[FILM_TONE_SATURATION] = .25f;
    current.sharpness = 3;
    current.grain_amount = .3f;
    current.aspect = FILM_1_1;
    current.scale = FILM_200;
    FilmRecipe before = current;
    FilmProfile asset = {.input = FILM_FLOG2C, .output = FILM_REC709, .strength = 1, .exposure = -2};
    film_ui_select_look(&current, &saved, "new.cube", &asset);
    assert(current.profile.input == FILM_FLOG2C && current.profile.output == FILM_REC709);
    assert(current.profile.strength == .4f && current.profile.exposure == 0 && current.profile.global.hue == 0);
    assert(!memcmp(&current.input_tone, &before.input_tone, sizeof(FilmTone)));
    assert(!memcmp(&current.output_tone, &before.output_tone, sizeof(FilmTone)));
    assert(!memcmp(&current.tone, &before.tone, sizeof(FilmTone)));
    assert(current.sharpness == 3 && current.grain_amount == .3f && current.aspect == FILM_1_1 && current.scale == FILM_200);
    film_ui_select_look(&current, &saved, "old.cube", &saved.profile);
    assert(!strcmp(current.lut_id, "old.cube"));
    assert(current.profile.exposure == 0 && current.profile.global.hue == 0 && current.profile.strength == .4f);
    assert(!memcmp(&current.input_tone, &before.input_tone, sizeof(FilmTone)));
    assert(!memcmp(&current.output_tone, &before.output_tone, sizeof(FilmTone)));
}

static void controls(void)
{
    FilmDarkroom room = {0};
    film_darkroom_move(&room, -1);
    assert(room.row == DARKROOM_ROW_COUNT - 1 && room.first_row == DARKROOM_ROW_COUNT - FILM_DARKROOM_VISIBLE_ROWS);
    film_darkroom_move(&room, 1);
    assert(room.row == DARKROOM_ASPECT && room.first_row == 0);
    FilmRecipe recipe = film_default_recipe();
    const FilmDarkroomRow rows[] = {DARKROOM_HIGHLIGHTS, DARKROOM_MIDTONES, DARKROOM_SHADOWS};
    const FilmToneControl controls[] = {FILM_TONE_HIGH, FILM_TONE_MID, FILM_TONE_SHADOWS};
    for (unsigned j = 0; j < 3; ++j) {
        for (int i = 1; i <= 8; ++i) {
            assert(film_darkroom_adjust(&recipe, rows[j], 1));
            assert(fabsf(recipe.output_tone.value[controls[j]] - fminf(i, 5) / 5) < 1e-6f);
        }
        for (int i = 1; i <= 12; ++i) {
            film_darkroom_adjust(&recipe, rows[j], -1);
            assert(fabsf(recipe.output_tone.value[controls[j]] - fmaxf(5 - i, -5) / 5) < 1e-6f);
        }
    }
    for (int i = 0; i < 20; ++i) film_darkroom_adjust(&recipe, DARKROOM_EXPOSURE, 1);
    assert(recipe.input_tone.value[FILM_TONE_EV] == 4);
    assert(recipe.output_tone.value[FILM_TONE_HIGH] == -1);
    assert(film_darkroom_adjust(&recipe, DARKROOM_SIZE, 1) && recipe.scale == FILM_150);
    assert(film_darkroom_adjust(&recipe, DARKROOM_ASPECT, -1) && recipe.aspect == FILM_16_9);
    assert(film_recipe_validate(&recipe) == FILM_OK);
}

static void input_tone(void)
{
    FilmRecipe recipe = film_default_recipe();
    for (int h = -5; h <= 5; ++h) for (int m = -5; m <= 5; ++m) for (int s = -5; s <= 5; ++s) {
        recipe.output_tone.value[FILM_TONE_HIGH] = h / 5.0f;
        recipe.output_tone.value[FILM_TONE_MID] = m / 5.0f;
        recipe.output_tone.value[FILM_TONE_SHADOWS] = s / 5.0f;
        FilmDevelopGrade grade = film_develop_grade_prepare(NULL, &recipe);
        float previous = -1;
        for (unsigned i = 0; i <= 256; ++i) {
            float code = i / 256.0f;
            FilmRgb out = film_develop_grade_pixel((FilmRgb){code, code, code}, &grade);
            assert(isfinite(out.r) && out.r >= previous - 1e-6f && out.r <= 1.000001f);
            previous = out.r;
            if (!h && !m && !s) assert(fabsf(out.r - code) < 1e-6f);
        }
    }
    /* Observe actual pixels: the gain precedes nonlinear LUT sampling. */
    FilmLut lut = {.size = 17, .max = {1, 1, 1}};
    lut.samples = malloc(17 * 17 * 17 * sizeof(FilmRgb));
    assert(lut.samples);
    for (unsigned b = 0; b < 17; ++b) for (unsigned g = 0; g < 17; ++g) for (unsigned r = 0; r < 17; ++r)
        lut.samples[r + 17 * g + 289 * b] = (FilmRgb){r*r/256.0f, g*g/256.0f, b*b/256.0f};
    recipe = film_default_recipe();
    recipe.input_tone.value[FILM_TONE_EV] = 1;
    FilmDevelopGrade pre = film_develop_grade_prepare(NULL, &recipe);
    FilmDevelopGrade full = film_develop_grade_prepare(&lut, &recipe);
    FilmRgb input = {.25f, .25f, .25f};
    FilmRgb adjusted = film_develop_grade_pixel(input, &pre);
    FilmRgb expected = film_lut_sample(&lut, adjusted);
    FilmRgb actual = film_develop_grade_pixel(input, &full);
    assert(fabsf(actual.r - expected.r) < 1e-6f);
    recipe = film_default_recipe();
    recipe.tone.value[FILM_TONE_EV] = 1;
    FilmDevelopGrade post = film_develop_grade_prepare(&lut, &recipe);
    assert(fabsf(actual.r - film_develop_grade_pixel(input, &post).r) > .01f);
    film_lut_free(&lut);
}

static void sharpness(void)
{
    FilmImage image = {0};
    assert(film_image_alloc(&image, 8, 6, FILM_RGB24) == FILM_OK);
    uint8_t original[8 * 6 * 3];
    memset(image.pixels, 123, sizeof original);
    for (int level = -5; level <= 5; ++level) {
        assert(film_sharpen(&image, level) == FILM_OK);
        for (unsigned i = 0; i < sizeof original; ++i) assert(image.pixels[i] == 123);
    }
    for (unsigned y = 0; y < 6; ++y) for (unsigned x = 0; x < 8; ++x)
        memset(original + (y * 8 + x) * 3, x < 4 ? 80 : 180, 3);
    memcpy(image.pixels, original, sizeof original);
    assert(film_sharpen(&image, 0) == FILM_OK && !memcmp(original, image.pixels, sizeof original));
    assert(film_sharpen(&image, 5) == FILM_OK);
    assert(image.pixels[3 * 3] < 80 && image.pixels[4 * 3] > 180);
    memcpy(image.pixels, original, sizeof original);
    assert(film_sharpen(&image, -5) == FILM_OK);
    assert(image.pixels[3 * 3] > 80 && image.pixels[4 * 3] < 180);
    assert(film_sharpen(&image, 6) == FILM_INVALID);
    film_image_free(&image);
}

static void output_curve_response(void)
{
    FilmRgb samples[8];
    FilmLut lut = {.size = 2, .max = {1, 1, 1}, .samples = samples};
    const FilmToneControl controls[] = {FILM_TONE_HIGH, FILM_TONE_SHADOWS, FILM_TONE_MID};
    const float centers[] = {.82f, .055f, .5f};
    for (unsigned c = 0; c < 3; ++c) {
        for (unsigned i = 0; i < 8; ++i) samples[i] = (FilmRgb){centers[c], centers[c], centers[c]};
        float previous = c == 1 ? 2 : -1;
        int previous_code = -1;
        for (int level = -5; level <= 5; ++level) {
            FilmRecipe recipe = film_default_recipe();
            recipe.output_tone.value[controls[c]] = level / 5.f;
            FilmDevelopGrade grade = film_develop_grade_prepare(&lut, &recipe);
            FilmRgb out = film_develop_grade_pixel((FilmRgb){.3f, .3f, .3f}, &grade);
            assert(c == 1 ? out.r < previous : out.r > previous);
            int code = (int)floorf(out.r * 255 + .5f);
            assert(code != previous_code); /* Even a constant LUT cannot swallow the user control. */
            if (!level) assert(fabsf(out.r - centers[c]) < 1e-6f);
            if (grade.output_active)
                for (unsigned i = 0; i < 3; ++i) {
                    const float *a = grade.output_curve[i], *b = grade.output_curve[i + 1];
                    assert(fabsf(a[1] + 2*a[2] + 3*a[3] - b[1]) < 1e-6f);
                }
            previous = out.r; previous_code = code;
        }
    }
    FilmRecipe recipe = film_default_recipe();
    recipe.output_tone.value[FILM_TONE_HIGH] = -1;
    FilmDevelopGrade grade = film_develop_grade_prepare(NULL, &recipe);
    FilmRgb white = film_develop_grade_pixel((FilmRgb){1, 1, 1}, &grade);
    assert(fabsf(white.r - .9f) < 1e-6f);
    recipe.output_tone = (FilmTone){0};
    recipe.output_tone.value[FILM_TONE_SHADOWS] = -1;
    grade = film_develop_grade_prepare(NULL, &recipe);
    FilmRgb black = film_develop_grade_pixel((FilmRgb){0, 0, 0}, &grade);
    assert(fabsf(black.r - .06f) < 1e-6f);
    recipe.output_tone.value[FILM_TONE_EV] = 1;
    assert(film_recipe_validate(&recipe) == FILM_INVALID);
    /* Turning any control up must not move any gray value in the opposite
     * direction, including outside its main band and with other controls set. */
    for (unsigned c = 0; c < 3; ++c)
        for (int a = -1; a <= 1; ++a) for (int b = -1; b <= 1; ++b) {
            FilmDevelopGrade grades[11];
            for (int level = -5; level <= 5; ++level) {
                recipe = film_default_recipe();
                recipe.output_tone.value[controls[c]] = level / 5.f;
                recipe.output_tone.value[controls[(c+1)%3]] = a;
                recipe.output_tone.value[controls[(c+2)%3]] = b;
                grades[level+5] = film_develop_grade_prepare(NULL, &recipe);
            }
            for (unsigned code = 0; code <= 255; ++code) {
                float previous = c == 1 ? 2 : -1;
                for (unsigned level = 0; level < 11; ++level) {
                    float x = code / 255.f;
                    FilmRgb out = film_develop_grade_pixel((FilmRgb){x,x,x}, &grades[level]);
                    assert(c == 1 ? out.r <= previous + 2e-6f : out.r >= previous - 2e-6f);
                    previous = out.r;
                }
            }
        }
}

static void preview_and_storage(void)
{
    char directory[] = "build/darkroom-test.XXXXXX";
    int temporary = mkstemp(directory);
    assert(temporary >= 0 && close(temporary) == 0 && unlink(directory) == 0 && mkdir(directory, 0700) == 0);
    char saved[160], expected[160], source[160];
    snprintf(saved, sizeof saved, "%s/saved.jpg", directory);
    snprintf(expected, sizeof expected, "%s/expected.jpg", directory);
    snprintf(source, sizeof source, "%s/source.f3s", directory);
    FilmFrame frame = {.recipe = film_default_recipe()};
    assert(film_image_alloc(&frame.source, 8, 6, FILM_RGB24) == FILM_OK);
    for (unsigned i = 0; i < 8 * 6 * 3; ++i) frame.source.pixels[i] = (i * 7) % 256;
    frame.recipe.sharpness = 2;
    frame.recipe.input_tone.value[FILM_TONE_MID] = .4f;
    frame.recipe.tone.value[FILM_TONE_EV] = -.5f; /* Legacy and new adjustments round-trip separately. */
    assert(film_frame_write(source, &frame) == FILM_OK);
    FilmFrame reopened = {0};
    assert(film_frame_read(source, &reopened) == FILM_OK);
    assert(reopened.recipe.sharpness == 2);
    assert(!memcmp(&frame.recipe.input_tone, &reopened.recipe.input_tone, sizeof(FilmTone)));
    assert(reopened.recipe.tone.value[FILM_TONE_EV] == -.5f);
    assert(!memcmp(frame.source.pixels, reopened.source.pixels, 8 * 6 * 3));
    for (unsigned i = 0; i < FILM_TONE_COUNT; ++i) assert(reopened.recipe.output_tone.value[i] == 0);
    frame.recipe.output_tone.value[FILM_TONE_HIGH] = -.4f;
    frame.recipe.output_tone.value[FILM_TONE_MID] = .2f;
    frame.recipe.output_tone.value[FILM_TONE_SHADOWS] = .8f;
    assert(film_frame_write(source, &frame) == FILM_OK);
    film_frame_free(&reopened);
    assert(film_frame_read(source, &reopened) == FILM_OK);
    assert(!memcmp(&frame.recipe.output_tone, &reopened.recipe.output_tone, sizeof(FilmTone)));
    assert(!memcmp(&frame.recipe.input_tone, &reopened.recipe.input_tone, sizeof(FilmTone)));
    assert(reopened.recipe.sharpness == 2 && reopened.recipe.tone.value[FILM_TONE_EV] == -.5f);
    FilmDarkroom room = {0};
    assert(film_darkroom_finish(&room, true, saved) == FILM_STATE);
    const uint32_t select_key = UINT32_C(1) << 2;
    for (unsigned key = 0; key < 32; ++key) {
        uint32_t pressed = UINT32_C(1) << key;
        if (pressed == select_key) continue;
        assert(film_darkroom_preview(&room, &frame, NULL, NULL) == FILM_OK);
        assert(files(directory) == 1 && access(saved, F_OK) != 0);
        assert(film_darkroom_input(&room, 0, select_key) == DARKROOM_INPUT_NONE);
        assert(room.preview.pixels);
        assert(film_darkroom_input(&room, pressed, select_key) == DARKROOM_INPUT_CONTINUE);
        assert(!room.preview.pixels && files(directory) == 1);
    }
    assert(film_darkroom_preview(&room, &frame, NULL, NULL) == FILM_OK);
    assert(film_darkroom_input(&room, select_key | 1, select_key) == DARKROOM_INPUT_CONTINUE);
    assert(!room.preview.pixels && files(directory) == 1);
    assert(film_darkroom_preview(&room, &frame, NULL, NULL) == FILM_OK);
    assert(film_darkroom_input(&room, select_key, select_key) == DARKROOM_INPUT_SAVE && room.preview.pixels);
    assert(film_jpeg_write(expected, &room.preview, room.quality) == FILM_OK);
    memset(frame.source.pixels, 0, 8 * 6 * 3);
    frame.recipe.jpeg_quality = 1;
    frame.recipe.scale = FILM_200;
    assert(film_darkroom_finish(&room, true, saved) == FILM_OK);
    equal_files(saved, expected); /* Save exactly the rendered image, without rerendering. */
    assert(!room.preview.pixels && film_darkroom_finish(&room, true, saved) == FILM_STATE);
    assert(film_darkroom_preview(&room, &frame, NULL, NULL) == FILM_OK);
    assert(film_darkroom_finish(&room, true, saved) == FILM_STATE);
    equal_files(saved, expected); /* Existing file must not be overwritten. */
    assert(film_darkroom_preview(&room, &frame, NULL, NULL) == FILM_OK);
    strcpy(frame.recipe.lut_id, "missing.cube");
    assert(film_darkroom_preview(&room, &frame, NULL, NULL) == FILM_STATE);
    assert(!room.preview.pixels); /* Failed render must not leave a stale saveable preview. */
    film_frame_free(&frame); film_frame_free(&reopened);
    assert(unlink(saved) == 0 && unlink(expected) == 0 && unlink(source) == 0 && rmdir(directory) == 0);
}

int main(void)
{
    assert(film_runtime_source_valid("film-darkroom/v1"));
    assert(film_runtime_source_valid("film-darkroom-save/v1"));
    assert(!film_runtime_source_valid("film-darkroom-unknown/v1"));
    assert(!film_runtime_source_valid("scheduler_decision:admitted"));
    switching(); puts("PASS film simulation preserves current controls, including explicit zero and return to saved look");
    controls(); puts("PASS one-list navigation and 11 curve levels, separate exposure, crop and output size");
    input_tone(); puts("PASS 1331 monotonic output curves, zero identity, and nonlinear pre-LUT exposure order");
    output_curve_response(); puts("PASS post-LUT H/M/S survive a constant LUT, 11 distinct levels, C1 joins and adjustable endpoints");
    sharpness(); puts("PASS offline sharpen/soften, constant preservation and exact zero bypass");
    preview_and_storage(); puts("PASS v4/v5 roundtrip, memory-only preview, cancel/no writes, cached SELECT save and stale-preview rejection");
    return 0;
}
