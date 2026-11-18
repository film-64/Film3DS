#include "debug.h"
#include "platform_camera.h"
#include "platform_develop.h"
#include "platform_qa.h"
#include "ui.h"

#include <3ds.h>
#include <citro2d.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define ROOT "sdmc:/3ds/Film3DS/"
#define ROLLS ROOT "rolls/"
#define EXPORTS ROOT "exports/"
#define LUTS ROOT "luts/"
#define PACKED_LUTS "romfs:/luts/"
#define VIRTUAL_PHOTOS 3u
#define MAX_PHOTOS 9999u
#define MAX_LUTS 64u

enum { PREVIEW_TEX_SIDE = 256, PREVIEW_TEX_ROWS = 192 };

typedef struct {
    const char *directory;
    char name[sizeof ((FilmRecipe *)0)->lut_id];
} Look;

typedef struct {
    Thread thread;
    Handle completion;
    FilmFrame frame;
    const Look *look;
    const char *capture_mode;
    unsigned id;
    uint64_t capture_ms, save_ms;
    FilmStatus status, profile_status;
    bool written;
} CaptureSave;

typedef struct {
    FilmUi ui;
    FilmRecipe darkroom, saved_recipe;
    unsigned darkroom_id;
    bool darkroom_ready;
    const Look *capture_look;
    float capture_strength;
    FilmCamera camera;
    FilmCameraAutoState camera_auto;
    FilmYuv camera_yuv;
    C3D_Tex texture;
    unsigned image_width, image_height;
    FilmAspect saved_aspect;
    C2D_TextBuf text;
    FILE *log;
    uint16_t photos[MAX_PHOTOS];
    unsigned count, next_number;
    CaptureSave save;
    Look luts[MAX_LUTS];
    unsigned lut_count;
    char status[96];
    char y2r_status[64];
    const char *probe_title;
    char probe_rows[6][72];
    bool live;
    bool preview_failed;
    bool preview_single;
    bool camera_controls;
    enum { DARKROOM_LUT, DARKROOM_GRAIN, DARKROOM_TONE } darkroom_panel;
    FilmToneControl tone_control;
    bool halation;
    FilmGrain grain;
    bool grain_reference[FILM_GRAIN_COUNT];
    unsigned control;
    int camera_values[FILM_CAM_CONTROL_COUNT];
    FilmCameraStability stability;
    FilmExposureMeter meter;
    FilmMeterReading meter_reading;
    uint64_t meter_after;
    int applied_exposure;
    bool meter_failed;
    uint64_t preview_period;
    unsigned preview_frames, preview_fps;
} App;

static App app;
static const char *const aspect_names[] = {"4:3", "3:2", "1:1", "16:9"};
static const char *const scale_names[] = {"Native", "1.5x", "2x"};
static const char *const sensors[] = {"SIM", "REAR1", "REAR2", "FRONT"};
static const char *const grain_names[] = {"Fine", "Medium", "Coarse"};
static const char *const grain_stock_names[] = {"PAN F+ 50", "Pan 400", "Delta 3200"};
static const char *const grain_paths[] = {"romfs:/grain/fine.f3g", "romfs:/grain/medium.f3g", "romfs:/grain/coarse.f3g"};
static const Look legacy_look = {ROOT, "look.cube"};
static const char *const control_names[] = {
    [FILM_CAM_EXPOSURE] = "Exposure step", [FILM_CAM_METERING] = "Metering",
    [FILM_CAM_EV_COMP] = "Exposure compensation", [FILM_CAM_WB] = "White balance",
    [FILM_CAM_CONTRAST] = "Contrast", [FILM_CAM_SATURATION] = "Saturation",
    [FILM_CAM_SHARPNESS] = "Sharpness step"
};
static const char *const tone_names[] = {
    "Post-LUT EV", "Black point", "Midtones", "Highlights", "Shadows", "Shoulder", "Saturation"
};
static void update_status(const char *operation, FilmStatus status, uint32_t result);

static void reset_meter(void)
{
    app.stability = (FilmCameraStability){0};
    app.meter = (FilmExposureMeter){0};
    app.meter_after = osGetTime() + 350;
    app.meter_failed = false;
}

static void reset_preview_tracking(void)
{
    app.preview_period = 0;
    app.preview_frames = app.preview_fps = 0;
    reset_meter();
}

static void adjust_grain(int step)
{
    app.darkroom.grain_amount = fminf(1, fmaxf(0, app.darkroom.grain_amount + step / 100.0f));
}

static void select_grain(int step)
{
    app.grain = (app.grain + FILM_GRAIN_COUNT + step) % FILM_GRAIN_COUNT;
}

static FilmStatus load_camera_color(uint32_t sensor, FilmCameraColor *color, const FilmCameraColor **selected)
{
    static const char *const paths[] = {NULL, ROOT "rear1.rgbmat", ROOT "rear2.rgbmat", ROOT "front.rgbmat"};
    if (!sensor || sensor >= sizeof paths / sizeof paths[0]) return FILM_OK;
    FILE *file = fopen(paths[sensor], "r");
    if (!file) return errno == ENOENT ? FILM_OK : FILM_IO;
    FilmStatus status = film_camera_color_load(file, color);
    fclose(file);
    if (status == FILM_OK) *selected = color;
    return status;
}

static FilmStatus apply_exposure(int step)
{
    if (step == app.applied_exposure) return FILM_OK;
    FilmStatus status = film_camera_preview_stop(&app.camera);
    if (status == FILM_OK) status = film_camera_set_control(&app.camera, FILM_CAM_EXPOSURE, step);
    if (status == FILM_OK) {
        app.applied_exposure = step;
        app.stability = (FilmCameraStability){0};
        app.meter_after = osGetTime() + 350;
    }
    return status;
}

static void adjust_camera(int change)
{
    static const int minimum[FILM_CAM_CONTROL_COUNT] = {
        [FILM_CAM_EXPOSURE] = FILM_EXPOSURE_MIN, [FILM_CAM_EV_COMP] = -6,
        [FILM_CAM_CONTRAST] = -1, [FILM_CAM_SATURATION] = -10, [FILM_CAM_SHARPNESS] = FILM_SHARPNESS_MIN
    };
    static const int maximum[FILM_CAM_CONTROL_COUNT] = {
        [FILM_CAM_EXPOSURE] = FILM_EXPOSURE_MAX, [FILM_CAM_METERING] = FILM_METER_CENTER,
        [FILM_CAM_EV_COMP] = 6,
        [FILM_CAM_WB] = 5, [FILM_CAM_CONTRAST] = 1, [FILM_CAM_SATURATION] = 10,
        [FILM_CAM_SHARPNESS] = FILM_SHARPNESS_MAX
    };
    FilmMeterMode mode = app.camera_values[FILM_CAM_METERING];
    if ((app.control == FILM_CAM_EXPOSURE && mode != FILM_METER_MANUAL) ||
        (app.control == FILM_CAM_EV_COMP && mode == FILM_METER_MANUAL)) return;
    int value = app.camera_values[app.control] + change;
    if (value < minimum[app.control] || value > maximum[app.control]) return;
    FilmStatus status = FILM_OK;
    if (app.control == FILM_CAM_SATURATION) {
        app.ui.recipe.base_saturation = value / 10.0f;
    } else if (app.control == FILM_CAM_METERING || app.control == FILM_CAM_EV_COMP) {
        if (app.live && app.control == FILM_CAM_METERING && value == FILM_METER_MANUAL)
            status = apply_exposure(app.camera_values[FILM_CAM_EXPOSURE]);
        if (status == FILM_OK) reset_meter();
    } else if (app.control == FILM_CAM_EXPOSURE && app.live) {
        status = apply_exposure(value);
    } else {
        if (app.control == FILM_CAM_SHARPNESS) status = film_camera_preview_stop(&app.camera);
        if (status == FILM_OK) status = film_camera_set_control(&app.camera, app.control, value);
        app.camera_auto = film_camera_auto_state(&app.camera);
        reset_meter();
    }
    if (status != FILM_OK) {
        update_status(control_names[app.control], status, app.camera.last_result);
        return;
    }
    app.camera_values[app.control] = value;
}

static void update_status(const char *operation, FilmStatus status, uint32_t result)
{
    snprintf(app.status, sizeof app.status, "%s  %s", operation, film_status_name(status));
    film_log(app.log, operation, status, result);
}

static FilmStatus init(void *context, FilmPixelFormat format)
{
    return film_camera_open(context, format, FILM_CAMERA_REAR1);
}

static FilmStatus capture(void *context, FilmFrame *frame)
{
    return film_camera_capture(context, frame);
}

static uint64_t ticks(void *context)
{
    (void)context;
    return (uint64_t)(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000.0));
}

static uint32_t device_result(void *context)
{
    return ((FilmCamera *)context)->last_result;
}

static int compare_looks(const void *a, const void *b)
{
    return strcasecmp(((const Look *)a)->name, ((const Look *)b)->name);
}

static void scan_looks(const char *path)
{
    DIR *directory = opendir(path);
    if (!directory) return;
    unsigned first = app.lut_count;
    struct dirent *entry;
    while (app.lut_count < MAX_LUTS && (entry = readdir(directory))) {
        size_t len = strlen(entry->d_name);
        if (len <= 5 || len >= sizeof app.luts[0].name ||
            strcasecmp(entry->d_name + len - 5, ".cube") || entry->d_name[0] == '.' ||
            entry->d_type == DT_DIR) continue;
        Look *look = &app.luts[app.lut_count++];
        look->directory = path;
        memcpy(look->name, entry->d_name, len + 1);
    }
    closedir(directory);
    qsort(app.luts + first, app.lut_count - first, sizeof *app.luts, compare_looks);
}

static unsigned find_look(const char *id)
{
    for (unsigned i = 0; i < app.lut_count; ++i)
        if (!strcmp(id, app.luts[i].name)) return i + 1;
    return 0;
}

static FilmStatus load_look_profile(const Look *look, FilmProfile *profile)
{
    char path[128];
    snprintf(path, sizeof path, "%s%.*s.f3p", look->directory, (int)strlen(look->name) - 5, look->name);
    return film_profile_read(path, profile);
}

