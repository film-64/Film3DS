#include "debug.h"
#include "platform_camera.h"
#include "ui.h"

#include <3ds.h>
#include <citro2d.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ROOT "sdmc:/3ds/Film3DS/"
#define ROLLS ROOT "rolls/"
#define EXPORTS ROOT "exports/"
#define VIRTUAL_PHOTOS 3u
#define MAX_PHOTOS 9999u

typedef struct {
    FilmUi ui;
    FilmCamera camera;
    C3D_Tex texture;
    C2D_TextBuf text;
    FILE *log;
    uint16_t photos[MAX_PHOTOS];
    unsigned count, next_number;
    char status[96];
    bool live;
} App;

static App app;
static const char *const aspect_names[] = {"4:3", "3:2", "1:1", "16:9"};
static const char *const scale_names[] = {"Native", "1.5x", "2x"};
static const char *const looks[] = {"Neutral", "Mono", "Warm", "SD LUT"};

static void update_status(const char *operation, FilmStatus status, uint32_t result)
{
    snprintf(app.status, sizeof app.status, "%s  %s", operation, film_status_name(status));
    film_log(app.log, operation, status, result);
}

static FilmStatus init(void *context, FilmPixelFormat format)
{
    return film_camera_open(context, format);
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

static FilmStatus load_look(FilmLut *lut, FilmProfile *profile)
{
    FilmStatus status = film_profile_read(ROOT "look.f3p", profile);
    if (status != FILM_OK) return status;
    FILE *file = fopen(ROOT "look.cube", "rb");
    if (!file) return FILM_IO;
    unsigned line;
    status = film_lut_load(file, lut, &line);
    fclose(file);
    if (status != FILM_OK && app.log) fprintf(app.log, "look.cube line %u\n", line);
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
        if (load_look(&lut, &debug.ui.recipe.profile) == FILM_OK) {
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
    unsigned tile = (x >> 3) + (y >> 3) * 32;
    unsigned offset = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                      ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return tile * 64 + offset;
}

static void show_image(const FilmImage *image, FilmYuv matrix)
{
    C3D_FrameSync();
    uint8_t *tex = app.texture.data;
    for (unsigned y = 0; y < 192; ++y) {
        for (unsigned x = 0; x < 256; ++x) {
            FilmRgb c = film_decode_pixel(image, x * image->width / 256, y * image->height / 192, matrix);
            unsigned i = swizzle(x, 255 - y) * 3;
            tex[i] = (uint8_t)(c.b * 255 + 0.5f);
            tex[i + 1] = (uint8_t)(c.g * 255 + 0.5f);
            tex[i + 2] = (uint8_t)(c.r * 255 + 0.5f);
        }
    }
    C3D_TexFlush(&app.texture);
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
}

static FilmStatus take(FilmFrame *frame)
{
    frame->recipe = app.ui.recipe;
    return app.live ? film_camera_capture(&app.camera, frame) : film_mock_capture(frame);
}

static void capture_photo(void)
{
    FilmFrame frame = {0};
    FilmStatus status = app.next_number > MAX_PHOTOS ? FILM_STATE : take(&frame);
    if (status == FILM_OK) {
        char path[64];
        roll_path(path, app.next_number);
        status = film_frame_write(path, &frame);
        if (status == FILM_OK) {
            show_image(&frame.source, frame.recipe.yuv);
            if (!app.count) app.ui.photos = 0;
            app.photos[app.count++] = app.next_number++;
            film_ui_add_photo(&app.ui);
        }
    }
    update_status("Capture", status, app.camera.last_result);
    film_frame_free(&frame);
}

static void export_photo(void)
{
    FilmFrame frame = {0};
    FilmImage image = {0};
    FilmLut lut = {0};
    unsigned selection = app.ui.selection;
    FilmStatus status;
    if (app.count) {
        char path[64];
        roll_path(path, app.photos[selection]);
        status = film_frame_read(path, &frame);
    } else {
        frame.recipe = app.ui.recipe;
        status = film_mock_capture(&frame);
        frame.recipe.grain_seed += selection;
    }
    if (status == FILM_OK) {
        float strength = app.ui.recipe.profile.strength;
        film_ui_apply_look(&frame.recipe, app.ui.look, strength);
        if (app.ui.look == 3) {
            status = load_look(&lut, &frame.recipe.profile);
            frame.recipe.profile.strength = strength;
            memcpy(frame.recipe.lut_id, "look.cube", sizeof "look.cube");
        }
    }
    if (status == FILM_OK) status = film_develop(&frame, lut.samples ? &lut : NULL, &image);
    if (status == FILM_OK) {
        char path[96];
        struct stat st;
        status = FILM_STATE;
        for (unsigned revision = 1; revision <= MAX_PHOTOS; ++revision) {
            unsigned id = app.count ? app.photos[selection] : selection + 1;
            snprintf(path, sizeof path, EXPORTS "%c%04u-%04u.JPG", app.count ? 'R' : 'V', id, revision);
            if (stat(path, &st) == 0) continue;
            status = film_jpeg_write(path, &image, frame.recipe.jpeg_quality);
            break;
        }
    }
    update_status("Export", status, 0);
    film_lut_free(&lut);
    film_image_free(&image);
    film_frame_free(&frame);
}

static void y2r_check(void)
{
    FilmFrame frame = {0};
    FilmImage cpu = {0}, gpu = {0};
    uint32_t rc = 0;
    FilmStatus status = film_mock_capture(&frame);
    if (status == FILM_OK) status = film_rgb24(&frame.source, FILM_601_FULL, &cpu);
    if (status == FILM_OK) status = film_y2r_convert(&frame.source, FILM_601_FULL, &gpu, &rc);
    if (status == FILM_OK) {
        unsigned max = 0;
        uint64_t sum = 0;
        for (size_t i = 0; i < (size_t)640 * 480 * 3; ++i) {
            unsigned difference = abs((int)gpu.pixels[i] - cpu.pixels[i]);
            if (difference > max) max = difference;
            sum += difference;
        }
        if (app.log) fprintf(app.log, "Y2R comparison max=%u mean=%.3f\n", max, sum / (640.0 * 480 * 3));
        if (max > 4) status = FILM_DEVICE;
    }
    update_status("Y2R", status, rc);
    film_frame_free(&frame);
    film_image_free(&cpu);
    film_image_free(&gpu);
}

static void text_at(float x, float y, float scale, u32 color, const char *message)
{
    C2D_Text text;
    C2D_TextParse(&text, app.text, message);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

static void draw(C3D_RenderTarget *top, C3D_RenderTarget *bottom)
{
    static const Tex3DS_SubTexture sub = {256, 192, 0, 1, 1, 0.25f};
    static const char *const pages[] = {"VIEW", "ROLL", "DARKROOM"};
    u32 bg = C2D_Color32(18, 22, 23, 255), fg = C2D_Color32(241, 235, 216, 255);
    u32 gold = C2D_Color32(248, 193, 107, 255), dim = C2D_Color32(40, 49, 48, 255);
    FilmRect guide = {0};
    film_crop_rect(320, 240, app.ui.recipe.aspect, &guide);
    unsigned w = 0, h = 0;
    FilmRect output = {0};
    film_scaled_dims(640, 480, app.ui.recipe.scale, &w, &h);
    film_crop_rect(w, h, app.ui.recipe.aspect, &output);
    char line[100];
    C2D_TextBufClear(app.text);
    if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
    C2D_TargetClear(top, bg);
    C2D_SceneBegin(top);
    C2D_DrawImageAt((C2D_Image){&app.texture, &sub}, 40, 0, 0, NULL, 1.25f, 1.25f);
    float x = 40 + guide.x, y = guide.y;
    C2D_DrawRectSolid(x, y, 0.1f, guide.width, 1, gold);
    C2D_DrawRectSolid(x, y + guide.height - 1, 0.1f, guide.width, 1, gold);
    C2D_DrawRectSolid(x, y, 0.1f, 1, guide.height, gold);
    C2D_DrawRectSolid(x + guide.width - 1, y, 0.1f, 1, guide.height, gold);
    C2D_DrawRectSolid(48, 10, 0.2f, 303, 23, bg);
    snprintf(line, sizeof line, "FILM3DS   %s   %s   %u x %u",
             app.live ? "CAM" : "SIM", aspect_names[app.ui.recipe.aspect], output.width, output.height);
    text_at(54, 12, 0.57f, fg, line);

    C2D_TargetClear(bottom, bg);
    C2D_SceneBegin(bottom);
    text_at(12, 9, 0.82f, gold, pages[app.ui.page]);
    text_at(12, 34, 0.53f, fg, app.status);
    for (unsigned i = 0; i < 3; ++i) C2D_DrawRectSolid(10, 57 + i * 48, 0.1f, 300, 43, dim);
    if (app.ui.page == FILM_VIEW) {
        snprintf(line, sizeof line, "<   Frame  %s   >", aspect_names[app.ui.recipe.aspect]);
        text_at(20, 68, 0.62f, fg, line);
        snprintf(line, sizeof line, "-   Output  %s   +", scale_names[app.ui.recipe.scale]);
        text_at(20, 116, 0.62f, fg, line);
        text_at(20, 163, 0.62f, gold, "A / R   Capture");
    } else if (app.ui.page == FILM_ROLL) {
        snprintf(line, sizeof line, "<   %s %u / %u   >", app.count ? "Photo" : "Virtual",
                 app.ui.selection + 1, app.ui.photos);
        text_at(20, 68, 0.62f, fg, line);
        text_at(20, 163, 0.62f, gold, "A   Open darkroom");
    } else {
        snprintf(line, sizeof line, "<   Look  %s   >", looks[app.ui.look]);
        text_at(20, 68, 0.62f, fg, line);
        snprintf(line, sizeof line, "-   Strength  %.0f%%   +", app.ui.recipe.profile.strength * 100);
        text_at(20, 116, 0.62f, fg, line);
        text_at(20, 163, 0.62f, gold, "A   Export JPEG");
    }
    for (unsigned i = 0; i < 3; ++i) {
        C2D_DrawRectSolid(i * 107, 204, 0.1f, 105, 36, i == app.ui.page ? gold : dim);
        text_at(i * 107 + 7, 213, 0.55f, i == app.ui.page ? bg : fg, pages[i]);
    }
    C3D_FrameEnd(0);
}

int main(void)
{
    gfxInitDefault();
    bool c3d = C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    bool c2d = c3d && C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    if (!c2d) { if (c3d) C3D_Fini(); gfxExit(); return 1; }
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    app.text = C2D_TextBufNew(1024);
    bool texture = C3D_TexInit(&app.texture, 256, 256, GPU_RGB8);
    if (!top || !bottom || !app.text || !texture) goto cleanup;
    C3D_TexSetFilter(&app.texture, GPU_NEAREST, GPU_NEAREST);
    memset(app.texture.data, 0, 256 * 256 * 3);
    mkdir("sdmc:/3ds", 0777);
    mkdir(ROOT, 0777);
    mkdir(ROLLS, 0777);
    mkdir(EXPORTS, 0777);
    app.log = fopen(ROOT "film3ds.log", "a");
    load_roll();
    run_commands();
    FilmFrame preview = {0};
    if (film_mock_capture(&preview) == FILM_OK) show_image(&preview.source, FILM_601_FULL);
    film_frame_free(&preview);
    update_status("L: CAM   X: page   Y: Y2R", FILM_OK, 0);

    uint64_t next_preview = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 keys = hidKeysDown();
        if (keys & KEY_START) break;
        if (keys & KEY_L) {
            if (app.live) {
                film_camera_close(&app.camera);
                app.live = false;
                if (film_mock_capture(&preview) == FILM_OK) show_image(&preview.source, FILM_601_FULL);
                film_frame_free(&preview);
                update_status("Simulator", FILM_OK, 0);
            } else {
                FilmStatus status = film_camera_open(&app.camera, FILM_YUYV);
                app.live = status == FILM_OK;
                update_status("CAM init", status, app.camera.last_result);
            }
        }
        if (keys & KEY_Y) y2r_check();
        if (keys & KEY_X) film_ui_key(&app.ui, UI_NEXT_PAGE);
        if (keys & KEY_B) film_ui_key(&app.ui, UI_BACK);
        if (keys & KEY_LEFT) film_ui_key(&app.ui, UI_LEFT);
        if (keys & KEY_RIGHT) film_ui_key(&app.ui, UI_RIGHT);
        if (keys & KEY_UP) film_ui_key(&app.ui, UI_UP);
        if (keys & KEY_DOWN) film_ui_key(&app.ui, UI_DOWN);
        FilmAction action = UI_NONE;
        if (keys & KEY_A) action = film_ui_key(&app.ui, UI_ACCEPT);
        if (keys & KEY_TOUCH) {
            touchPosition touch;
            hidTouchRead(&touch);
            action = film_ui_touch(&app.ui, touch.px, touch.py);
        }
        if (action == UI_CAPTURE || (keys & KEY_R)) capture_photo();
        else if (action == UI_EXPORT) export_photo();
        if (app.live && app.ui.page == FILM_VIEW && osGetTime() >= next_preview) {
            FilmStatus status = take(&preview);
            if (status == FILM_OK) show_image(&preview.source, preview.recipe.yuv);
            else {
                update_status("CAM preview", status, app.camera.last_result);
                film_camera_close(&app.camera);
                app.live = false;
            }
            film_frame_free(&preview);
            next_preview = osGetTime() + 250;
        }
        draw(top, bottom);
    }
    film_camera_close(&app.camera);
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