static FilmStatus load_look_lut(const Look *look, FilmLut *lut)
{
    char path[128];
    snprintf(path, sizeof path, "%s%s", look->directory, look->name);
    FILE *file = fopen(path, "rb");
    if (!file) return FILM_IO;
    unsigned line = 0;
    FilmStatus status = film_lut_load(file, lut, &line);
    fclose(file);
    if (status != FILM_OK && app.log) fprintf(app.log, "%s line %u\n", look->name, line);
    return status;
}

static void run_commands(void)
{
    FILE *commands = fopen(ROOT "commands.txt", "r");
    if (!commands) return;
    FILE *replies = fopen(ROOT "replies.txt", "w");
    if (replies) {
        FilmLut lut = {0};
        FilmDebug debug = {.ops = {init, capture, ticks, device_result}, .context = &app.camera,
            .jpeg_path = ROOT "debug.jpg", .source_path = ROOT "debug.f3s", .ui = film_ui_new(3)};
        if (load_look_profile(&legacy_look, &debug.ui.recipe.profile) == FILM_OK &&
            load_look_lut(&legacy_look, &lut) == FILM_OK) {
            debug.lut = &lut;
            memcpy(debug.ui.recipe.lut_id, "look.cube", sizeof "look.cube");
        }
        film_debug_serve(&debug, commands, replies);
        film_debug_clear(&debug);
        film_lut_free(&lut);
        fclose(replies);
    }
    fclose(commands);
    film_camera_close(&app.camera);
}

static unsigned swizzle(unsigned x, unsigned y)
{
    unsigned tile = (x >> 3) + (y >> 3) * (PREVIEW_TEX_SIDE / 8);
    unsigned offset = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                      ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return tile * 64 + offset;
}

static uint8_t preview_channel(int fixed)
{
    int code = (fixed + 128) / 256;
    return code < 0 ? 0 : code > 255 ? 255 : code;
}

static void show_camera_image(const FilmImage *image, FilmYuv matrix, float saturation)
{
    static struct {
        int y[256], red[256], green_u[256], green_v[256], blue[256];
        FilmYuv matrix;
        float saturation;
        bool ready;
    } colors;
    if (!colors.ready || colors.matrix != matrix || colors.saturation != saturation) {
        FilmYuvTransform t = film_yuv_transform(matrix, saturation);
        for (int i = 0; i < 256; ++i) {
            const float scale = 255 * 256;
            colors.y[i] = lroundf((i - t.y_offset) * t.y_gain * scale);
            colors.red[i] = lroundf((i - 128) * t.r_cr * scale);
            colors.green_u[i] = lroundf((i - 128) * t.g_cb * scale);
            colors.green_v[i] = lroundf((i - 128) * t.g_cr * scale);
            colors.blue[i] = lroundf((i - 128) * t.b_cb * scale);
        }
        colors.matrix = matrix;
        colors.saturation = saturation;
        colors.ready = true;
    }
    C3D_FrameSync();
    uint8_t *tex = app.texture.data;
    for (unsigned y = 0; y < PREVIEW_TEX_ROWS; ++y) {
        const uint8_t *row = image->pixels + (y * image->height / PREVIEW_TEX_ROWS) * image->width * 2;
        for (unsigned x = 0; x < PREVIEW_TEX_SIDE; ++x) {
            unsigned sx = x * image->width / PREVIEW_TEX_SIDE;
            const uint8_t *pair = row + (sx & ~1u) * 2;
            int yy = colors.y[pair[(sx & 1) * 2]], u = pair[1], v = pair[3];
            unsigned i = swizzle(x, y) * 3;
            int b = yy + colors.blue[u], g = yy + colors.green_u[u] + colors.green_v[v];
            int r = yy + colors.red[v];
            const int white = 255 * 256;
            if (r < 0 || g < 0 || b < 0 || r > white || g > white || b > white) {
                FilmRgb fit = film_fit_chroma((FilmRgb){r / (float)white, g / (float)white, b / (float)white},
                                             yy / (float)white, 1);
                r = fit.r * white;
                g = fit.g * white;
                b = fit.b * white;
            }
            tex[i] = preview_channel(b);
            tex[i + 1] = preview_channel(g);
            tex[i + 2] = preview_channel(r);
        }
    }
    C3D_TexFlush(&app.texture);
    app.image_width = image->width;
    app.image_height = image->height;
}

static void show_image(const FilmImage *image, FilmYuv matrix)
{
    if (image->format == FILM_YUYV) {
        show_camera_image(image, matrix, 0);
        return;
    }
    C3D_FrameSync();
    uint8_t *tex = app.texture.data;
    for (unsigned y = 0; y < PREVIEW_TEX_ROWS; ++y) {
        for (unsigned x = 0; x < PREVIEW_TEX_SIDE; ++x) {
            FilmRgb c = film_decode_pixel(image, x * image->width / PREVIEW_TEX_SIDE,
                                         y * image->height / PREVIEW_TEX_ROWS, matrix);
            unsigned i = swizzle(x, y) * 3;
            tex[i] = (uint8_t)(c.b * 255 + 0.5f);
            tex[i + 1] = (uint8_t)(c.g * 255 + 0.5f);
            tex[i + 2] = (uint8_t)(c.r * 255 + 0.5f);
        }
    }
    C3D_TexFlush(&app.texture);
    app.image_width = image->width;
    app.image_height = image->height;
}

static void roll_path(char path[64], unsigned id)
{
    snprintf(path, 64, ROLLS "ROLL%04u.F3S", id);
}

static void load_roll(void)
{
    bool exists[MAX_PHOTOS + 1] = {false};
    DIR *directory = opendir(ROLLS);
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            unsigned id;
            char suffix;
            if (strlen(entry->d_name) == 12 && sscanf(entry->d_name, "ROLL%4u.F3S%c", &id, &suffix) == 1 &&
                id > 0 && id <= MAX_PHOTOS && !strcmp(entry->d_name + 8, ".F3S")) exists[id] = true;
        }
        closedir(directory);
    }
    app.next_number = 1;
    for (unsigned i = 1; i <= MAX_PHOTOS; ++i) {
        if (exists[i]) {
            app.photos[app.count++] = i;
            app.next_number = i + 1;
        }
    }
    app.ui = film_ui_new(app.count ? app.count : VIRTUAL_PHOTOS);
    app.ui.looks = app.lut_count + 1;
    app.capture_strength = 1;
    if (app.lut_count) {
        FilmProfile profile;
        FilmStatus status = load_look_profile(&app.luts[0], &profile);
        if (status == FILM_OK) {
            app.capture_look = &app.luts[0];
            app.capture_strength = profile.strength;
        } else film_log(app.log, "Default LUT profile", status, 0);
    }
}

static void sync_darkroom(void)
{
    if (app.ui.page != FILM_DARKROOM) return;
    unsigned id = app.count ? app.photos[app.ui.selection] : MAX_PHOTOS + app.ui.selection + 1;
    if (id == app.darkroom_id) return;
    FilmFrame frame = {.recipe = film_default_recipe()};
    FilmStatus status = FILM_OK;
    if (app.count) {
        char path[64];
        roll_path(path, id);
        status = film_frame_read(path, &frame);
    } else {
        frame.recipe = app.ui.recipe;
        frame.recipe.grain_amount = .3f;
        frame.recipe.grain_seed += app.ui.selection;
        if (app.capture_look) {
            status = load_look_profile(app.capture_look, &frame.recipe.profile);
            if (status == FILM_OK) {
                strcpy(frame.recipe.lut_id, app.capture_look->name);
                frame.recipe.profile.strength = app.capture_strength;
            }
        }
    }
    app.darkroom_id = id;
    app.darkroom_ready = status == FILM_OK;
    app.darkroom = app.saved_recipe = frame.recipe;
    app.saved_aspect = frame.recipe.aspect;
    app.ui.look = find_look(frame.recipe.lut_id);
    if (status != FILM_OK) update_status("Open recipe", status, 0);
    film_frame_free(&frame);
}

static void change_darkroom_look(int direction)
{
    unsigned index = (app.ui.look + app.ui.looks + direction) % app.ui.looks;
    const Look *look = index ? &app.luts[index - 1] : NULL;
    const char *id = look ? look->name : "";
    if (!strcmp(id, app.darkroom.lut_id)) return;
    FilmProfile preset = film_default_recipe().profile;
    if (look && strcmp(id, app.saved_recipe.lut_id)) {
        FilmStatus status = load_look_profile(look, &preset);
        if (status != FILM_OK) { update_status("LUT profile", status, 0); return; }
    }
    film_ui_select_look(&app.darkroom, &app.saved_recipe, id, &preset);
    app.ui.look = index;
    app.capture_look = look;
    app.capture_strength = app.darkroom.profile.strength;
}

static void adjust_darkroom(bool first_row, int direction)
{
    if (!app.darkroom_ready) return;
    if (app.darkroom_panel == DARKROOM_GRAIN) {
        if (first_row) select_grain(direction);
        else adjust_grain(direction * 10);
    } else if (app.darkroom_panel == DARKROOM_TONE) {
        if (first_row) app.tone_control = (app.tone_control + FILM_TONE_COUNT + direction) % FILM_TONE_COUNT;
        else {
            float *value = &app.darkroom.tone.value[app.tone_control];
            float step = app.tone_control == FILM_TONE_EV ? .25f : .05f;
            *value = film_tone_limit(app.tone_control, roundf(*value / step + direction) * step);
        }
    } else if (first_row) change_darkroom_look(direction);
    else if (app.ui.look) {
        float *strength = &app.darkroom.profile.strength;
        *strength = fminf(1, fmaxf(0, roundf(*strength * 100 + direction * 10) / 100));
        app.capture_look = &app.luts[app.ui.look - 1];
        app.capture_strength = *strength;
    }
}

static void save_darkroom_recipe(void)
{
    if (!app.count || !app.darkroom_ready || app.save.thread) return;
    unsigned id = app.next_number;
    if (id > MAX_PHOTOS) { update_status("Save recipe copy", FILM_STATE, 0); return; }
    char path[64];
    roll_path(path, app.darkroom_id);
    FilmFrame frame = {0};
    FilmStatus status = film_frame_read(path, &frame);
    if (status == FILM_OK) {
        frame.recipe = app.darkroom;
        frame.recipe.aspect = film_ui_output_aspect(&app.ui, frame.recipe.aspect);
        roll_path(path, id);
        struct stat existing;
        // libctru replaces an existing SD destination by deleting it first.
        status = stat(path, &existing) == 0 ? FILM_STATE : film_frame_write(path, &frame);
        if (status == FILM_OK) {
            app.photos[app.count++] = id;
            ++app.next_number;
            film_ui_add_photo(&app.ui);
            app.darkroom_id = id;
            app.darkroom = app.saved_recipe = frame.recipe;
            app.saved_aspect = frame.recipe.aspect;
        }
    }
    film_frame_free(&frame);
    update_status("Save recipe copy", status, 0);
}

static FilmStatus take(FilmFrame *frame)
{
    frame->recipe = app.ui.recipe;
    if (!app.live) return film_mock_capture(frame);
    frame->recipe.yuv = app.camera_yuv;
    return app.camera.preview_event ? film_camera_preview_snapshot(&app.camera, frame) :
                                      film_camera_capture(&app.camera, frame);
}

static FilmStatus update_preview(FilmFrame *single, bool allow_meter)
{
    FilmImage image = {0};
    FilmStatus status = FILM_OK;
    if (!app.preview_single) {
        status = film_camera_preview_start(&app.camera);
        if (status == FILM_OK) status = film_camera_preview_poll(&app.camera, &image);
        if (status != FILM_OK) {
            snprintf(app.status, sizeof app.status, "CAM %s %08lX  Single fallback",
                     app.camera.preview_stage ? app.camera.preview_stage : "preview",
                     (unsigned long)app.camera.last_result);
            film_log(app.log, app.status, status, app.camera.last_result);
            app.preview_single = app.camera.initialized;
            reset_preview_tracking();
        }
    }
    if (app.preview_single) {
        status = film_camera_capture(&app.camera, single);
        image = single->source;
    }
    if (status == FILM_OK && image.pixels) {
        FilmMeterMode mode = app.camera_values[FILM_CAM_METERING];
        app.meter_reading = film_camera_sample_preview(&app.stability, &image, app.camera_yuv,
                                                        app.ui.recipe.aspect, mode);
        show_camera_image(&image, app.camera_yuv, app.ui.recipe.base_saturation);
        uint64_t now = osGetTime();
        if (!app.preview_period) app.preview_period = now;
        else if (now - app.preview_period >= 1000) {
            ++app.preview_frames;
            app.preview_fps = (app.preview_frames * UINT64_C(1000)) / (now - app.preview_period);
            app.preview_period = now;
            app.preview_frames = 0;
        } else ++app.preview_frames;
        if (allow_meter && mode != FILM_METER_MANUAL && !app.meter_failed && now >= app.meter_after &&
            film_camera_stability_read(&app.stability).metering) {
            int next = film_camera_meter_step(&app.meter, app.applied_exposure, app.meter_reading,
                                             app.camera_values[FILM_CAM_EV_COMP]);
            FilmStatus metering = apply_exposure(next);
            if (metering != FILM_OK) {
                app.meter_failed = true;
                update_status("Software AE", metering, app.camera.last_result);
                if (!app.camera.initialized) return metering;
            }
        }
    }
    return status;
}

static void save_capture(void *argument)
{
    CaptureSave *save = argument;
    uint64_t started = ticks(NULL);
    FilmRecipe *recipe = &save->frame.recipe;
    if (save->look) {
        float strength = recipe->profile.strength;
        save->profile_status = load_look_profile(save->look, &recipe->profile);
        if (save->profile_status == FILM_OK) {
            recipe->profile.strength = strength;
            strcpy(recipe->lut_id, save->look->name);
        }
    }
    char path[64];
    roll_path(path, save->id);
    save->status = film_frame_write(path, &save->frame);
    save->written = save->status == FILM_OK;
    if (save->written) {
        FilmFrame saved = {0};
        save->status = film_frame_read(path, &saved);
        film_frame_free(&saved);
    }
    save->save_ms = (ticks(NULL) - started) / 1000;
}

static bool finish_capture(bool wait)
{
    CaptureSave *save = &app.save;
    if (!save->frame.source.pixels) return false;
    if (save->thread) {
        u64 timeout = wait ? U64_MAX : 0;
        Result joined = save->completion == UINT32_MAX ? threadJoin(save->thread, timeout) :
                                                       svcWaitSynchronization(save->completion, timeout);
        if (joined != 0) return false;
        threadFree(save->thread);
    }
    if (save->profile_status != FILM_OK)
        film_log(app.log, "Capture profile", save->profile_status, 0);
    if (save->written) ++app.next_number;
    bool saved = save->status == FILM_OK;
    if (saved) {
        if (!app.count) app.ui.photos = 0;
        app.photos[app.count++] = save->id;
        film_ui_add_photo(&app.ui);
        snprintf(app.status, sizeof app.status, "%s #%04u %" PRIu64 "ms %s SD %" PRIu64 "ms CRC OK",
                 save->frame.camera ? "CAM" : "SIM", save->id, save->capture_ms,
                 save->capture_mode, save->save_ms);
        film_log(app.log, app.status, FILM_OK, 0);
    } else update_status(save->written ? "Capture SD verify" : "Capture SD write", save->status, 0);
    film_frame_free(&save->frame);
    *save = (CaptureSave){0};
    return saved;
}

static bool capture_photo(FilmFrame *shown)
{
    CaptureSave *save = &app.save;
    if (save->thread) {
        snprintf(app.status, sizeof app.status, "Saving #%04u to SD...", save->id);
        return false;
    }
    if (app.next_number > MAX_PHOTOS) { update_status("Capture", FILM_STATE, 0); return false; }
    uint64_t started = ticks(NULL);
    if (shown->source.pixels) {
        save->frame = *shown;
        *shown = (FilmFrame){0};
        save->frame.recipe = app.ui.recipe;
        save->frame.recipe.yuv = app.camera_yuv;
        save->capture_mode = "preview";
    } else {
        FilmStatus status = take(&save->frame);
        if (status != FILM_OK) { update_status("Capture", status, app.camera.last_result); return false; }
        save->capture_mode = app.camera.preview_event ? "copy" : "capture";
    }
    save->id = app.next_number;
    save->look = app.capture_look;
    save->frame.recipe.profile.strength = app.capture_strength;
    save->frame.recipe.grain_amount = .3f;
    save->capture_ms = (ticks(NULL) - started) / 1000;
    snprintf(app.status, sizeof app.status, "Saving #%04u to SD...", save->id);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    save->thread = threadCreate(save_capture, save, 16 * 1024, priority < 0x3f ? priority + 1 : priority, 0, false);
    if (save->thread) {
        save->completion = threadGetHandle(save->thread);
        return false;
    }
    save_capture(save);
    return finish_capture(true);
}

static void export_photo(void)
{
    FilmFrame frame = {0};
    FilmImage image = {0};
    FilmLut lut = {0};
    if (!app.darkroom_ready) { update_status("Develop recipe", FILM_STATE, 0); return; }
    unsigned look_index = find_look(app.darkroom.lut_id);
    const Look *look = look_index ? &app.luts[look_index - 1] : NULL;
    unsigned selection = app.ui.selection;
    FilmCameraColor camera_color;
    FilmGrainProfile grain_profile;
    FilmDevelopOptions options = {.adaptive_grain = true, .grain = app.grain, .halation = app.halation};
    char filename[24] = {0};
    FilmStatus status;
    if (app.count) {
        char path[64];
        roll_path(path, app.photos[selection]);
        status = film_frame_read(path, &frame);
    } else {
        frame.recipe = app.darkroom;
        status = film_mock_capture(&frame);
    }
    if (status == FILM_OK) {
        frame.recipe = app.darkroom;
        if (!app.count) {
            frame.recipe.scale = app.ui.recipe.scale;
            frame.recipe.filter = app.ui.recipe.filter;
        }
        frame.recipe.aspect = film_ui_output_aspect(&app.ui, frame.recipe.aspect);
        frame.recipe.grain_seed ^= UINT32_C(0x9e3779b9) * (app.count ? app.photos[selection] : selection + 1);
        if (look && frame.recipe.profile.strength > 0) status = load_look_lut(look, &lut);
    }
    if (status == FILM_OK) {
        status = load_camera_color(frame.camera, &camera_color, &options.camera_color);
        if (status != FILM_OK) film_log(app.log, "Camera RGB matrix", status, 0);
    }
    if (status == FILM_OK && frame.recipe.grain_amount > 0 && app.grain_reference[app.grain]) {
        FILE *file = fopen(grain_paths[app.grain], "r");
        status = file ? film_grain_profile_load(file, &grain_profile) : FILM_IO;
        if (file) fclose(file);
        if (status == FILM_OK) options.grain_profile = &grain_profile;
        else film_log(app.log, "DxO grain reference", status, 0);
    }
    if (status == FILM_OK) status = film_develop_with_options(&frame, lut.samples ? &lut : NULL, &options, &image);
    if (status == FILM_OK) {
        char path[96];
        struct stat st;
        status = FILM_STATE;
        for (unsigned revision = 1; revision <= MAX_PHOTOS; ++revision) {
            unsigned id = app.count ? app.photos[selection] : selection + 1;
            snprintf(filename, sizeof filename, "%c%04u-%04u.JPG", app.count ? 'R' : 'V', id, revision);
            snprintf(path, sizeof path, EXPORTS "%s", filename);
            if (stat(path, &st) == 0) continue;
            status = film_jpeg_write(path, &image, frame.recipe.jpeg_quality);
            break;
        }
    }
    if (status == FILM_OK) {
        show_image(&image, FILM_601_FULL);
        bool applied = look && frame.recipe.profile.strength > 0;
        snprintf(app.status, sizeof app.status, "JPEG OK %s %ux%u", filename, image.width, image.height);
        film_log(app.log, app.status, status, 0);
        if (app.log && applied) {
            fprintf(app.log, "Applied LUT %s, %u^3, %.0f%%, %s%s\n", look->name, lut.size,
                    100 * frame.recipe.profile.strength, lut.trilinear ? "trilinear" : "tetrahedral",
                    lut.shadow_shaper ? ", dark shaper" : "");
            fflush(app.log);
        }
        if (app.log) {
            fprintf(app.log, "Develop camera matrix %s, grain %s %.0f%% (%s), halation %s\n",
                    options.camera_color ? "applied" : "absent",
                    options.grain_profile ? grain_stock_names[app.grain] : grain_names[app.grain],
                    frame.recipe.grain_amount * 100,
                    options.grain_profile ? "DxO output fit" : "procedural",
                    app.halation ? "on" : "off");
            fflush(app.log);
        }
    } else update_status(look ? "LUT export" : "Export", status, 0);
    film_lut_free(&lut);
    film_image_free(&image);
    film_frame_free(&frame);
}

static void y2r_check(void)
{
    FilmFrame frame = {0};
    FilmImage cpu = {0};
    FilmStatus status = film_mock_capture(&frame);
    if (status == FILM_OK) status = film_rgb24(&frame.source, FILM_601_FULL, &cpu);
    if (status == FILM_OK) {
        static const unsigned lines[] = {2, 4, 8};
        char outcomes[3][16];
        for (unsigned test = 0; test < 3; ++test) {
            FilmImage gpu = {0};
            uint32_t rc = 0;
            status = film_y2r_convert(&frame.source, FILM_601_FULL, lines[test], &gpu, &rc);
            if (status == FILM_OK) {
                unsigned max = 0;
                uint64_t sum = 0;
                for (size_t i = 0; i < (size_t)640 * 480 * 3; ++i) {
                    unsigned difference = abs((int)gpu.pixels[i] - cpu.pixels[i]);
                    if (difference > max) max = difference;
                    sum += difference;
                }
                if (app.log) fprintf(app.log, "Y2R %u lines max=%u mean=%.3f\n",
                                     lines[test], max, sum / (640.0 * 480 * 3));
                snprintf(outcomes[test], sizeof outcomes[test], "max %u", max);
                if (max > 4) status = FILM_DEVICE;
            } else {
                snprintf(outcomes[test], sizeof outcomes[test], "%s",
                         status == FILM_DEVICE && !rc ? "timeout" : film_status_name(status));
            }
            char operation[32];
            snprintf(operation, sizeof operation, "Y2R %u lines %s", lines[test], outcomes[test]);
            film_log(app.log, operation, status, rc);
            film_image_free(&gpu);
        }
        snprintf(app.y2r_status, sizeof app.y2r_status, "Y2R  2:%s  4:%s  8:%s",
                 outcomes[0], outcomes[1], outcomes[2]);
    } else {
        snprintf(app.y2r_status, sizeof app.y2r_status, "Y2R setup: %s", film_status_name(status));
        film_log(app.log, "Y2R setup", status, 0);
    }
    film_frame_free(&frame);
    film_image_free(&cpu);
}

static FilmStatus measure_frames(FilmCamera *camera, bool keep_stream, FilmCaptureTiming *times, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        FilmFrame frame = {0};
        FilmStatus status = film_camera_measure(camera, &frame, keep_stream, &times[i]);
        if (status == FILM_OK && (frame.camera != camera->sensor || frame.source.width != 640 ||
                                  frame.source.height != 480 || frame.source.format != FILM_YUYV))
            status = FILM_CORRUPT;
        film_frame_free(&frame);
        if (status != FILM_OK) return status;
    }
    return FILM_OK;
}

static unsigned median_index(const FilmCaptureTiming times[3])
{
    unsigned order[] = {0, 1, 2};
    for (unsigned i = 1; i < 3; ++i) {
        for (unsigned j = i; j > 0 && times[order[j]].total_us < times[order[j - 1]].total_us; --j) {
            unsigned index = order[j];
            order[j] = order[j - 1];
            order[j - 1] = index;
        }
    }
    return order[1];
}

static void camera_probe(void)
{
    film_camera_close(&app.camera);
    app.live = false;
    memset(app.probe_rows, 0, sizeof app.probe_rows);
    app.probe_title = "CAM timing, milliseconds";
    for (FilmSensor sensor = FILM_CAMERA_REAR1; sensor <= FILM_CAMERA_FRONT; ++sensor) {
        unsigned row = 2 * (sensor - FILM_CAMERA_REAR1);
        FilmCamera camera = {0};
        FilmCaptureTiming one[3], warm[4];
        FilmStatus status = film_camera_open(&camera, FILM_YUYV, sensor);
        if (status == FILM_OK) status = measure_frames(&camera, false, one, 3);
        if (status == FILM_OK) {
            const FilmCaptureTiming *sample = &one[median_index(one)];
            snprintf(app.probe_rows[row], sizeof app.probe_rows[row], "%s OK: first %u, single %u",
                     sensors[sensor], one[0].total_us / 1000, sample->total_us / 1000);
            snprintf(app.probe_rows[row + 1], sizeof app.probe_rows[row + 1], "prep %.1f arm %.1f wait %.1f end %.1f",
                     sample->setup_us / 1000.0, sample->arm_us / 1000.0,
                     sample->wait_us / 1000.0, sample->finish_us / 1000.0);
            status = measure_frames(&camera, true, warm, 4);
            size_t used = strlen(app.probe_rows[row]);
            if (status == FILM_OK) {
                unsigned minimum = warm[1].total_us;
                for (unsigned i = 2; i < 4; ++i)
                    if (warm[i].total_us < minimum) minimum = warm[i].total_us;
                snprintf(app.probe_rows[row] + used, sizeof app.probe_rows[row] - used,
                         ", warm %u/%u", minimum / 1000, warm[1 + median_index(warm + 1)].total_us / 1000);
            } else {
                snprintf(app.probe_rows[row] + used, sizeof app.probe_rows[row] - used,
                         ", warm %s", film_status_name(status));
            }
        } else {
            snprintf(app.probe_rows[row], sizeof app.probe_rows[row], "%s %s 0x%08" PRIx32,
                     sensors[sensor], film_status_name(status), camera.last_result);
        }
        film_camera_close(&camera);
        if (app.log) fprintf(app.log, "%s\n%s\n", app.probe_rows[row], app.probe_rows[row + 1]);
    }
    if (app.log) fflush(app.log);
    FilmFrame preview = {0};
    if (film_mock_capture(&preview) == FILM_OK) show_image(&preview.source, FILM_601_FULL);
    film_frame_free(&preview);
}

static FilmStatus lut_probe(const FilmFrame *frame, unsigned size, uint64_t *elapsed)
{
    FilmLut lut = {.size = size, .max = {1, 1, 1}};
    if (size) {
        lut.samples = malloc((size_t)size * size * size * sizeof *lut.samples);
        if (!lut.samples) return FILM_NOMEM;
        FilmRgb *sample = lut.samples;
        for (unsigned b = 0; b < size; ++b)
            for (unsigned g = 0; g < size; ++g)
                for (unsigned r = 0; r < size; ++r) {
                    float red = (float)r / (size - 1), blue = (float)b / (size - 1);
                    *sample++ = (FilmRgb){red * red, (float)g / (size - 1), blue * blue};
                }
    }
    FilmImage output = {0};
    uint64_t start = ticks(NULL);
    FilmStatus status = film_develop(frame, size ? &lut : NULL, &output);
    *elapsed = ticks(NULL) - start;
    film_image_free(&output);
    film_lut_free(&lut);
    return status;
}

static void gpu_resize_probe(void)
{
    enum { INPUT = 256, OUTPUT = 512, READ_BYTES = OUTPUT * OUTPUT * 4 };
    FilmImage input = {0}, cpu = {0};
    C3D_Tex texture;
    bool texture_ready = C3D_TexInit(&texture, INPUT, INPUT, GPU_RGBA8);
    C3D_RenderTarget *target = C3D_RenderTargetCreate(OUTPUT, OUTPUT, GPU_RB_RGBA8, -1);
    uint8_t *readback = linearMemAlign(READ_BYTES, 0x80);
    FilmStatus status = film_image_alloc(&input, INPUT, INPUT, FILM_RGB24);
    if (!texture_ready || !target || !readback || status != FILM_OK) {
        snprintf(app.probe_rows[3], sizeof app.probe_rows[3], "GPU benchmark allocation failed");
        goto cleanup;
    }
    for (unsigned y = 0; y < INPUT; ++y)
        for (unsigned x = 0; x < INPUT; ++x) {
            uint8_t *pixel = input.pixels + (y * INPUT + x) * 3;
            pixel[0] = x;
            pixel[1] = y < INPUT / 2 ? 2 * y : 2 * (INPUT - 1 - y);
            pixel[2] = 80 + x / 4;
        }
    uint64_t start = ticks(NULL);
    status = film_resize(&input, OUTPUT, OUTPUT, FILM_BILINEAR, &cpu);
    uint64_t cpu_us = ticks(NULL) - start;
    if (status != FILM_OK) {
        snprintf(app.probe_rows[3], sizeof app.probe_rows[3], "CPU resize: %s", film_status_name(status));
        goto cleanup;
    }
    C3D_TexSetFilter(&texture, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    start = ticks(NULL);
    uint8_t *tiles = texture.data;
    for (unsigned y = 0; y < INPUT; ++y)
        for (unsigned x = 0; x < INPUT; ++x) {
            const uint8_t *pixel = input.pixels + (y * INPUT + x) * 3;
            uint8_t *tile = tiles + swizzle(x, INPUT - 1 - y) * 4;
            tile[0] = 255;
            tile[1] = pixel[2];
            tile[2] = pixel[1];
            tile[3] = pixel[0];
        }
    C3D_TexFlush(&texture);
    uint64_t upload_us = ticks(NULL) - start;
    if (R_FAILED(GSPGPU_FlushDataCache(readback, READ_BYTES)) || !C3D_FrameBegin(0)) {
        snprintf(app.probe_rows[3], sizeof app.probe_rows[3], "GPU benchmark setup failed");
        goto cleanup;
    }
    start = ticks(NULL);
    static const Tex3DS_SubTexture sub = {INPUT, INPUT, 0, 1, 1, 0};
    C2D_TargetClear(target, 0);
    C2D_SceneBegin(target);
    C2D_DrawImageAt((C2D_Image){&texture, &sub}, 0, 0, 0, NULL, 2, 2);
    C3D_FrameEnd(0);
    u32 dims = GX_BUFFER_DIM(OUTPUT, OUTPUT);
    C3D_SyncDisplayTransfer(target->frameBuf.colorBuf, dims, (u32 *)readback, dims,
                           GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                           GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8));
    Result rc = GSPGPU_InvalidateDataCache(readback, READ_BYTES);
    uint64_t gpu_us = ticks(NULL) - start;
    if (R_FAILED(rc)) {
        snprintf(app.probe_rows[3], sizeof app.probe_rows[3], "GPU readback error 0x%08" PRIx32, (uint32_t)rc);
        goto cleanup;
    }
    unsigned max = 0;
    for (unsigned i = 0; i < OUTPUT * OUTPUT; ++i)
        for (unsigned c = 0; c < 3; ++c) {
            unsigned difference = abs((int)readback[i * 4 + 3 - c] - cpu.pixels[i * 3 + c]);
            if (difference > max) max = difference;
        }
    snprintf(app.probe_rows[3], sizeof app.probe_rows[3], "Bilinear 256 > 512, CPU %.1f ms", cpu_us / 1000.0);
    snprintf(app.probe_rows[4], sizeof app.probe_rows[4], "GPU + read %.1f ms, upload %.1f ms",
             gpu_us / 1000.0, upload_us / 1000.0);
    snprintf(app.probe_rows[5], sizeof app.probe_rows[5], "GPU vs CPU max error %u", max);
cleanup:
    if (target) C3D_RenderTargetDelete(target);
    if (texture_ready) C3D_TexDelete(&texture);
    if (readback) linearFree(readback);
    film_image_free(&cpu);
    film_image_free(&input);
}

static void acceleration_probe(void)
{
    memset(app.probe_rows, 0, sizeof app.probe_rows);
    app.probe_title = "CPU LUT / GPU resize";
    FilmFrame frame = {.recipe = film_default_recipe()};
    FilmStatus setup = film_mock_capture(&frame);
    static const unsigned sizes[] = {0, 33, 65};
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t elapsed = 0;
        FilmStatus status = setup == FILM_OK ? lut_probe(&frame, sizes[i], &elapsed) : setup;
        snprintf(app.probe_rows[i], sizeof app.probe_rows[i], "VGA CPU LUT %u: %.1f ms (%s)",
                 sizes[i], elapsed / 1000.0, film_status_name(status));
    }
    film_frame_free(&frame);
    gpu_resize_probe();
    if (app.log) {
        for (unsigned i = 0; i < 6; ++i) fprintf(app.log, "%s\n", app.probe_rows[i]);
        fflush(app.log);
    }
}

static void develop_quality_probe(void)
{
    app.probe_title = "Develop quality";
    memset(app.probe_rows, 0, sizeof app.probe_rows);
    FilmImage fixture = {0};
    FilmStatus status = film_image_alloc(&fixture, 640, 480, FILM_RGB24);
    if (status != FILM_OK) {
        snprintf(app.probe_rows[0], sizeof app.probe_rows[0], "Fixture %s", film_status_name(status));
        return;
    }
    for (unsigned y = 0; y < fixture.height; ++y)
        for (unsigned x = 0; x < fixture.width; ++x) {
            uint8_t *p = fixture.pixels + ((size_t)y * fixture.width + x) * 3;
            p[0] = x * 255 / (fixture.width - 1);
            p[1] = y * 255 / (fixture.height - 1);
            p[2] = ((x / 7) ^ (y / 11)) & 1 ? 220 : 20;
        }
    for (unsigned i = 0; i < 2; ++i) {
        FilmResizeQuality q = {0};
        status = film_3ds_resize_quality(&fixture, i + 3, &q);
        unsigned row = i * 3;
        const char *name = i ? "2x" : "1.5x";
        if (status != FILM_OK || q.gpu_error) {
            snprintf(app.probe_rows[row], sizeof app.probe_rows[0], "%s failed after %u tiles: %s", name,
                     q.tiles, status == FILM_OK ? q.gpu_error : film_status_name(status));
        } else {
            snprintf(app.probe_rows[row], sizeof app.probe_rows[0], "%s %s max %u p99 %u seam %u",
                     name, q.gpu_accepted ? "PASS" : "CHECK", q.max_error, q.p99_error, q.seam_error);
            snprintf(app.probe_rows[row + 1], sizeof app.probe_rows[0],
                     "Sample %u MAE %.2f GPU/CPU %.0f/%.0f ms",
                     q.sample_max, q.mean_error, q.gpu_us / 1000.0, q.cpu_us / 1000.0);
            snprintf(app.probe_rows[row + 2], sizeof app.probe_rows[0], "Pixels >4: %u   peak: %u,%u",
                     q.over_four, q.peak_x, q.peak_y);
        }
    }
    film_image_free(&fixture);
    if (app.log) {
        for (unsigned i = 0; i < 6; ++i) fprintf(app.log, "%s\n", app.probe_rows[i]);
        fflush(app.log);
    }
}

static void layer_quality_probe(void)
{
    app.probe_title = "PICA three-layer blend";
    memset(app.probe_rows, 0, sizeof app.probe_rows);
    for (unsigned i = 0; i < 2; ++i) {
        FilmLayerQuality q;
        unsigned side = 256u << i, row = i * 3;
        const char *error = film_3ds_layer_quality(side, &q);
        if (error) {
            snprintf(app.probe_rows[row], sizeof app.probe_rows[0], "%u blend failed: %s", side, error);
            continue;
        }
        snprintf(app.probe_rows[row], sizeof app.probe_rows[0], "%u %s max %u  p99 %u  MAE %.2f", side,
                 q.max_error <= 2 && q.p99_error <= 1 ? "PASS" : "CHECK", q.max_error, q.p99_error, q.mean_error);
        snprintf(app.probe_rows[row + 1], sizeof app.probe_rows[0], "CPU prepare + upload %.2f ms", q.prepare_us / 1000.0);
        snprintf(app.probe_rows[row + 2], sizeof app.probe_rows[0], "GPU + read %.2f / CPU %.2f ms",
                 q.gpu_us / 1000.0, q.cpu_us / 1000.0);
    }
    if (app.log) {
        for (unsigned i = 0; i < 6; ++i) if (app.probe_rows[i][0]) fprintf(app.log, "%s\n", app.probe_rows[i]);
        fflush(app.log);
    }
}

static FilmStatus native_median(FilmCamera *camera, FilmImage *image, unsigned *time_us)
{
    FilmCaptureTiming times[3];
    for (unsigned i = 0; i < 3; ++i) {
        FilmStatus status = film_camera_receive(camera, image, false, &times[i]);
        if (status != FILM_OK) return status;
    }
    *time_us = times[median_index(times)].total_us;
    return FILM_OK;
}

static void native_color_probe(const FilmImage *raw, FilmYuv matrix, bool recommended)
{
    enum { RGB_BYTES = 640 * 480 * 3, FIRST_FILL = 0xa5, SECOND_FILL = 0x5a };
    static const char *const matrices[] = {"601 full", "601 limited", "709 full", "709 limited"};
    FilmImage rgb = {0}, bgr = {640, 480, FILM_RGB24, linearMemAlign(RGB_BYTES, 0x80)};
    uint64_t start = ticks(NULL);
    FilmStatus cpu = film_rgb24(raw, matrix, &rgb);
    uint64_t cpu_us = ticks(NULL) - start;
    uint32_t rc = 0;
    if (bgr.pixels) memset(bgr.pixels, FIRST_FILL, RGB_BYTES);
    start = ticks(NULL);
    FilmStatus y2r = bgr.pixels ? film_y2r_transfer(raw, matrix, 2, &bgr, &rc) : FILM_NOMEM;
    uint64_t y2r_us = ticks(NULL) - start;
    snprintf(app.probe_rows[3], sizeof app.probe_rows[3], "%s %s | CPU %s %.1f ms",
             recommended ? "CAM" : "Default", matrices[matrix], film_status_name(cpu), cpu_us / 1000.0);
    if (y2r == FILM_OK && cpu == FILM_OK) {
        unsigned max = 0, peak = 0, sum = 0, bad = 0;
        for (unsigned i = 0; i < RGB_BYTES; i += 3)
            for (unsigned c = 0; c < 3; ++c) {
                unsigned error = abs((int)bgr.pixels[i + 2 - c] - rgb.pixels[i + c]);
                if (error > max) { max = error; peak = i / 3; }
                if (error > 4) ++bad;
                sum += error;
            }
        snprintf(app.probe_rows[4], sizeof app.probe_rows[4], "Y2R direct %.1f ms | max %u mean %.2f",
                 y2r_us / 1000.0, max, sum / (double)RGB_BYTES);
        uint8_t *first = malloc(RGB_BYTES);
        if (first) {
            memcpy(first, bgr.pixels, RGB_BYTES);
            memset(bgr.pixels, SECOND_FILL, RGB_BYTES);
            y2r = film_y2r_transfer(raw, matrix, 2, &bgr, &rc);
            if (y2r == FILM_OK) {
                unsigned changed = 0, untouched = 0;
                for (unsigned i = 0; i < RGB_BYTES; ++i) {
                    if (first[i] != bgr.pixels[i]) ++changed;
                    if (first[i] == FIRST_FILL && bgr.pixels[i] == SECOND_FILL) ++untouched;
                }
                snprintf(app.probe_rows[5], sizeof app.probe_rows[5], ">4:%u diff:%u unwritten:%u y:%u",
                         bad, changed, untouched, peak / 640);
                if (app.log) fprintf(app.log, "Y2R peak (%u,%u)\n", peak % 640, peak / 640);
            } else {
                snprintf(app.probe_rows[5], sizeof app.probe_rows[5], "Y2R repeat %s 0x%08" PRIx32,
                         film_status_name(y2r), rc);
            }
            free(first);
        } else {
            snprintf(app.probe_rows[5], sizeof app.probe_rows[5], "Y2R repeat: no memory");
        }
    } else {
        snprintf(app.probe_rows[4], sizeof app.probe_rows[4], "Y2R %s %.1f ms 0x%08" PRIx32,
                 film_status_name(y2r), y2r_us / 1000.0, rc);
    }
    if (bgr.pixels) linearFree(bgr.pixels);
    film_image_free(&rgb);
}

static void native_camera_probe(FilmSensor sensor)
{
    film_camera_close(&app.camera);
    app.live = false;
    memset(app.probe_rows, 0, sizeof app.probe_rows);
    app.probe_title = "Native CAM / direct Y2R";
    FilmImage raw = {640, 480, FILM_YUYV, linearMemAlign(640 * 480 * 2, 0x80)};
    if (!raw.pixels) {
        snprintf(app.probe_rows[0], sizeof app.probe_rows[0], "CAM DMA allocation failed");
        return;
    }
    FilmCamera camera = {0};
    unsigned us = 0;
    FilmStatus status = film_camera_open(&camera, raw.format, sensor);
    if (status == FILM_OK) status = native_median(&camera, &raw, &us);
    snprintf(app.probe_rows[0], sizeof app.probe_rows[0], "%s YUYV default: %s %.1f ms",
             sensors[sensor], film_status_name(status), us / 1000.0);
    if (status == FILM_OK) {
        FilmYuv matrix = FILM_601_FULL;
        bool recommended = film_camera_yuv_matrix(&camera, &matrix) == FILM_OK;
        native_color_probe(&raw, matrix, recommended);
        us = 0;
        status = film_camera_request_30fps(&camera);
        if (status == FILM_OK) status = native_median(&camera, &raw, &us);
        snprintf(app.probe_rows[1], sizeof app.probe_rows[1], "YUYV request 30 fps: %s %.1f ms",
                 film_status_name(status), us / 1000.0);
    }
    film_camera_close(&camera);
    raw.format = FILM_RGB565_LE;
    us = 0;
    status = film_camera_open(&camera, raw.format, sensor);
    if (status == FILM_OK) status = native_median(&camera, &raw, &us);
    snprintf(app.probe_rows[2], sizeof app.probe_rows[2], "CAM RGB565 default: %s %.1f ms",
             film_status_name(status), us / 1000.0);
    film_camera_close(&camera);
    if (status == FILM_OK) show_image(&raw, FILM_601_FULL);
    linearFree(raw.pixels);
    if (app.log) {
        for (unsigned i = 0; i < 6; ++i) fprintf(app.log, "%s\n", app.probe_rows[i]);
        fflush(app.log);
    }
}

static void text_at(float x, float y, float scale, u32 color, const char *message)
{
    C2D_Text text;
    C2D_TextParse(&text, app.text, message);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

static const char *auto_indicator(uint32_t result, bool enabled, bool ready, bool stable)
{
    if (R_FAILED((Result)result)) return "?";
    if (!enabled) return "OFF";
    if (!ready) return "WAIT";
    return stable ? "OK" : "CHG";
}

static void draw(C3D_RenderTarget *top, C3D_RenderTarget *bottom)
{
    static const char *const pages[] = {"VIEW LV11", "ROLL", "DARKROOM"};
    u32 bg = C2D_Color32(18, 22, 23, 255), fg = C2D_Color32(241, 235, 216, 255);
    u32 gold = C2D_Color32(248, 193, 107, 255), dim = C2D_Color32(40, 49, 48, 255);
    unsigned w = 0, h = 0;
    FilmRect output = {0};
    unsigned zoom = app.live ? app.camera.zoom : 1;
    film_scaled_dims(640 / zoom, 480 / zoom, app.ui.recipe.scale, &w, &h);
    film_crop_rect(w, h, app.ui.recipe.aspect, &output);
    char line[100];
    C2D_TextBufClear(app.text);
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
    C2D_TargetClear(top, bg);
    C2D_SceneBegin(top);
    // PICA samples tiled row 0 at the top (v = 1).
    Tex3DS_SubTexture sub = {320, 240, 0, 1, 1, 1.0f - PREVIEW_TEX_ROWS / (float)PREVIEW_TEX_SIDE};
    float x = 40, y = 0;
    bool view = app.ui.page == FILM_VIEW && !app.probe_title;
    if (!view && app.image_width && app.image_height) {
        float scale = fminf(320.0f / app.image_width, 240.0f / app.image_height);
        sub.width = app.image_width * scale + .5f;
        sub.height = app.image_height * scale + .5f;
        x = (400 - sub.width) * .5f;
        y = (240 - sub.height) * .5f;
    }
    C2D_DrawImageAt((C2D_Image){&app.texture, &sub}, x, y, 0, NULL, 1, 1);
    FilmRect crop;
    if (view && film_crop_rect(320, 240, app.ui.recipe.aspect, &crop) == FILM_OK) {
        u32 shade = C2D_Color32(0, 0, 0, 96);
        unsigned right = crop.x + crop.width, bottom_y = crop.y + crop.height;
        C2D_DrawRectSolid(40, 0, 0.1f, 320, crop.y, shade);
        C2D_DrawRectSolid(40, bottom_y, 0.1f, 320, 240 - bottom_y, shade);
        C2D_DrawRectSolid(40, crop.y, 0.1f, crop.x, crop.height, shade);
        C2D_DrawRectSolid(40 + right, crop.y, 0.1f, 320 - right, crop.height, shade);
        C2D_DrawRectSolid(40 + crop.x, crop.y, 0.2f, crop.width, 1, gold);
        C2D_DrawRectSolid(40 + crop.x, bottom_y - 1, 0.2f, crop.width, 1, gold);
        C2D_DrawRectSolid(40 + crop.x, crop.y, 0.2f, 1, crop.height, gold);
        C2D_DrawRectSolid(40 + right - 1, crop.y, 0.2f, 1, crop.height, gold);
    }

    C2D_TargetClear(bottom, bg);
    C2D_SceneBegin(bottom);
    if (app.probe_title) {
        text_at(12, 9, 0.66f, gold, app.probe_title);
        for (unsigned i = 0; i < 6; ++i)
            text_at(12, 39 + i * 25, 0.48f, fg, app.probe_rows[i]);
        text_at(12, 205, 0.48f, gold, "B: back   ZL: CAM   ZR: native   SELECT+ZR: GPU");
        C3D_FrameEnd(0);
        return;
    }
    text_at(12, 9, 0.82f, gold, pages[app.ui.page]);
    if (view) {
        if (app.preview_failed) snprintf(line, sizeof line, "CAM ERROR");
        else if (!app.live) snprintf(line, sizeof line, "CAM OFF");
        else if (!app.preview_fps) snprintf(line, sizeof line, "%s %s WAIT", sensors[app.camera.sensor],
                                            app.preview_single ? "ONE" : "DMA");
        else snprintf(line, sizeof line, "%s %s %u FPS", sensors[app.camera.sensor],
                      app.preview_single ? "ONE" : "DMA", app.preview_fps);
        text_at(148, 12, 0.59f, gold, line);
        if (app.live) {
            FilmCameraStabilityResult stable = film_camera_stability_read(&app.stability);
            const char *meter = "M";
            if (app.camera_values[FILM_CAM_METERING] != FILM_METER_MANUAL) {
                meter = app.meter_failed ? "SW ERR" : "SW WAIT";
                if (!app.meter_failed && stable.metering && osGetTime() >= app.meter_after) {
                    float error = film_camera_meter_error_ev(app.meter_reading, app.camera_values[FILM_CAM_EV_COMP]);
                    bool limited = (app.applied_exposure == FILM_EXPOSURE_MAX && error > FILM_METER_TOLERANCE_EV) ||
                                   (app.applied_exposure == FILM_EXPOSURE_MIN && error < -FILM_METER_TOLERANCE_EV);
                    meter = limited ? "LIMIT" : app.meter.hold ? "SW STEP" : "SW";
                }
            }
            snprintf(line, sizeof line, "%s %+d  AWB %s", meter, app.applied_exposure,
                     auto_indicator(app.camera_auto.white_balance_result, app.camera_auto.white_balance,
                                    stable.ready, stable.white_balance));
            text_at(195, 45, 0.44f, gold, line);
        }
    }
    text_at(12, 34, 0.53f, fg, app.status);
    for (unsigned i = 0; i < 3; ++i) C2D_DrawRectSolid(10, 57 + i * 48, 0.1f, 300, 43, dim);
    if (app.ui.page == FILM_VIEW && app.camera_controls) {
        static const char *const wb[] = {"Auto", "3200K", "4150K", "5200K", "6000K", "7000K"};
        static const char *const contrast[] = {"Low", "Normal", "High"};
        static const char *const metering[] = {"Manual", "Full average", "Center weighted"};
        int value = app.camera_values[app.control];
        text_at(20, 68, 0.62f, fg, control_names[app.control]);
        if (app.control == FILM_CAM_WB) {
            const char *awb = !app.live || R_FAILED((Result)app.camera_auto.white_balance_result)
                ? "?" : app.camera_auto.white_balance ? "ON" : "OFF";
            snprintf(line, sizeof line, "<   %s   >    AWB %s", wb[value], awb);
        }
        else if (app.control == FILM_CAM_CONTRAST) snprintf(line, sizeof line, "<   %s   >", contrast[value + 1]);
        else if (app.control == FILM_CAM_METERING)
            snprintf(line, sizeof line, "<   %s   >", metering[value]);
        else if (app.control == FILM_CAM_EV_COMP)
            snprintf(line, sizeof line, "%+.2f EV%s", value / 3.0f,
                     app.camera_values[FILM_CAM_METERING] == FILM_METER_MANUAL ? "   (Auto only)" : "");
        else if (app.control == FILM_CAM_EXPOSURE && app.camera_values[FILM_CAM_METERING] != FILM_METER_MANUAL)
            snprintf(line, sizeof line, "%+d active   (%+d saved manual)", app.applied_exposure, value);
        else if (app.control == FILM_CAM_SATURATION) snprintf(line, sizeof line, "<   %d%%   >", 100 + value * 10);
        else snprintf(line, sizeof line, "<   %+d   >", value);
        text_at(20, 116, 0.62f, fg, line);
        text_at(20, 163, 0.53f, gold, "Up/Down: setting   Left/Right: value");
        text_at(20, 183, 0.48f, fg, "Y: back   R: capture");
    } else if (app.ui.page == FILM_VIEW) {
        snprintf(line, sizeof line, "<   Frame  %s   >", aspect_names[app.ui.recipe.aspect]);
        text_at(20, 68, 0.62f, fg, line);
        snprintf(line, sizeof line, "-   %s   %u x %u   +", scale_names[app.ui.recipe.scale],
                 output.width, output.height);
        text_at(20, 116, 0.62f, fg, line);
        snprintf(line, sizeof line, "A/R: Capture   Zoom %ux (Circle Pad)", zoom);
        text_at(20, 163, 0.54f, gold, line);
        text_at(20, 183, 0.48f, fg, "L: lens   Y: camera controls");
    } else if (app.ui.page == FILM_ROLL) {
        snprintf(line, sizeof line, "<   %s %u / %u   >", app.count ? "Photo" : "Virtual",
                 app.ui.selection + 1, app.ui.photos);
        text_at(20, 68, 0.62f, fg, line);
        text_at(20, 163, 0.62f, gold, "A   Open darkroom");
    } else {
        const Look *look = app.ui.look ? &app.luts[app.ui.look - 1] : NULL;
        if (app.darkroom_panel == DARKROOM_GRAIN) snprintf(line, sizeof line, "<   %s Grain: %s   >",
                                       app.grain_reference[app.grain] ? "DxO-fit" : "Procedural",
                                       app.grain_reference[app.grain] ? grain_stock_names[app.grain] : grain_names[app.grain]);
        else if (app.darkroom_panel == DARKROOM_TONE)
            snprintf(line, sizeof line, "<   %s   >", tone_names[app.tone_control]);
        else if (look) snprintf(line, sizeof line, "<   %.*s   >", (int)strlen(look->name) - 5, look->name);
        else if (app.darkroom.lut_id[0]) snprintf(line, sizeof line, "<   %s   >", app.darkroom.lut_id);
        else snprintf(line, sizeof line, "%s", app.lut_count ? "<   Neutral   >" : "No LUTs installed");
        text_at(20, 68, 0.62f, fg, line);
        if (app.darkroom_panel == DARKROOM_GRAIN)
            snprintf(line, sizeof line, "-   Grain %.0f%%   +", app.darkroom.grain_amount * 100);
        else if (app.darkroom_panel == DARKROOM_TONE) {
            float value = app.darkroom.tone.value[app.tone_control];
            if (app.tone_control == FILM_TONE_EV) snprintf(line, sizeof line, "-   %+.2f EV   +", value);
            else snprintf(line, sizeof line, "-   %+.0f%%   +", value * 100);
        } else snprintf(line, sizeof line, "- LUT %.0f%% +   %s grain %.0f%%",
                        app.darkroom.lut_id[0] ? app.darkroom.profile.strength * 100 : 0,
                        grain_names[app.grain], app.darkroom.grain_amount * 100);
        text_at(20, 116, 0.62f, fg, line);
        text_at(20, 163, 0.57f, gold, "A: export   Y: LUT / grain / tone");
        FilmAspect aspect = film_ui_output_aspect(&app.ui, app.saved_aspect);
        if (app.darkroom_panel == DARKROOM_TONE && app.count)
            snprintf(line, sizeof line, "SELECT: save recipe copy   ZL: %s", aspect_names[aspect]);
        else snprintf(line, sizeof line, "ZL: Frame %s   ZR: Halation %s", aspect_names[aspect], app.halation ? "ON" : "OFF");
        text_at(20, 183, 0.48f, fg, line);
    }
    for (unsigned i = 0; i < 3; ++i) {
        C2D_DrawRectSolid(i * 107, 204, 0.1f, 105, 36, i == app.ui.page ? gold : dim);
        text_at(i * 107 + 7, 213, 0.55f, i == app.ui.page ? bg : fg, pages[i]);
    }
    C3D_FrameEnd(0);
}

static void qa_rows(FILE *stream, const char *name)
{
    fprintf(stream, "SECTION %s\n", name);
    for (unsigned i = 0; i < 6; ++i)
        if (app.probe_rows[i][0]) fprintf(stream, "%s\n", app.probe_rows[i]);
    fflush(stream);
}

static void run_hardware_qa(const char *token, bool frames, C3D_RenderTarget *top, C3D_RenderTarget *bottom)
{
    FilmHardwareQA qa = {0};
    app.probe_title = "Remote hardware QA";
    snprintf(app.probe_rows[0], sizeof app.probe_rows[0], "Connecting to upload host...");
    draw(top, bottom);
    if (!film_hardware_qa_open(&qa)) {
        snprintf(app.probe_rows[0], sizeof app.probe_rows[0], "Host connection failed");
        return;
    }
    FILE *stream = qa.output;
    bool new_model = false;
    Result model = APT_CheckNew3DS(&new_model);
    s64 mhz = -1, l2 = -1, luma = -1, value = 0;
    if (svcGetSystemInfo(&value, 0x20000, 0) == 1) {
        if (svcGetSystemInfo(&value, 0x10000, 0) == 0) luma = value;
        if (new_model && svcGetSystemInfo(&value, 0x10001, 0) == 0) mhz = value;
        if (new_model && svcGetSystemInfo(&value, 0x10001, 2) == 0) l2 = value;
    }
    fprintf(stream, "F3QA1 %s\nSECTION system\n", token);
    fprintf(stream, "SYSTEM new_model=%d kernel=%08" PRIx32 " app_mem=%" PRIu32
            " app_free=%" PRIu32 " linear_free=%" PRIu32 " vram_free=%" PRIu32
            " cpu_mhz=%" PRId64 " l2=%" PRId64 " luma=%" PRId64 " wifi=%u\n",
            R_SUCCEEDED(model) ? (int)new_model : -1, osGetKernelVersion(),
            osGetMemRegionSize(MEMREGION_APPLICATION), osGetMemRegionFree(MEMREGION_APPLICATION),
            linearSpaceFree(), vramSpaceFree(), mhz, l2, luma, osGetWifiStrength());
    fflush(stream);
    camera_probe();
    qa_rows(stream, "camera_timing");
    fputs("SECTION camera_color\n", stream);
    fflush(stream);
    film_hardware_qa_camera(stream, frames);
    for (FilmSensor sensor = FILM_CAMERA_REAR1; sensor <= FILM_CAMERA_FRONT && !ferror(stream); ++sensor) {
        static const char *const sections[] = {"native_rear1", "native_rear2", "native_front"};
        native_camera_probe(sensor);
        qa_rows(stream, sections[sensor - FILM_CAMERA_REAR1]);
    }
    if (!ferror(stream)) {
        acceleration_probe();
        qa_rows(stream, "cpu_lut_and_gpu_tile");
        develop_quality_probe();
        qa_rows(stream, "gpu_full_resize");
        layer_quality_probe();
        qa_rows(stream, "gpu_three_layers");
    }
    fputs("DONE\n", stream);
    bool sent = film_hardware_qa_confirm(&qa);
    film_hardware_qa_close(&qa);
    app.probe_title = "Remote hardware QA";
    memset(app.probe_rows, 0, sizeof app.probe_rows);
    snprintf(app.probe_rows[0], sizeof app.probe_rows[0], "%s", sent ? "Report sent to host" : "Report interrupted");
}

int main(int argc, char **argv)
{
    const char *qa_token = NULL;
    bool qa_frames = false;
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "f3qa=", 5) && strlen(argv[i] + 5) == 32 &&
            strspn(argv[i] + 5, "0123456789abcdef") == 32) qa_token = argv[i] + 5;
        if (!strcmp(argv[i], "f3qa-frames")) qa_frames = true;
    }
    gfxInitDefault();
    bool c3d = C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    bool c2d = c3d && C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    if (!c2d) { if (c3d) C3D_Fini(); gfxExit(); return 1; }
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    app.text = C2D_TextBufNew(1024);
    bool texture = C3D_TexInit(&app.texture, PREVIEW_TEX_SIDE, PREVIEW_TEX_SIDE, GPU_RGB8);
    if (!top || !bottom || !app.text || !texture) goto cleanup;
    C3D_TexSetFilter(&app.texture, GPU_NEAREST, GPU_NEAREST);
    memset(app.texture.data, 0, PREVIEW_TEX_SIDE * PREVIEW_TEX_SIDE * 3);
    mkdir("sdmc:/3ds", 0777);
    mkdir(ROOT, 0777);
    mkdir(ROLLS, 0777);
    mkdir(EXPORTS, 0777);
    mkdir(LUTS, 0777);
    bool romfs_mounted = R_SUCCEEDED(romfsInit());
    if (romfs_mounted) {
        scan_looks(PACKED_LUTS);
        for (unsigned i = 0; i < FILM_GRAIN_COUNT; ++i) {
            struct stat profile;
            app.grain_reference[i] = stat(grain_paths[i], &profile) == 0 && S_ISREG(profile.st_mode);
        }
    }
    scan_looks(LUTS);
    struct stat look_file;
    if (app.lut_count < MAX_LUTS && stat(ROOT "look.cube", &look_file) == 0 && S_ISREG(look_file.st_mode))
        app.luts[app.lut_count++] = legacy_look;
    app.log = fopen(ROOT "film3ds.log", "a");
    load_roll();
    app.grain = FILM_GRAIN_MEDIUM;
    if (!qa_token) run_commands();
    FilmFrame preview = {0};
    bool calibration_logged = false;
    if (film_mock_capture(&preview) == FILM_OK) show_image(&preview.source, FILM_601_FULL);
    film_frame_free(&preview);
    update_status("L: camera  R: capture  X: darkroom", FILM_OK, 0);
    if (qa_token) run_hardware_qa(qa_token, qa_frames, top, bottom);

    while (aptMainLoop()) {
        FilmPage previous_page = app.ui.page;
        unsigned previous_photo = app.ui.selection;
        FilmAspect previous_aspect = app.ui.recipe.aspect;
        bool saved = finish_capture(false);
        hidScanInput();
        u32 keys = hidKeysDown();
        if (keys & KEY_START) break;
        if (keys & KEY_ZL) {
            if (app.ui.page == FILM_DARKROOM && (hidKeysHeld() & KEY_SELECT) && !app.save.thread) {
                app.probe_title = "Develop quality";
                memset(app.probe_rows, 0, sizeof app.probe_rows);
                snprintf(app.probe_rows[0], sizeof app.probe_rows[0], "Testing both export sizes...");
                draw(top, bottom);
                develop_quality_probe();
            } else if (app.ui.page == FILM_DARKROOM && !app.probe_title && !(hidKeysHeld() & KEY_SELECT)) {
                FilmAspect current = film_ui_output_aspect(&app.ui, app.saved_aspect);
                film_ui_change_aspect(&app.ui, current, true);
            } else if (app.ui.page != FILM_DARKROOM) camera_probe();
        }
        if (keys & KEY_ZR) {
            if ((hidKeysHeld() & KEY_SELECT) && app.ui.page == FILM_DARKROOM) layer_quality_probe();
            else if (hidKeysHeld() & KEY_SELECT) acceleration_probe();
            else if (app.ui.page == FILM_DARKROOM && !app.probe_title) app.halation = !app.halation;
            else native_camera_probe(app.live ? app.camera.sensor : FILM_CAMERA_REAR1);
        }
        if (keys & KEY_B) app.probe_title = NULL;
        if (app.probe_title) {
            draw(top, bottom);
            continue;
        }
        if (keys & KEY_L) {
            FilmSensor next = app.live ? app.camera.sensor + 1 : FILM_CAMERA_REAR1;
            film_camera_close(&app.camera);
            reset_preview_tracking();
            app.preview_failed = false;
            app.preview_single = false;
            if (next > FILM_CAMERA_FRONT) {
                app.live = false;
                if (film_mock_capture(&preview) == FILM_OK) show_image(&preview.source, FILM_601_FULL);
                film_frame_free(&preview);
                update_status("Simulator", FILM_OK, 0);
            } else {
                FilmStatus status = film_camera_open(&app.camera, FILM_YUYV, next);
                app.live = status == FILM_OK;
                app.preview_failed = !app.live;
                update_status("CAM init", status, app.camera.last_result);
                if (status == FILM_OK && next == FILM_CAMERA_REAR1) {
                    FilmStatus rate = film_camera_request_30fps(&app.camera);
                    film_log(app.log, "Right rear 30 fps", rate, app.camera.last_result);
                }
                if (app.live) {
                    app.applied_exposure = 0;
                    app.camera_yuv = FILM_601_FULL;
                    FilmStatus matrix = film_camera_yuv_matrix(&app.camera, &app.camera_yuv);
                    film_log(app.log, "CAM color matrix", matrix, app.camera.last_result);
                    if (!calibration_logged) {
                        film_camera_log_calibration(app.log);
                        calibration_logged = true;
                    }
                    for (unsigned i = 0; i < FILM_CAM_CONTROL_COUNT; ++i) {
                        if (i == FILM_CAM_SATURATION || i == FILM_CAM_METERING || i == FILM_CAM_EV_COMP ||
                            !app.camera_values[i]) continue;
                        FilmStatus control = film_camera_set_control(&app.camera, i, app.camera_values[i]);
                        if (control != FILM_OK) {
                            update_status(control_names[i], control, app.camera.last_result);
                            app.camera_values[i] = 0;
                        } else if (i == FILM_CAM_EXPOSURE) app.applied_exposure = app.camera_values[i];
                    }
                    app.camera_auto = film_camera_auto_state(&app.camera);
                }
            }
        }
        if ((keys & KEY_SELECT) && !(hidKeysHeld() & (KEY_Y | KEY_ZL | KEY_ZR))) {
            if (app.ui.page == FILM_DARKROOM && app.darkroom_panel == DARKROOM_TONE)
                save_darkroom_recipe();
            else {
                app.ui.recipe.filter = app.ui.recipe.filter == FILM_BILINEAR ? FILM_BICUBIC : FILM_BILINEAR;
                update_status(app.ui.recipe.filter == FILM_BILINEAR ? "Bilinear" : "Bicubic", FILM_OK, 0);
            }
        }
        if ((keys & KEY_Y) && (hidKeysHeld() & KEY_SELECT)) y2r_check();
        else if ((keys & KEY_Y) && app.ui.page == FILM_VIEW) app.camera_controls = !app.camera_controls;
        else if ((keys & KEY_Y) && app.ui.page == FILM_DARKROOM)
            app.darkroom_panel = (app.darkroom_panel + 1) % 3;
        if (keys & KEY_X) film_ui_key(&app.ui, UI_NEXT_PAGE);
        if (keys & KEY_B) {
            if (app.ui.page == FILM_VIEW && app.camera_controls) app.camera_controls = false;
            else film_ui_key(&app.ui, UI_BACK);
        }
        sync_darkroom();
        if (app.ui.page == FILM_VIEW && app.camera_controls) {
            if (keys & KEY_UP) app.control = (app.control + FILM_CAM_CONTROL_COUNT - 1) % FILM_CAM_CONTROL_COUNT;
            if (keys & KEY_DOWN) app.control = (app.control + 1) % FILM_CAM_CONTROL_COUNT;
            if (keys & KEY_LEFT) adjust_camera(-1);
            if (keys & KEY_RIGHT) adjust_camera(1);
        } else if (app.ui.page == FILM_DARKROOM) {
            if (keys & KEY_LEFT) adjust_darkroom(true, -1);
            if (keys & KEY_RIGHT) adjust_darkroom(true, 1);
            if (keys & KEY_UP) adjust_darkroom(false, 1);
            if (keys & KEY_DOWN) adjust_darkroom(false, -1);
        } else {
            if (keys & KEY_LEFT) film_ui_key(&app.ui, UI_LEFT);
            if (keys & KEY_RIGHT) film_ui_key(&app.ui, UI_RIGHT);
            bool live_view = app.ui.page == FILM_VIEW && app.live;
            if (keys & (live_view ? KEY_DUP : KEY_UP)) film_ui_key(&app.ui, UI_UP);
            if (keys & (live_view ? KEY_DDOWN : KEY_DOWN)) film_ui_key(&app.ui, UI_DOWN);
            if (live_view && (keys & (KEY_CPAD_UP | KEY_CPAD_DOWN))) {
                unsigned target = keys & KEY_CPAD_UP ? 2 : 1;
                if (target != app.camera.zoom) {
                    keys &= ~(KEY_A | KEY_R);
                    FilmStatus status = film_camera_set_zoom(&app.camera, target);
                    app.live = app.camera.initialized;
                    reset_preview_tracking();
                    update_status(target == 2 ? "CAM zoom 2x" : "CAM zoom 1x", status, app.camera.last_result);
                }
            }
        }
        FilmAction action = UI_NONE;
        if (keys & KEY_A) action = film_ui_key(&app.ui, UI_ACCEPT);
        if (keys & KEY_TOUCH) {
            touchPosition touch;
            hidTouchRead(&touch);
            if (app.ui.page == FILM_DARKROOM && touch.py >= 57 && touch.py < 154)
                adjust_darkroom(touch.py < 102, touch.px < 160 ? -1 : 1);
            else if (!app.camera_controls || app.ui.page != FILM_VIEW || touch.py >= 154)
                action = film_ui_touch(&app.ui, touch.px, touch.py);
        }
        sync_darkroom();
        bool camera_failed = false;
        FilmFrame shown = {0};
        if (app.live && app.ui.page == FILM_VIEW) {
            if (app.ui.recipe.aspect != previous_aspect) reset_meter();
            FilmStatus status = update_preview(&shown, action != UI_CAPTURE && !(keys & KEY_R));
            if (status != FILM_OK) {
                update_status("CAM preview", status, app.camera.last_result);
                snprintf(app.status, sizeof app.status, "CAM %s %s %08lX",
                         app.preview_single ? "single" : (app.camera.preview_stage ? app.camera.preview_stage : "preview"),
                         film_status_name(status), (unsigned long)app.camera.last_result);
                film_camera_close(&app.camera);
                app.live = false;
                app.preview_failed = true;
                camera_failed = true;
            }
        } else if (app.live && previous_page == FILM_VIEW) {
            FilmStatus status = film_camera_preview_stop(&app.camera);
            reset_preview_tracking();
            if (status != FILM_OK) {
                update_status("CAM stop", status, app.camera.last_result);
                app.live = false;
                app.preview_failed = true;
                camera_failed = true;
            }
        }
        if (!camera_failed && (action == UI_CAPTURE || (keys & KEY_R))) saved = capture_photo(&shown) || saved;
        else if (action == UI_EXPORT && !app.save.thread) {
            snprintf(app.status, sizeof app.status, "%s",
                     app.darkroom.lut_id[0] && app.darkroom.profile.strength > 0 ? "Applying LUT..." : "Developing...");
            draw(top, bottom);
            export_photo();
        }
        film_frame_free(&shown);
        if ((saved || app.ui.page != previous_page || app.ui.selection != previous_photo) &&
            app.ui.page != FILM_VIEW && app.count) {
            char path[64];
            roll_path(path, app.photos[app.ui.selection]);
            FilmStatus status = film_frame_read(path, &preview);
            if (status == FILM_OK) app.saved_aspect = preview.recipe.aspect;
            if (status == FILM_OK && preview.source.format == FILM_YUYV)
                show_camera_image(&preview.source, preview.recipe.yuv, preview.recipe.base_saturation);
            else if (status == FILM_OK) show_image(&preview.source, preview.recipe.yuv);
            else update_status("Photo preview", status, 0);
            film_frame_free(&preview);
        } else if (app.ui.page == FILM_VIEW && previous_page != FILM_VIEW && !app.live) {
            if (film_mock_capture(&preview) == FILM_OK) show_image(&preview.source, FILM_601_FULL);
            film_frame_free(&preview);
        }
        sync_darkroom();
        draw(top, bottom);
    }
    film_camera_close(&app.camera);
    finish_capture(true);
    if (romfs_mounted) romfsExit();
    if (app.log) fclose(app.log);
cleanup:
    C3D_FrameSync();
    if (texture) C3D_TexDelete(&app.texture);
    if (app.text) C2D_TextBufDelete(app.text);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return top && bottom && texture && app.text ? 0 : 1;
}
