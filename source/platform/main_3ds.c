#include "debug.h"
#include "platform_camera.h"
#include "platform_develop.h"
#include "platform_qa.h"
#include "ui.h"
#include "darkroom.h"
#include "roll.h"
#include "raw_darkroom.h"
#include "raw_capture.h"
#include "exposure_matrix.h"
#include "gray_capture.h"
#include "layered_grain.h"
#include "lut_catalog.h"
#include "debug_mode.h"

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
#define LOGS ROOT "log/"
#define ROLLS ROOT "rolls/"
#define EXPORTS ROOT "exports/"
#define PREVIEWS ROOT "previews/"
#define LUTS ROOT "luts/"
#define RAWS ROOT "raw/"
#define PACKED_LUTS "romfs:/luts/"
#define VIRTUAL_PHOTOS 3u
#define MAX_PHOTOS FILM_ROLL_MAX_PHOTOS

enum { PREVIEW_TEX_SIDE = 256, PREVIEW_TEX_ROWS = 192 };
enum { ROLL_TEX_SIDE = 64, ROLL_TEX_ROWS = 48, ROLL_VISIBLE_TEXTURES=30 };

typedef FilmLook Look;

enum {CAPTURE_ACQUIRING=1,CAPTURE_SAVING,CAPTURE_DONE};
typedef struct {
    Thread thread;
    Handle completion;
    FilmFrame frame;
    const char *capture_mode;
    unsigned id;
    uint64_t capture_ms, save_ms;
    FilmStatus status, profile_status;
    FilmRollSaveResult result;
    FilmExposureMatrix matrix;
    FilmStatus matrix_status;
    FilmGrayPair gray_pair;
    FilmStatus gray_status;
    bool gray_requested;
    atomic_uint phase;
    unsigned capture_zoom;
    int expected_step;
    FilmStatus capture_status;
} CaptureSave;

typedef struct {
    C3D_Tex texture;
    unsigned id;
    bool allocated, ready;
} RollThumbnail;

typedef struct {
    FilmUi ui;
    FilmRecipe darkroom, saved_recipe;
    unsigned darkroom_id;
    bool darkroom_ready;
    FilmDebugMode debug;
    const Look *capture_look;
    float capture_strength;
    FilmCamera camera;
    FilmCameraAutoState camera_auto;
    FilmAeRegion ae_region; /* requested native VGA metering rectangle */
    FilmYuv camera_yuv;
    C3D_Tex texture;
    RollThumbnail thumbnails[ROLL_VISIBLE_TEXTURES];
    unsigned image_width, image_height,roll_preview_id;
    FilmPixelFormat roll_preview_format;
    FilmAspect saved_aspect;
    C2D_TextBuf text;
    FILE *log;
    uint16_t photos[MAX_PHOTOS];
    unsigned count, next_number;
    CaptureSave save;
    Look *luts;
    unsigned lut_count;
    char status[96];
    char y2r_status[64];
    const char *probe_title;
    char probe_rows[6][72];
    bool live;
    bool preview_failed;
    bool camera_controls;
    bool camera_toolbar;
    FilmDarkroom room;
    FilmRoll roll;
    FilmRollDecode roll_decode;
    FilmRawScene decoded_scene;
    unsigned decoded_id,decoded_calibration;
    FilmRecipe decoded_recipe;
    FilmExposureMatrix shot_matrix;
    unsigned shot_curve_mode; /* 0 neutral, 1 capture-reference approximation */
    unsigned source_width, source_height;
    bool halation;
    FilmGrain grain;
    FilmGrainTexture grain_texture;
    bool grain_gpu;
    float grain_size,grain_shadow,grain_mid,grain_highlight;
    float chroma_denoise;
    unsigned raw_calibration;
    float raw_shadows,raw_midtones,raw_highlights,raw_shoulder,raw_local;
    bool grain_reference[FILM_GRAIN_COUNT];
    unsigned control;
    int camera_values[FILM_CAM_CONTROL_COUNT];
    FilmCameraStability stability;
    int applied_exposure;
    uint64_t preview_period;
    unsigned preview_frames, preview_fps;
    uint64_t last_native_frame_ms;
} App;

static App app;
static const char *const aspect_names[] = {"4:3", "3:2", "1:1", "16:9"};
static const char *const scale_names[] = {"Native", "1.5x", "2x"};
static const char *const sensors[] = {"SIM", "REAR1", "REAR2", "FRONT"};
static const char *const grain_names[] = {"Fine", "Medium", "Coarse"};
static const char *const grain_stock_names[] = {"PAN F+ 50", "Pan 400", "Delta 3200"};
static const char *const grain_paths[] = {"romfs:/grain/fine.f3g", "romfs:/grain/medium.f3g", "romfs:/grain/coarse.f3g"};
static const Look legacy_look = {.directory=ROOT,.name="look.cube",.key="look.cube"};
static const char *const control_names[] = {
    [FILM_CAM_EXPOSURE] = "Manual exposure", [FILM_CAM_EXPOSURE_MODE] = "Exposure mode",
    [FILM_CAM_EV_COMP] = "Exposure compensation", [FILM_CAM_WB] = "White balance",
    [FILM_CAM_CONTRAST] = "Contrast", [FILM_CAM_SATURATION] = "Saturation",
    [FILM_CAM_SHARPNESS] = "Sharpness step"
};
static void update_status(const char *operation, FilmStatus status, uint32_t result);

static void reset_exposure_tracking(void)
{
    app.stability=(FilmCameraStability){0};
}

static void reset_preview_tracking(void)
{
    app.preview_period = 0;
    app.preview_frames = app.preview_fps = 0;
    app.last_native_frame_ms = 0;
    reset_exposure_tracking();
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
    film_log(app.log, "film-camera-exposure/v1", status, app.camera.last_result);
    if (status == FILM_OK) {
        app.applied_exposure = step;
        app.stability = (FilmCameraStability){0};
    }
    return status;
}

/* Recreate Native AE from a known CAMU_SetExposure(0) baseline. Changing
 * compensation while the old YUV DMA is active has failed on real hardware.
 * A failed transition is visible and leaves the camera closed; it never
 * silently promotes an unverified native level to a valid exposure reference. */
static FilmStatus reopen_native_ae(int level,FilmAeRegion requested)
{
    FilmSensor sensor=app.camera.sensor;
    film_camera_close(&app.camera);
    reset_preview_tracking();
    app.live=false;
    FilmStatus s=film_camera_open(&app.camera,FILM_YUYV,sensor);
    if(s==FILM_OK)s=film_camera_request_15fps(&app.camera);
    if(s==FILM_OK)s=film_camera_select_exposure_mode(&app.camera,
        FILM_EXPOSURE_NATIVE_AE,level,app.log);
    if(s==FILM_OK)s=film_camera_set_ae_window(&app.camera,requested);
    if(s==FILM_OK){
        app.camera_yuv=FILM_601_FULL;
        FilmYuv actual=app.camera_yuv;
        FilmStatus color=film_camera_yuv_matrix(&app.camera,&actual);
        if(color==FILM_OK)app.camera_yuv=actual;
        film_log(app.log,"Native AE reset baseline=0, compensation absolute",s,(uint32_t)level);
        app.camera_auto=film_camera_auto_state(&app.camera);
        app.ae_region=requested;
        app.applied_exposure=0;
        app.live=true;app.preview_failed=false;
    }else{
        film_log(app.log,"Native AE reset/reopen FAILED",s,app.camera.last_result);
        film_camera_close(&app.camera);
        app.live=false;app.preview_failed=true;
    }
    return s;
}
static void change_meter_region(FilmAeRegion region)
{
    if(!film_ae_region_valid(region))return;
    if(app.camera_values[FILM_CAM_EXPOSURE_MODE]!=FILM_EXPOSURE_NATIVE_AE||
       !app.live||app.camera.zoom!=1||app.save.thread){
        update_status("AE window requires Native AE, 1x, ready",FILM_STATE,0);return;
    }
    if(app.camera.ae_window_applied&&
       !memcmp(&region,&app.camera.ae_window,sizeof region))return;
    FilmStatus s=reopen_native_ae(app.camera_values[FILM_CAM_EV_COMP],region);
    update_status(s==FILM_OK?"Hardware AE window set":"AE window reset failed; L to reopen",
        s,app.camera.last_result);
}

static void adjust_camera(int change)
{
    if(app.control>FILM_CAM_EV_COMP)return;
    FilmExposureMode mode=(FilmExposureMode)app.camera_values[FILM_CAM_EXPOSURE_MODE];
    if((app.control==FILM_CAM_EXPOSURE&&mode!=FILM_EXPOSURE_MANUAL)||
       (app.control==FILM_CAM_EV_COMP&&mode!=FILM_EXPOSURE_NATIVE_AE))return;
    int value=app.camera_values[app.control]+change;
    int low=app.control==FILM_CAM_EXPOSURE_MODE?0:FILM_EXPOSURE_MIN;
    int high=app.control==FILM_CAM_EXPOSURE_MODE?FILM_EXPOSURE_MANUAL:FILM_EXPOSURE_MAX;
    if(value<low||value>high)return;
    FilmStatus status=FILM_OK;
    if(app.live) {
        if(app.control==FILM_CAM_EXPOSURE_MODE) {
            status=film_camera_select_exposure_mode(&app.camera,(FilmExposureMode)value,app.camera_values[FILM_CAM_EV_COMP],app.log);
            if(status==FILM_OK&&value==FILM_EXPOSURE_NATIVE_AE)
                status=film_camera_set_ae_window(&app.camera,app.ae_region);
            if(status==FILM_OK)app.applied_exposure=0;
        } else if(app.control==FILM_CAM_EV_COMP) {
            /* A fresh camera init resets the native exposure baseline to 0
             * before the requested absolute level is applied once. */
            status=reopen_native_ae(value,app.ae_region);
        } else if(app.camera.exposure.ready)status=apply_exposure(value);
    }
    if(status!=FILM_OK){app.live=app.camera.initialized;if(!app.live)app.preview_failed=true;update_status(control_names[app.control],status,app.camera.last_result);return;}
    app.camera_values[app.control]=value;reset_exposure_tracking();
    if(app.live)app.camera_auto=film_camera_auto_state(&app.camera);
}

static void update_status(const char *operation, FilmStatus status, uint32_t result)
{
    snprintf(app.status, sizeof app.status, "%s  %s", operation, film_status_name(status));
    film_log(app.log, operation, status, result);
}

static FilmStatus init(void *context, FilmPixelFormat format)
{
    FilmStatus status=film_camera_open(context,format,FILM_CAMERA_REAR1);
    if(status==FILM_OK)status=film_camera_request_15fps(context);
    if(status==FILM_OK)status=film_camera_select_exposure_mode(context,FILM_EXPOSURE_NATIVE_AE,0,app.log);
    if(status==FILM_OK)status=film_camera_prepare_raw(context,app.log);
    return status;
}

static FilmStatus capture(void *context, FilmFrame *frame)
{
    film_log(app.log,"film-camera-command-raw/v1",FILM_OK,0);
    return film_camera_capture_raw(context,frame,app.log);
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

static void scan_looks(const char *directory)
{
    FilmLookCatalog catalog={0};FilmStatus status=film_lut_catalog_scan("film-lut-catalog/v1",directory,&catalog);
    film_log(app.log,"film-lut-catalog/v1",status,0);
    if(status==FILM_OK){free(app.luts);app.luts=catalog.looks;app.lut_count=catalog.count;}
    else update_status("LUT directory",status,0);
}

static unsigned find_look(const char *id)
{
    for (unsigned i = 0; i < app.lut_count; ++i)
        if (!strcmp(id, app.luts[i].key)) return i + 1;
    return 0;
}

static FilmStatus load_look_lut(const Look *look, FilmLut *lut)
{
    char path[768];
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
            .jpeg_path = ROOT "debug.jpg", .source_path = ROOT "debug.DNG", .ui = film_ui_new(3)};
        if (load_look_lut(&legacy_look, &lut) == FILM_OK) {
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

static unsigned swizzle(unsigned x, unsigned y, unsigned side)
{
    unsigned tile = (x >> 3) + (y >> 3) * (side / 8);
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
    app.roll_preview_id=0;
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
            unsigned i = swizzle(x, y, PREVIEW_TEX_SIDE) * 3;
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
    app.roll_preview_id=0;
    if(image->format==FILM_RAW10_RGGB) {
        FilmImage rgb={0};FilmStatus s=film_rgb24(image,matrix,&rgb);
        if(s==FILM_OK)show_image(&rgb,matrix);else update_status("RAW preview",s,0);
        film_image_free(&rgb);return;
    }
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
            unsigned i = swizzle(x, y, PREVIEW_TEX_SIDE) * 3;
            tex[i] = (uint8_t)(c.b * 255 + 0.5f);
            tex[i + 1] = (uint8_t)(c.g * 255 + 0.5f);
            tex[i + 2] = (uint8_t)(c.r * 255 + 0.5f);
        }
    }
    C3D_TexFlush(&app.texture);
    app.image_width = image->width;
    app.image_height = image->height;
}

static void show_camera_closed(void)
{
    uint8_t black[3]={0};FilmImage image={1,1,FILM_RGB24,black};show_image(&image,FILM_601_FULL);
}

static void roll_path(char path[64], unsigned id)
{
    film_roll_original_path(path,64,ROLLS,id);
}

/* ROLL_PREVIEW_BEGIN */
static void roll_prepare_grid(void)
{
    if(app.ui.page!=FILM_ROLL)return;
    C3D_FrameSync();
    for(unsigned slot=0;slot<ROLL_VISIBLE_TEXTURES;++slot){
        unsigned index=app.roll.first+slot,id=index<app.count?app.photos[index]:0;
        RollThumbnail *thumb=&app.thumbnails[slot];if(thumb->id==id)continue;
        thumb->id=id;thumb->ready=false;if(!id||!thumb->allocated)continue;
        FilmImage image={0};FilmStatus status=film_roll_jpeg_preview("film-roll-jpeg-preview/v1",PREVIEWS,id,NULL,&image);
        if(status==FILM_OK){
            memset(thumb->texture.data,0,ROLL_TEX_SIDE*ROLL_TEX_SIDE*3);
            for(unsigned y=0;y<ROLL_TEX_ROWS;++y)for(unsigned x=0;x<ROLL_TEX_SIDE;++x){
                const uint8_t *src=image.pixels+((size_t)(y*image.height/ROLL_TEX_ROWS)*image.width+x*image.width/ROLL_TEX_SIDE)*3;
                uint8_t *dst=(uint8_t *)thumb->texture.data+swizzle(x,y,ROLL_TEX_SIDE)*3;
                dst[0]=src[2];dst[1]=src[1];dst[2]=src[0];
            }
            C3D_TexFlush(&thumb->texture);thumb->ready=true;
        }
        film_image_free(&image);
        break; /* At most one visible thumbnail decode per UI iteration. */
    }
}
static void roll_show_selection(void)
{
    if(!film_runtime_source_valid("film-roll-preview/v1")){update_status("Roll preview",FILM_STATE,0);return;}
    film_roll_sync(&app.roll,&app.ui);
    if(!app.count){app.image_width=app.image_height=app.roll_preview_id=0;app.roll_preview_format=0;return;}
    unsigned id=app.photos[app.ui.selection];
    if(app.decoded_id&&app.decoded_id!=id){film_raw_scene_free(&app.decoded_scene);app.decoded_id=app.darkroom_id=0;}
    if(app.roll_preview_id==id)return;
    FilmImage image={0};FilmRollPair pair={0};
    FilmStatus status=film_roll_jpeg_preview("film-roll-jpeg-preview/v1",PREVIEWS,id,&pair,&image);
    app.roll_preview_format=status==FILM_OK?pair.format:0;
    if(status==FILM_OK)show_image(&image,FILM_601_FULL);
    else{app.image_width=app.image_height=0;update_status("Not decoded; SELECT in Roll",status,0);}
    app.roll_preview_id=id;film_image_free(&image);
}

static void roll_delete_input(u32 keys)
{
    FilmRollDeleteInput input=film_roll_delete_input(keys,KEY_A,KEY_X,
        KEY_A|KEY_B|KEY_X|KEY_Y|KEY_L|KEY_R|KEY_ZL|KEY_ZR|KEY_START|KEY_SELECT);
    if(input==ROLL_DELETE_CANCEL){film_roll_cancel_delete(&app.roll);return;}
    if(input!=ROLL_DELETE_CONFIRM)return;
    unsigned id=app.roll.pending_id;
    FilmStatus status=app.save.thread?FILM_STATE:
        film_roll_confirm_delete(&app.roll,&app.ui,app.photos,&app.count,ROLLS,PREVIEWS);
    int delete_errno=status==FILM_IO?errno:0;
    if (status==FILM_OK) {
        app.roll_preview_id=0;app.roll_decode.decode_selection[id]=false;
        film_raw_scene_free(&app.decoded_scene);app.decoded_id=0;
        film_darkroom_discard(&app.room);app.darkroom_id=0;app.darkroom_ready=false;
        roll_show_selection();
        snprintf(app.status,sizeof app.status,"Original deleted; %u photos",app.count);
    } else {
        film_roll_cancel_delete(&app.roll);
        snprintf(app.status,sizeof app.status,"Delete #%04u %s errno=%d",id,film_status_name(status),delete_errno);
        film_log(app.log,app.status,status,delete_errno);
    }
}
/* ROLL_PREVIEW_END */

static void load_roll(void)
{
    bool exists[MAX_PHOTOS + 1] = {false};
    DIR *directory = opendir(ROLLS);
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            unsigned id;
            char suffix;
            if (strlen(entry->d_name) == 12 && sscanf(entry->d_name, "ROLL%4u%*4s%c", &id, &suffix) == 1 &&
                id > 0 && id <= MAX_PHOTOS && (!strcmp(entry->d_name + 8, ".F3S")||!strcmp(entry->d_name + 8, ".DNG"))) exists[id] = true;
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
    app.ui = film_ui_new(app.count);
    app.ui.looks = app.lut_count + 1;
    app.capture_strength = 1;
    if (app.lut_count) app.capture_look=&app.luts[0];
}

static void import_raw_on_device(void)
{
    if(app.save.thread)return;
    DIR *dir=opendir(RAWS);if(!dir){update_status("RAW folder",FILM_IO,0);return;}
    struct dirent *e;FilmStatus status=FILM_STATE;unsigned imported=0;
    while((e=readdir(dir))) {
        unsigned id;char tail;
        if(strlen(e->d_name)!=11||sscanf(e->d_name,"RAW%4u.BIN%c",&id,&tail)!=1||!id||id>MAX_PHOTOS||strcmp(e->d_name+7,".BIN"))continue;
        char output[64],input[96];roll_path(output,id);struct stat st;
        if(!stat(output,&st))continue;
        snprintf(input,sizeof input,RAWS "%s",e->d_name);
        status=film_raw_import("film-app-raw-import/v1",input,output);
        film_log(app.log,"film-app-raw-import/v1",status,0);
        if(status==FILM_OK)imported=id;
        break;
    }
    closedir(dir);
    if(imported){app.count=0;load_roll();app.ui.page=FILM_ROLL;app.darkroom_id=0;roll_show_selection();}
    update_status(imported?"RAW imported; SELECT decodes":"RAW import (RAWnnnn.BIN)",status,0);
}

static FilmStatus open_selected_original(FilmFrame *frame)
{
    if(!app.count||app.ui.selection>=app.count)return FILM_STATE;
    unsigned id=app.photos[app.ui.selection];FilmRollPair pair={0};
    FilmStatus status=film_roll_pair_index("film-roll-pair-index/v1",PREVIEWS,id,&pair);
    char name[64];roll_path(name,id);
    if(status==FILM_OK)status=film_frame_read(name,frame);
    if(status==FILM_OK)status=film_roll_pair_verify("film-roll-original-open/v1",frame,&pair);
    if(status!=FILM_OK)film_frame_free(frame);
    return status;
}
static void prepare_selected_preview(void)
{
    if(app.save.thread||!app.count)return;
    FilmStatus status=film_roll_decode_begin(&app.roll_decode,app.photos,app.count,app.ui.selection);
    if(status!=FILM_OK)update_status("Decode selection",status,0);
    else{film_darkroom_discard(&app.room);app.darkroom_id=0;app.darkroom_ready=false;}
}

static void decode_roll_step(void)
{
    FilmRollDecode *batch=&app.roll_decode;
    unsigned id=batch->decode_ids[batch->decode_position];
    film_raw_scene_free(&app.decoded_scene);app.decoded_id=0;
    bool selected=id==app.photos[app.ui.selection];
    FilmStatus status=film_roll_decode("film-roll-decode/v1",ROLLS,PREVIEWS,id,
        selected?&app.decoded_scene:NULL,selected?&app.decoded_recipe:NULL);
    if(status==FILM_OK&&app.decoded_scene.pixels){app.decoded_id=id;app.decoded_calibration=0;}
    film_roll_decode_advance(batch,status==FILM_OK);
    app.darkroom_id=0;
    if(app.photos[app.ui.selection]==id)app.roll_preview_id=0;
    for(unsigned i=0;i<ROLL_VISIBLE_TEXTURES;++i)if(app.thumbnails[i].id==id)app.thumbnails[i].id=0;
    roll_show_selection();
    if(status!=FILM_OK)update_status("Decode failed; original kept",status,0);
    else if(!batch->decode_active)update_status("Decoded; A: darkroom",FILM_OK,0);
}

static void apply_raw_curve_preset(unsigned selected)
{
    const FilmExposureCurve *curve=selected?&app.shot_matrix.shot:&app.shot_matrix.neutral;
    FilmDevelopOptions options={0};
    if(film_exposure_curve_apply(curve,&app.darkroom,&options)!=FILM_OK)return;
    app.raw_shadows=options.raw_shadows;
    app.raw_midtones=options.raw_midtones;
    app.raw_highlights=options.raw_highlights;
    app.raw_shoulder=options.raw_shoulder;
    app.raw_local=0;
    app.shot_curve_mode=selected;
}

static void sync_darkroom(void)
{
    if (app.ui.page != FILM_DARKROOM) return;
    if(!app.count){film_darkroom_discard(&app.room);app.darkroom_id=0;app.darkroom_ready=false;app.source_width=app.source_height=0;return;}
    unsigned id = app.count ? app.photos[app.ui.selection] : MAX_PHOTOS + app.ui.selection + 1;
    if (id == app.darkroom_id) return;
    FilmFrame frame = {.recipe = film_default_recipe()};
    FilmStatus status = FILM_OK;
    if(app.decoded_id==id&&app.decoded_scene.pixels){
        frame.recipe=app.decoded_recipe;app.roll_preview_format=FILM_RAW10_RGGB;
        frame.source.width=app.decoded_scene.width;frame.source.height=app.decoded_scene.height;
    }else if (app.count) {
        film_raw_scene_free(&app.decoded_scene);app.decoded_id=0;
        FilmRollPair pair={0};
        status=film_roll_pair_index("film-roll-pair-index/v1",PREVIEWS,id,&pair);
        if(status==FILM_OK)app.roll_preview_format=pair.format;
        if(status==FILM_OK&&pair.format==FILM_RAW10_RGGB){
            status=film_roll_decode_load("film-roll-decode-load/v1",ROLLS,PREVIEWS,id,&app.decoded_scene);
            if(status==FILM_OK){
                char name[64];roll_path(name,id);
                /* Standard DNG has no private recipe. Legacy F3S retains it. */
                if(!strcasecmp(name+strlen(name)-4,".F3S"))status=open_selected_original(&frame);
                else frame.recipe=film_default_recipe();
                if(status==FILM_OK){app.decoded_id=id;app.decoded_calibration=0;app.decoded_recipe=frame.recipe;frame.source.width=pair.width;frame.source.height=pair.height;}
            }
        }else if(status==FILM_OK)status=open_selected_original(&frame);
    } else {
        frame.recipe = app.ui.recipe;
        frame.recipe.grain_amount = .3f;
        frame.recipe.grain_seed += app.ui.selection;
        if (app.capture_look) {
            frame.recipe.profile=film_default_recipe().profile;
            if (status == FILM_OK) {
                strcpy(frame.recipe.lut_id, app.capture_look->key);
                frame.recipe.profile.strength = app.capture_strength;
            }
        }
    }
    film_darkroom_discard(&app.room);
    app.source_width = frame.source.width ? frame.source.width : 640;
    app.source_height = frame.source.height ? frame.source.height : 480;
    app.darkroom_id = id;
    app.darkroom_ready = status == FILM_OK;
    app.darkroom = app.saved_recipe = frame.recipe;
    app.shot_matrix=film_exposure_matrix_default();
    app.shot_curve_mode=0;
    /* The DNG has no private recipe. The CRC-bound F3EM sidecar records
     * both identity and an explicitly approximate native-preview match.
     * Missing/invalid metadata never prevents opening the original RAW. */
    if(status==FILM_OK && app.roll_preview_format==FILM_RAW10_RGGB){
        char original[64];roll_path(original,id);
        size_t n=strlen(original);
        if(n>=4&&!strcasecmp(original+n-4,".DNG")){
            FilmRollPair pair={0};
            FilmStatus ps=film_roll_pair_index("film-roll-pair-index/v1",PREVIEWS,id,&pair);
            if(ps==FILM_OK&&pair.format==FILM_RAW10_RGGB){
                FilmStatus ms=film_exposure_matrix_read("film-exposure-matrix-read/v1",PREVIEWS,id,pair.source_crc,&app.shot_matrix);
                if(ms!=FILM_OK){
                    app.shot_matrix=film_exposure_matrix_default();
                    if(ms==FILM_CORRUPT)update_status("Curve metadata invalid; Neutral",ms,0);
                }
            }
            apply_raw_curve_preset(app.shot_matrix.fitted?1u:0u);
            app.saved_recipe=app.darkroom;
        }
    }
    app.saved_aspect = frame.recipe.aspect;
    app.ui.look = find_look(frame.recipe.lut_id);
    if (status != FILM_OK){
        film_raw_scene_free(&app.decoded_scene);app.decoded_id=0;
        update_status("Roll SELECT: decode first", status, 0);
    }
    film_frame_free(&frame);
}

static void change_darkroom_look(int direction)
{
    unsigned index = (app.ui.look + app.ui.looks + direction) % app.ui.looks;
    const Look *look = index ? &app.luts[index - 1] : NULL;
    const char *id = look ? look->key : "";
    if (!strcmp(id, app.darkroom.lut_id)) return;
    FilmProfile preset = film_default_recipe().profile;
    film_ui_select_look(&app.darkroom, &app.saved_recipe, id, &preset);
    app.ui.look = index;

}

static void adjust_darkroom(int direction)
{
    if (!app.darkroom_ready) return;
    if (app.room.row==DARKROOM_WB_SHIFT) {
        app.room.pending_wb=app.darkroom.white_balance;
        app.room.wb_editor=true;
        return;
    }
    bool geometry=film_darkroom_geometry_row(app.room.row);
    if (!geometry) film_darkroom_discard(&app.room);
    if (app.room.row==DARKROOM_COMPOSITION)
        film_composition_percent(&app.darkroom,app.source_width,app.source_height,direction);
    else if (app.room.row == DARKROOM_LOOK) change_darkroom_look(direction);
    else if (app.room.row == DARKROOM_GRAIN_KIND)
        app.grain = (app.grain + FILM_GRAIN_COUNT + direction) % FILM_GRAIN_COUNT;
    else if(app.room.row==DARKROOM_GRAIN_BACKEND)app.grain_gpu=!app.grain_gpu;
    else if(app.room.row==DARKROOM_GRAIN_SIZE)app.grain_size=fminf(4,fmaxf(.25f,app.grain_size+direction*.25f));
    else if(app.room.row==DARKROOM_GRAIN_SHADOW)app.grain_shadow=fminf(2,fmaxf(0,app.grain_shadow+direction*.1f));
    else if(app.room.row==DARKROOM_GRAIN_MID)app.grain_mid=fminf(2,fmaxf(0,app.grain_mid+direction*.1f));
    else if(app.room.row==DARKROOM_GRAIN_HIGH)app.grain_highlight=fminf(2,fmaxf(0,app.grain_highlight+direction*.1f));
    else if(app.room.row==DARKROOM_RAW_CALIBRATION)app.raw_calibration=(app.raw_calibration+3+direction)%3;
    else if(app.room.row==DARKROOM_RAW_CURVE){
        if(app.roll_preview_format==FILM_RAW10_RGGB){
            unsigned target=app.shot_curve_mode==2?(direction>0?1u:0u):1u-app.shot_curve_mode;
            if(target&&!app.shot_matrix.fitted)update_status("Shot reference unavailable; Neutral",FILM_STATE,0);
            else apply_raw_curve_preset(target);
        }
    }
    else if(app.room.row==DARKROOM_RAW_SHADOWS)app.raw_shadows=fminf(2.5f,fmaxf(-2.5f,app.raw_shadows+.5f*direction));
    else if(app.room.row==DARKROOM_RAW_MIDTONES)app.raw_midtones=fminf(2.5f,fmaxf(-2.5f,app.raw_midtones+.5f*direction));
    else if(app.room.row==DARKROOM_RAW_HIGHLIGHTS)app.raw_highlights=fminf(2.5f,fmaxf(-2.5f,app.raw_highlights+.5f*direction));
    else if(app.room.row==DARKROOM_RAW_SHOULDER)app.raw_shoulder=fminf(.95f,fmaxf(0,roundf(app.raw_shoulder*20+direction)/20));
    else if(app.room.row==DARKROOM_RAW_LOCAL)app.raw_local=fminf(1,fmaxf(0,app.raw_local+.1f*direction));
    else if(app.room.row==DARKROOM_DENOISE)app.chroma_denoise=fminf(1,fmaxf(0,app.chroma_denoise+direction*.1f));
    else if (app.room.row == DARKROOM_HALATION) app.halation = !app.halation;
    else film_darkroom_adjust(&app.darkroom, app.room.row, direction);
    if(app.roll_preview_format==FILM_RAW10_RGGB &&
       (app.room.row==DARKROOM_EXPOSURE||app.room.row==DARKROOM_RAW_SHADOWS||
        app.room.row==DARKROOM_RAW_MIDTONES||app.room.row==DARKROOM_RAW_HIGHLIGHTS||
        app.room.row==DARKROOM_RAW_SHOULDER||app.room.row==DARKROOM_RAW_LOCAL))
        app.shot_curve_mode=2; /* Edits are independent of saved presets. */
    if (geometry) film_darkroom_update_geometry(&app.room,&app.darkroom);
}

static FilmStatus update_preview(void)
{
    bool was_prepared=app.camera.raw_preparation.attempted;
    FilmImage image={0};FilmStatus status=film_camera_preview_start(&app.camera);
    if(status==FILM_OK)status=film_camera_preview_poll(&app.camera,&image);
    if(status!=FILM_OK){film_log(app.log,"film-camera-native-preview/v1",status,app.camera.last_result);reset_preview_tracking();}
    if(!was_prepared&&app.camera.raw_preparation.attempted)
        update_status(app.camera.raw_preparation.ready?"RAW camera ready":"RAW preparation failed",app.camera.raw_preparation.status,0);
    if (status == FILM_OK && image.pixels) {
        FilmExposureMode mode=(FilmExposureMode)app.camera_values[FILM_CAM_EXPOSURE_MODE];
        film_camera_sample_preview(&app.stability,&image,app.camera_yuv,app.ui.recipe.aspect,FILM_METER_MANUAL);
        app.last_native_frame_ms=osGetTime();
        show_camera_image(&image, app.camera_yuv, 0);
        /* Native preview has already rearmed DMA. Exposure transitions are a
         * separate boundary; a failed setup cannot turn a valid frame into a
         * camera-open failure. No register polling once exposure is ready. */
        if (app.camera.exposure.active && !app.camera.exposure.ready && !app.camera.exposure.failed) {
            FilmStatus exposure = film_camera_progress_manual_exposure(&app.camera);
            if (exposure != FILM_OK) update_status("Manual exposure", exposure, app.camera.last_result);
        }
        if (app.camera.exposure.ready && mode == FILM_EXPOSURE_MANUAL &&
            app.applied_exposure != app.camera_values[FILM_CAM_EXPOSURE]) {
            FilmStatus exposure = apply_exposure(app.camera_values[FILM_CAM_EXPOSURE]);
            if (exposure != FILM_OK) update_status("Manual exposure", exposure, app.camera.last_result);
        }
        uint64_t now = osGetTime();
        if (!app.preview_period) app.preview_period = now;
        else if (now - app.preview_period >= 1000) {
            ++app.preview_frames;
            app.preview_fps = (app.preview_frames * UINT64_C(1000)) / (now - app.preview_period);
            app.preview_period = now;
            app.preview_frames = 0;
        } else ++app.preview_frames;

    }
    return status;
}

static void save_capture(void *argument)
{
    CaptureSave *save=argument;uint64_t started=ticks(NULL);
    atomic_store(&app.camera.capture_phase,FILM_CAPTURE_VERIFY);
    if(app.log&&atomic_load(&app.camera.debug_mode)){fprintf(app.log,"RAW acquisition begin mode=%u expected_step=%d\n",app.camera.exposure_mode,save->expected_step);fflush(app.log);}
    FilmStatus status=FILM_OK;
    if(app.camera.exposure_mode==FILM_EXPOSURE_MANUAL&&app.camera.exposure.step!=save->expected_step)status=FILM_STATE;
    if(status==FILM_OK)status=film_camera_capture_raw(&app.camera,&save->frame,app.log);
    bool valid=save->frame.source.pixels&&save->frame.source.format==FILM_RAW10_RGGB;
    if(status==FILM_OK&&!valid)status=FILM_CORRUPT;
    if(status!=FILM_OK)film_camera_close(&app.camera);
    save->capture_status=status;
    film_log(app.log,"RAW acquisition result",status,app.camera.last_result);
    if(!valid){film_frame_free(&save->frame);save->status=status;atomic_store(&save->phase,CAPTURE_DONE);return;}
    save->capture_ms=(ticks(NULL)-started)/1000;
    atomic_store(&save->phase,CAPTURE_SAVING);started=ticks(NULL);
    char path[64];snprintf(path,sizeof path,ROLLS "ROLL%04u.DNG",save->id);
    if(app.log){fprintf(app.log,"SAVE original begin path=%s\n",path);fflush(app.log);}
    save->status=film_roll_capture_save("film-roll-capture-save/v1",&save->frame,path,PREVIEWS,save->id,&save->result);
    if(app.log){fprintf(app.log,"SAVE result written=%u write=%s verify=%s preview=%s\n",save->result.written,film_status_name(save->result.write_status),film_status_name(save->result.verification_status),film_status_name(save->result.preview_status));fflush(app.log);}
    film_log(app.log,"Shutter original SD CRC",save->result.verification_status,save->id);
    if(save->status==FILM_OK&&save->capture_status==FILM_OK){
        save->matrix.id=save->id;
        save->matrix.source_crc=film_roll_source_crc(&save->frame.source);
        save->matrix_status=film_exposure_matrix_write("film-exposure-matrix-write/v1",PREVIEWS,&save->matrix);
        film_log(app.log,"Capture native-preview curve reference",save->matrix_status,save->id);
        if(save->gray_requested){
            save->gray_pair.id=save->id;
            save->gray_pair.raw_source_crc=save->matrix.source_crc;
            save->gray_status=film_gray_pair_write("film-gray-pair-write/v1",PREVIEWS,&save->gray_pair);
            if(save->gray_status==FILM_OK)
                save->gray_status=film_gray_pair_verify("film-gray-pair-verify/v1",PREVIEWS,
                    save->id,save->gray_pair.raw_source_crc);
            film_log(app.log,"Gray preview YUV / DNG paired evidence",save->gray_status,save->id);
        }
    }
    save->save_ms=(ticks(NULL)-started)/1000;atomic_store(&save->phase,CAPTURE_DONE);
}

static bool finish_capture(bool wait)
{
    CaptureSave *save = &app.save;
    if (!save->thread && !save->frame.source.pixels) return false;
    if (save->thread) {
        u64 timeout = wait ? U64_MAX : 0;
        Result joined = save->completion == UINT32_MAX ? threadJoin(save->thread, timeout) :
                                                       svcWaitSynchronization(save->completion, timeout);
        if (joined != 0) return false;
        threadFree(save->thread);
    }
    app.live=app.camera.initialized;app.preview_failed=!app.live;
    if(app.live&&app.camera.exposure_mode==FILM_EXPOSURE_NATIVE_AE){
        /* Raw transaction temporarily switches AE off; reassert the
         * explicitly programmed window before restarting any preview. */
        FilmStatus roi=film_camera_set_ae_window(&app.camera,app.ae_region);
        if(roi!=FILM_OK){
            film_log(app.log,"Post-RAW AE window restore",roi,app.camera.last_result);
            film_camera_close(&app.camera);
            app.live=false;app.preview_failed=true;
        }
    }
    reset_preview_tracking();
    if(save->capture_status!=FILM_OK)update_status("RAW capture/restore; L reopens",save->capture_status,app.camera.last_result);
    if(!save->frame.source.pixels){free(save->gray_pair.preview.pixels);*save=(CaptureSave){0};return false;}
    if (save->profile_status != FILM_OK)
        film_log(app.log, "Capture profile", save->profile_status, 0);
    if (save->result.written) ++app.next_number;
    bool saved = save->result.written;
    if (saved) {
        if (!app.count) app.ui.photos = 0;
        app.photos[app.count++] = save->id;
        film_ui_add_photo(&app.ui);
        snprintf(app.status, sizeof app.status, "%s #%04u %" PRIu64 "ms %s SD %" PRIu64 "ms CRC %s",
                 save->frame.camera ? "CAM" : "SIM", save->id, save->capture_ms,
                 save->capture_mode, save->save_ms, save->result.verification_status==FILM_OK?"OK":"FAILED");
        film_log(app.log, app.status, FILM_OK, 0);
        if(save->status!=FILM_OK)update_status("Original kept; SD verify failed",save->status,0);
        else if(save->gray_requested&&save->gray_status!=FILM_OK)
            update_status("Original kept; gray evidence incomplete",save->gray_status,0);
        else if(save->matrix_status!=FILM_OK)update_status("Original saved; look reference missing",save->matrix_status,0);
        else if(save->gray_requested)update_status("Gray pair saved: DNG+F3Y+F3E",FILM_OK,0);
        else update_status("Original saved; Roll SELECT decodes",FILM_OK,0);

    } else update_status(save->result.written ? "Capture SD verify" : "Capture SD write", save->status, 0);
    film_frame_free(&save->frame);
    free(save->gray_pair.preview.pixels);
    *save = (CaptureSave){0};
    return saved;
}

static bool capture_photo(bool gray_pair)
{
    CaptureSave *save=&app.save;
    if(save->thread){update_status("RAW capture/save busy",FILM_STATE,0);return false;}
    FilmStatus allowed=film_camera_raw_shutter_check("film-camera-shutter-raw/v1",&app.camera);
    if(allowed!=FILM_OK)film_log(app.log,"film-camera-shutter-raw/v1",allowed,app.next_number);
    if(allowed!=FILM_OK){update_status(allowed==FILM_UNSUPPORTED?"Lens cannot capture RAW":"RAW camera not ready; L opens",allowed,0);return false;}
    if(app.preview_failed){update_status("Camera failed: reopen with L",FILM_STATE,0);return false;}
    if(app.next_number>MAX_PHOTOS){update_status("Capture",FILM_STATE,0);return false;}
    film_raw_scene_free(&app.decoded_scene);app.decoded_id=app.darkroom_id=0;
    save->capture_status=FILM_STATE;save->capture_zoom=app.camera.zoom;
    save->expected_step=app.camera_values[FILM_CAM_EXPOSURE];
    save->id=app.next_number;save->frame.recipe=film_ui_capture_recipe(&app.ui);
    save->matrix=film_exposure_matrix_default();
    save->matrix.exposure_mode=(unsigned)app.camera_values[FILM_CAM_EXPOSURE_MODE];
    save->matrix.native_level=save->matrix.exposure_mode==FILM_EXPOSURE_NATIVE_AE?
        app.camera_values[FILM_CAM_EV_COMP]:0;
    /* Legacy F3E fit samples the whole RAW scene. Do not fit a partial
     * metering ROI against full-scene RAW quantiles; new F3YG v2 carries the
     * precise hardware ROI for later paired Display-gray calibration. */
    save->matrix.preview_valid=app.stability.preview_valid&&
        app.camera.ae_window_applied&&film_ae_region_is_full(app.camera.ae_window);
    if(save->matrix.preview_valid)memcpy(save->matrix.preview_linear,app.stability.preview_linear,
        sizeof save->matrix.preview_linear);
    if(gray_pair){
        /* The last finished DMA slot is idle while the next slot is armed.
         * Copy it on the UI thread BEFORE the RAW shutter worker stops CAM.
         * No camera register writes, reshoots or background acquisition. */
        uint64_t now=osGetTime();
        if(app.camera.exposure_mode!=FILM_EXPOSURE_NATIVE_AE||app.camera.zoom!=1||
           app.camera.format!=FILM_YUYV||!app.camera.preview_ready||
           !app.last_native_frame_ms||now<app.last_native_frame_ms||
           now-app.last_native_frame_ms>2500||!app.stability.preview_valid||
           !app.camera.ae_window_applied||
           !app.camera.preview_dma[app.camera.preview_pending^1]){
            update_status("Gray pair: need fresh Native AE VGA preview",FILM_STATE,0);
            *save=(CaptureSave){0};return false;
        }
        FilmCameraAutoState state=film_camera_auto_state(&app.camera);
        if(R_FAILED(state.exposure_result)||R_FAILED(state.white_balance_result)||!state.exposure){
            update_status("Gray pair: AE readback failed",FILM_STATE,state.exposure_result);
            *save=(CaptureSave){0};return false;
        }
        uint8_t *pixels=malloc(640u*480u*2u);
        if(!pixels){update_status("Gray pair: memory",FILM_NOMEM,0);*save=(CaptureSave){0};return false;}
        memcpy(pixels,app.camera.preview_dma[app.camera.preview_pending^1],640u*480u*2u);
        save->gray_requested=true;
        save->gray_pair=(FilmGrayPair){
            .preview={640,480,FILM_YUYV,pixels},.yuv=app.camera_yuv,
            .meter_region=app.camera.ae_window,
            .preview_time_ms=app.last_native_frame_ms,
            .native_level=app.camera_values[FILM_CAM_EV_COMP],
            .exposure_mode=FILM_EXPOSURE_NATIVE_AE,
            .native_ae=state.exposure,.native_awb=state.white_balance};
    }
    save->capture_mode=gray_pair?"RAW+GRAY":"RAW DNG";
    atomic_store(&app.camera.capture_phase,FILM_CAPTURE_VERIFY);atomic_store(&save->phase,CAPTURE_ACQUIRING);
    snprintf(app.status,sizeof app.status,"Capturing RAW #%04u",save->id);
    s32 priority=0x30;svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
    save->thread=threadCreate(save_capture,save,16*1024,priority<0x3f?priority+1:priority,0,false);
    if(save->thread){save->completion=threadGetHandle(save->thread);return false;}
    update_status("RAW worker allocation",FILM_NOMEM,0);
    free(save->gray_pair.preview.pixels);*save=(CaptureSave){0};return false;
}

static void draw(C3D_RenderTarget *top,C3D_RenderTarget *bottom);
static void serve_selected_evidence(C3D_RenderTarget *top,C3D_RenderTarget *bottom)
{
    if(!film_runtime_source_valid("film-app-frozen-evidence/v1"))return;
    finish_capture(true);
    if(!app.count){update_status("No saved photo",FILM_STATE,0);return;}
    film_camera_close(&app.camera);app.live=false;reset_preview_tracking();
    if(app.log)fflush(app.log);
    unsigned id=app.photos[app.ui.selection];char original[64],jpeg[80],index[80],run[80];
    roll_path(original,id);snprintf(jpeg,sizeof jpeg,PREVIEWS "ROLL%04u.JPG",id);
    snprintf(index,sizeof index,PREVIEWS "ROLL%04u.F3I",id);
    snprintf(run,sizeof run,"film-app-%04u-%llu",id,(unsigned long long)osGetTime());
    const char *paths[4]={original,jpeg,index,LOGS "film3ds.log"};
    app.probe_title="Existing online recovery";memset(app.probe_rows,0,sizeof app.probe_rows);
    snprintf(app.probe_rows[0],sizeof app.probe_rows[0],"Photo #%04u; FILMRAM HTTP port 5000",id);
    snprintf(app.probe_rows[1],sizeof app.probe_rows[1],"Keep open; START returns to app");draw(top,bottom);
    bool served=film_app_evidence_serve(run,paths);
    app.probe_title=NULL;update_status(served?"Recovery ended; L: reopen camera":"Recovery snapshot failed",served?FILM_OK:FILM_IO,0);
}

static void preview_darkroom(void)
{
    FilmFrame frame = {0};
    FilmLut lut = {0};
    if (!app.darkroom_ready) { update_status("Develop recipe", FILM_STATE, 0); return; }
    unsigned look_index = find_look(app.darkroom.lut_id);
    const Look *look = look_index ? &app.luts[look_index - 1] : NULL;
    FilmCameraColor camera_color;
    FilmGrainProfile grain_profile;
    FilmRawIsp raw_isp;
    FilmDevelopOptions options = {.adaptive_grain = true, .grain = app.grain, .halation = app.halation,
        .raw_shadows=app.raw_shadows,.raw_midtones=app.raw_midtones,.raw_highlights=app.raw_highlights,
        .raw_shoulder=app.raw_shoulder,.raw_local=app.raw_local,.raw_local_limit=1.5f,
        .layered_grain=true,.chroma_denoise=app.chroma_denoise,.grain_size=app.grain_size,.grain_shadow=app.grain_shadow,
        .grain_mid=app.grain_mid,.grain_highlight=app.grain_highlight,
        .output_renderer=app.grain_gpu?film_3ds_canvas_output:NULL,
        .grain_renderer=app.grain_gpu?film_3ds_grain_layered:film_grain_layered_texture,
        .grain_texture=app.grain==FILM_GRAIN_MEDIUM&&app.grain_texture.pixels?&app.grain_texture:NULL};
    FilmStatus status;
    bool cached_scene=app.decoded_id==app.darkroom_id&&app.decoded_scene.pixels;
    if(cached_scene){
        frame.recipe=app.darkroom;status=FILM_OK;
        if(app.decoded_calibration!=app.raw_calibration){
            /* A changed sensor calibration is a different decode. Cache it
             * once; later A renders reuse the same linear pixels. */
            status=open_selected_original(&frame);
            if(status==FILM_OK&&app.raw_calibration){
                const char *path=app.raw_calibration==1?"romfs:/calibration/display-color.f3isp":"romfs:/calibration/display-gray.f3isp";
                FILE *file=fopen(path,"rb");status=file?film_raw_profile_load("film-raw-profile-load/v1",file,&raw_isp):FILM_IO;
                if(file)fclose(file);if(status==FILM_OK)options.raw_isp=&raw_isp;
            }
            if(status==FILM_OK){
                film_raw_scene_free(&app.decoded_scene);
                status=film_raw_frame_scene(&frame,&options,&app.decoded_scene);
                if(status==FILM_OK)app.decoded_calibration=app.raw_calibration;
                else{app.decoded_id=0;app.darkroom_ready=false;}
            }
            options.raw_isp=NULL; /* Sensor calibration is baked into the scene. */
        }
    }else if(app.roll_preview_format==FILM_RAW10_RGGB)status=FILM_STATE;
    else if(app.count&&app.roll_preview_format)status=open_selected_original(&frame);
    else status=FILM_STATE;
    if (status == FILM_OK) {
        frame.recipe = app.darkroom;
        frame.recipe.grain_seed ^= UINT32_C(0x9e3779b9) * app.darkroom_id;
        if (look && frame.recipe.profile.strength > 0) status = load_look_lut(look, &lut);
    }
    if (status == FILM_OK)
        status = load_camera_color(frame.camera, &camera_color, &options.camera_color);
    if (status == FILM_OK && app.grain_reference[app.grain]) {
        FILE *file = fopen(grain_paths[app.grain], "r");
        status = file ? film_grain_profile_load(file, &grain_profile) : FILM_IO;
        if (file) fclose(file);
        if (status == FILM_OK) options.grain_profile = &grain_profile;
    }
    if (status == FILM_OK) {
        status = cached_scene ? film_darkroom_scene_preview(&app.room,&app.decoded_scene,&frame.recipe,lut.samples?&lut:NULL,&options) :
            film_darkroom_preview(&app.room, &frame, lut.samples ? &lut : NULL, &options);
        if (status==FILM_OK) {
        show_image(&app.room.composition_view, FILM_601_FULL);
        snprintf(app.status, sizeof app.status, "Preview %ux%u - not saved",
                 app.room.preview.width, app.room.preview.height);
        if (app.log) {
            fprintf(app.log, "Darkroom preview LUT=%s strength=%.0f%% input_EV=%.2f H/M/S=%+.0f/%+.0f/%+.0f sharpness=%+d\n",
                    frame.recipe.lut_id[0] ? frame.recipe.lut_id : "Neutral",
                    frame.recipe.profile.strength * 100,
                    frame.recipe.input_tone.value[FILM_TONE_EV],
                    frame.recipe.output_tone.value[FILM_TONE_HIGH] * 5,
                    frame.recipe.output_tone.value[FILM_TONE_MID] * 5,
                    frame.recipe.output_tone.value[FILM_TONE_SHADOWS] * 5,
                    frame.recipe.sharpness);
            fflush(app.log);
        }
        } else {
            snprintf(app.status,sizeof app.status,"Preview %s %s (%s)",app.room.failure_phase?app.room.failure_phase:"load",
                film_status_name(status),app.grain_gpu?"GPU":"CPU");
            film_log(app.log,app.status,status,0);
        }
    } else update_status("Darkroom preview", status, 0);
    film_lut_free(&lut);
    film_frame_free(&frame);
}

static void white_balance_input(u32 keys, u32 repeated)
{
    int red=app.room.pending_wb.red,blue=app.room.pending_wb.blue;
    if (repeated&KEY_LEFT) --red;
    if (repeated&KEY_RIGHT) ++red;
    if (repeated&KEY_UP) ++blue;
    if (repeated&KEY_DOWN) --blue;
    if (keys&KEY_Y) red=blue=0;
    if (keys&KEY_TOUCH) {
        touchPosition touch;hidTouchRead(&touch);
        if (touch.px>=84 && touch.px<236 && touch.py>=44 && touch.py<196) {
            red=(touch.px-84)/8-9; blue=9-(touch.py-44)/8;
        } else if (touch.py>=208) keys|=touch.px<160?KEY_A:KEY_X;
    }
    if (red<-9) red=-9;
    if (red>9) red=9;
    if (blue<-9) blue=-9;
    if (blue>9) blue=9;
    app.room.pending_wb.red=red;app.room.pending_wb.blue=blue;
    if (keys&KEY_X) {
        app.room.wb_editor=false;
        if (app.room.composition_view.pixels) show_image(&app.room.composition_view,FILM_601_FULL);
    } else if (keys&KEY_A) {
        app.darkroom.white_balance=app.room.pending_wb;
        app.room.wb_editor=false;
        preview_darkroom();
    }
}

static void save_darkroom_preview(void)
{
    if (!app.room.preview.pixels) return;
    char filename[24] = {0}, path[96] = {0};
    struct stat existing;
    FilmStatus status = FILM_STATE;
    for (unsigned revision = 1; revision <= MAX_PHOTOS; ++revision) {
        unsigned id = app.count ? app.photos[app.ui.selection] : app.ui.selection + 1;
        snprintf(filename, sizeof filename, "%c%04u-%04u.JPG", app.count ? 'R' : 'V', id, revision);
        snprintf(path, sizeof path, EXPORTS "%s", filename);
        if (stat(path, &existing) == 0) continue;
        status = film_darkroom_finish(&app.room, true, path);
        break;
    }
    film_darkroom_discard(&app.room);
    if (status == FILM_OK) {
        snprintf(app.status, sizeof app.status, "JPEG saved %s", filename);
        film_log(app.log, app.status, status, 0);
    } else update_status("Save preview", status, 0);
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
            uint8_t *tile = tiles + swizzle(x, INPUT - 1 - y, INPUT) * 4;
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

static void darkroom_value(FilmDarkroomRow row, char *value, size_t size)
{
    const FilmRecipe *r = &app.darkroom;
    unsigned w = 0, h = 0;
    FilmRect crop = {0};
    switch (row) {
    case DARKROOM_ASPECT: snprintf(value, size, "%s", aspect_names[r->aspect]); break;
    case DARKROOM_COMPOSITION: snprintf(value,size,"%u%%",r->crop_percent?r->crop_percent:100); break;
    case DARKROOM_WB:
        if (r->white_balance.kelvin) snprintf(value,size,"%u K",r->white_balance.kelvin);
        else snprintf(value,size,"Original");
        break;
    case DARKROOM_WB_SHIFT: snprintf(value,size,"R%+d B%+d >",r->white_balance.red,r->white_balance.blue); break;
    case DARKROOM_SIZE:
        film_scaled_dims(app.source_width, app.source_height, r->scale, &w, &h);
        film_crop_rect(w, h, r->aspect, &crop);
        snprintf(value, size, "%ux%u %s", crop.width, crop.height, scale_names[r->scale]); break;
    case DARKROOM_LOOK: {
        unsigned look=find_look(r->lut_id);
        const char *name=look?app.luts[look-1].name:r->lut_id[0]?r->lut_id:"Neutral";
        size_t length = strlen(name);
        if (length > 5 && !strcasecmp(name + length - 5, ".cube")) length -= 5;
        snprintf(value, size, "%.*s", (int)length, name); break;
    }
    case DARKROOM_EXPOSURE: snprintf(value, size, "%+.2f EV", r->input_tone.value[FILM_TONE_EV]); break;
    case DARKROOM_HIGHLIGHTS: snprintf(value, size, "%+.0f", r->output_tone.value[FILM_TONE_HIGH] * 5); break;
    case DARKROOM_MIDTONES: snprintf(value, size, "%+.0f", r->output_tone.value[FILM_TONE_MID] * 5); break;
    case DARKROOM_SHADOWS: snprintf(value, size, "%+.0f", r->output_tone.value[FILM_TONE_SHADOWS] * 5); break;
    case DARKROOM_GRAIN_AMOUNT: snprintf(value, size, "%.0f%%", r->grain_amount * 100); break;
    case DARKROOM_GRAIN_BACKEND:snprintf(value,size,"%s",app.grain_gpu?"GPU mix":"CPU");break;
    case DARKROOM_RAW_CALIBRATION:snprintf(value,size,"%s",app.raw_calibration==1?"Display color":app.raw_calibration==2?"Display gray":"Off");break;
    case DARKROOM_RAW_CURVE:snprintf(value,size,"%s",
        app.roll_preview_format!=FILM_RAW10_RGGB?"RAW only":
        app.shot_curve_mode==2?"Custom":app.shot_curve_mode?"Shot (approx)":"Neutral");break;
    case DARKROOM_RAW_SHADOWS:snprintf(value,size,"%+.1f EV",app.raw_shadows);break;
    case DARKROOM_RAW_MIDTONES:snprintf(value,size,"%+.1f EV",app.raw_midtones);break;
    case DARKROOM_RAW_HIGHLIGHTS:snprintf(value,size,"%+.1f EV",app.raw_highlights);break;
    case DARKROOM_RAW_SHOULDER:snprintf(value,size,"%s%.2f",app.raw_shoulder?"Knee ":"Off ",app.raw_shoulder);break;
    case DARKROOM_RAW_LOCAL:snprintf(value,size,"%.0f%%",app.raw_local*100);break;
    case DARKROOM_DENOISE:snprintf(value,size,"%.0f%%",app.chroma_denoise*100);break;
    case DARKROOM_GRAIN_SIZE:snprintf(value,size,"%.2fx",app.grain_size);break;
    case DARKROOM_GRAIN_SHADOW:snprintf(value,size,"%.1f",app.grain_shadow);break;
    case DARKROOM_GRAIN_MID:snprintf(value,size,"%.1f",app.grain_mid);break;
    case DARKROOM_GRAIN_HIGH:snprintf(value,size,"%.1f",app.grain_highlight);break;
    case DARKROOM_GRAIN_KIND: snprintf(value, size, "%s", app.grain_reference[app.grain] ?
        grain_stock_names[app.grain] : grain_names[app.grain]); break;
    case DARKROOM_SATURATION: snprintf(value, size, "%+.0f%%", r->tone.value[FILM_TONE_SATURATION] * 100); break;
    case DARKROOM_SHARPNESS: snprintf(value, size, "%+d", r->sharpness); break;
    case DARKROOM_STRENGTH: snprintf(value, size, "%.0f%%", r->profile.strength * 100); break;
    case DARKROOM_RESAMPLING: snprintf(value, size, "%s", r->filter == FILM_BILINEAR ? "Bilinear" : "Bicubic"); break;
    case DARKROOM_HALATION: snprintf(value, size, "%s", app.halation ? "ON" : "OFF"); break;
    default: value[0] = 0; break;
    }
}

static void draw_darkroom(u32 bg, u32 fg, u32 gold, u32 dim)
{
    char line[80], value[64];
    for (unsigned visible = 0; visible < FILM_DARKROOM_VISIBLE_ROWS; ++visible) {
        unsigned index = app.room.first_row + visible;
        if (index >= DARKROOM_ROW_COUNT) break;
        FilmDarkroomRow row = (FilmDarkroomRow)index;
        float y = FILM_DARKROOM_ROW_Y + visible * FILM_DARKROOM_ROW_HEIGHT;
        bool selected = row == app.room.row;
        C2D_DrawRectSolid(10, y, 0.1f, 300, FILM_DARKROOM_ROW_HEIGHT - 1, selected ? gold : dim);
        text_at(16, y - 1, 0.62f, selected ? bg : fg, film_darkroom_label(row));
        darkroom_value(row, value, sizeof value);
        snprintf(line, sizeof line, "[%.18s]", value);
        text_at(133, y - 1, 0.62f, selected ? bg : fg, line);
    }
    snprintf(line, sizeof line, "^v Row <> Edit B+Pad Pan A View %u/%u",
             (unsigned)app.room.row + 1, DARKROOM_ROW_COUNT);
    text_at(12, 184, 0.48f, fg, line);
}

static void draw_white_balance(u32 bg,u32 fg,u32 gold)
{
    char line[80];
    text_at(12,8,.7f,gold,"WHITE BALANCE SHIFT");
    snprintf(line,sizeof line,"R:%+d  B:%+d",app.room.pending_wb.red,app.room.pending_wb.blue);
    text_at(100,25,.53f,fg,line);
    for (int y=0;y<19;++y) for (int x=0;x<19;++x) {
        FilmRgb c=film_white_balance_swatch(x-9,9-y);
        u32 color=C2D_Color32((u8)(c.r*255+.5f),(u8)(c.g*255+.5f),(u8)(c.b*255+.5f),255);
        C2D_DrawRectSolid(84+x*8,44+y*8,.1f,8,8,color);
    }
    int x=84+(app.room.pending_wb.red+9)*8,y=44+(9-app.room.pending_wb.blue)*8;
    C2D_DrawRectSolid(x-1,y-1,.2f,10,1,fg);C2D_DrawRectSolid(x-1,y+8,.2f,10,1,fg);
    C2D_DrawRectSolid(x-1,y-1,.2f,1,10,fg);C2D_DrawRectSolid(x+8,y-1,.2f,1,10,fg);
    C2D_DrawRectSolid(x+2,y+2,.3f,4,4,bg);
    text_at(20,110,.53f,fg,"R-");text_at(254,110,.53f,fg,"R+");
    text_at(241,48,.53f,fg,"B+");text_at(241,179,.53f,fg,"B-");
    text_at(12,197,.42f,fg,"Arrows: move   Y: reset");
    text_at(12,213,.53f,gold,"A: preview               X: cancel");
}

static void draw_composition(float x,float y,float w,float h,u32 gold)
{
    if (app.ui.page!=FILM_DARKROOM || !app.source_width || !app.source_height || app.probe_title) return;
    FilmRect rect;
    if (film_composition_rect(app.source_width,app.source_height,&app.darkroom,&rect)!=FILM_OK) return;
    float sx=w/app.source_width,sy=h/app.source_height;
    float left=x+rect.x*sx,top=y+rect.y*sy,right=left+rect.width*sx,bottom=top+rect.height*sy;
    u32 shade=C2D_Color32(0,0,0,80);
    C2D_DrawRectSolid(x,y,.1f,w,top-y,shade);
    C2D_DrawRectSolid(x,bottom,.1f,w,y+h-bottom,shade);
    C2D_DrawRectSolid(x,top,.1f,left-x,bottom-top,shade);
    C2D_DrawRectSolid(right,top,.1f,x+w-right,bottom-top,shade);
    C2D_DrawRectSolid(left,top,.2f,right-left,1,gold);C2D_DrawRectSolid(left,bottom-1,.2f,right-left,1,gold);
    C2D_DrawRectSolid(left,top,.2f,1,bottom-top,gold);C2D_DrawRectSolid(right-1,top,.2f,1,bottom-top,gold);
}

/* ROLL_DRAW_BEGIN */
static void draw_roll(u32 bg,u32 fg,u32 gold,u32 dim)
{
    char line[80];
    if (app.roll.deleting) {
        C2D_DrawRectSolid(12,50,.4f,296,150,bg);
        text_at(24,64,.65f,gold,"DELETE ORIGINAL?");
        snprintf(line,sizeof line,"Photo #%04u original",app.roll.pending_id);
        text_at(24,95,.62f,fg,line);
        text_at(24,124,.5f,fg,"Exported JPEGs are kept");
        text_at(24,165,.58f,gold,"A: delete       X: cancel");
        return;
    }
    snprintf(line,sizeof line,"Photo %u / %u   %s",app.count?app.ui.selection+1:0,app.count,
        app.roll_preview_format==FILM_RAW10_RGGB?"RAW 10bit":app.roll_preview_format?"RGB / YUYV":app.count?"Not decoded":"");
    text_at(20,62,.62f,fg,line);
    film_roll_sync(&app.roll,&app.ui);
    unsigned rows=app.count?(app.count+FILM_ROLL_COLUMNS-1)/FILM_ROLL_COLUMNS:1;
    unsigned first_row=app.roll.first/FILM_ROLL_COLUMNS;
    if (rows>first_row+FILM_ROLL_ROWS) rows=first_row+FILM_ROLL_ROWS;
    for (unsigned row=first_row;row<rows;++row) for (unsigned col=0;col<FILM_ROLL_COLUMNS;++col) {
        unsigned index=row*FILM_ROLL_COLUMNS+col;
        float x=FILM_ROLL_GRID_X+col*FILM_ROLL_CELL,y=FILM_ROLL_GRID_Y+(row-first_row)*FILM_ROLL_CELL;
        bool valid=index<app.count,selected=valid && index==app.ui.selection;
        if (selected) C2D_DrawRectSolid(x-3,y-3,.1f,FILM_ROLL_SQUARE+6,FILM_ROLL_SQUARE+6,C2D_Color32(88,194,99,255));
        C2D_DrawRectSolid(x,y,.2f,FILM_ROLL_SQUARE,FILM_ROLL_SQUARE,valid?gold:dim);
        unsigned slot=index-app.roll.first;RollThumbnail *thumb=&app.thumbnails[slot];
        if(valid&&thumb->ready&&thumb->id==app.photos[index]){
            const Tex3DS_SubTexture sub={24,18,0,1,1,1.0f-ROLL_TEX_ROWS/(float)ROLL_TEX_SIDE};
            C2D_DrawImageAt((C2D_Image){&thumb->texture,&sub},x,y+3,.3f,NULL,1,1);
        }else if(valid)text_at(x+7,y+5,.45f,bg,"?");
        if(valid&&app.roll_decode.decode_selection[app.photos[index]])
            C2D_DrawRectSolid(x+17,y,.4f,7,7,C2D_Color32(88,194,99,255));

    }
    snprintf(line,sizeof line,"B: mark  SELECT: decode  Y: delete");
    text_at(12,184,.48f,fg,app.count?line:"No original photos");

}
/* ROLL_DRAW_END */

static void draw(C3D_RenderTarget *top, C3D_RenderTarget *bottom)
{
    static const char *const pages[] = {"VIEW RAW15", "ROLL", "DARKROOM"};
    u32 bg = C2D_Color32(18, 22, 23, 255), fg = C2D_Color32(241, 235, 216, 255);
    u32 gold = C2D_Color32(248, 193, 107, 255), dim = C2D_Color32(40, 49, 48, 255);
    unsigned zoom = app.save.thread&&atomic_load(&app.save.phase)==CAPTURE_ACQUIRING?app.save.capture_zoom:app.live?app.camera.zoom:1;
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
    /* ROLL_EMPTY_IMAGE_BEGIN */
    if (app.ui.page!=FILM_ROLL || (app.count && app.image_width)) {
    C2D_DrawImageAt((C2D_Image){&app.texture, &sub}, x, y, 0, NULL, 1, 1);
    } else text_at(100,110,.65f,fg,"No photo preview");
    /* ROLL_EMPTY_IMAGE_END */
    draw_composition(x,y,sub.width,sub.height,gold);
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

    if(app.debug.debug_mode)text_at(350,222,.45f,gold,"debug");
    C2D_TargetClear(bottom, bg);
    C2D_SceneBegin(bottom);
    if (app.ui.page==FILM_DARKROOM && app.room.wb_editor) {
        draw_white_balance(bg,fg,gold);C3D_FrameEnd(0);return;
    }
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
                                            "DMA");
        else snprintf(line, sizeof line, "%s %s %u FPS", sensors[app.camera.sensor],
                      "DMA", app.preview_fps);
        text_at(148, 12, 0.59f, gold, line);
        if (app.live) {
            if(app.camera_values[FILM_CAM_EXPOSURE_MODE]==FILM_EXPOSURE_NATIVE_AE)
                snprintf(line,sizeof line,"Native AE  comp %+d",app.camera_values[FILM_CAM_EV_COMP]);
            else snprintf(line,sizeof line,"Manual %+d %s",app.camera_values[FILM_CAM_EXPOSURE],
                app.camera.exposure.ready?"READY":app.camera.exposure.failed?"ERROR":"SETTLING");
            text_at(195, 45, 0.44f, gold, line);
        }
    }
    text_at(12, 34, 0.53f, fg, app.status);
    if (app.ui.page == FILM_VIEW)
        for (unsigned i = 0; i < 3; ++i) C2D_DrawRectSolid(10, 57 + i * 48, 0.1f, 300, 43, dim);
    if (app.ui.page == FILM_VIEW && app.camera_controls) {
        static const char *const wb[] = {"Auto", "3200K", "4150K", "5200K", "6000K", "7000K"};
        static const char *const contrast[] = {"Low", "Normal", "High"};
        static const char *const exposure_modes[] = {"Native AE", "Manual"};
        int value = app.camera_values[app.control];
        text_at(20, 68, 0.62f, fg, control_names[app.control]);
        if (app.control == FILM_CAM_WB) {
            const char *awb = !app.live || R_FAILED((Result)app.camera_auto.white_balance_result)
                ? "?" : app.camera_auto.white_balance ? "ON" : "OFF";
            snprintf(line, sizeof line, "<   %s   >    AWB %s", wb[value], awb);
        }
        else if (app.control == FILM_CAM_CONTRAST) snprintf(line, sizeof line, "<   %s   >", contrast[value + 1]);
        else if (app.control == FILM_CAM_EXPOSURE_MODE)
            snprintf(line, sizeof line, "<   %s   >", exposure_modes[value]);
        else if (app.control == FILM_CAM_EV_COMP)
            snprintf(line,sizeof line,"Native %+d%s",value,
                     app.camera_values[FILM_CAM_EXPOSURE_MODE]==FILM_EXPOSURE_MANUAL?" (AE only)":"");
        else if (app.control == FILM_CAM_EXPOSURE && app.camera_values[FILM_CAM_EXPOSURE_MODE] != FILM_EXPOSURE_MANUAL)
            snprintf(line,sizeof line,"%+d (Manual only)",value);
        else if (app.control == FILM_CAM_SATURATION) snprintf(line, sizeof line, "<   %d%%   >", 100 + value * 10);
        else snprintf(line, sizeof line, "<   %+d   >", value);
        text_at(20, 116, 0.62f, fg, line);
        text_at(20, 163, 0.53f, gold, "Up/Down: setting   Left/Right: value");
        text_at(20, 183, 0.48f, fg, "Y: back   R: capture");
    } else if (app.ui.page == FILM_VIEW) {
        text_at(20, 68, 0.62f, fg, "Capture RAW DNG");
        text_at(20, 116, 0.62f, fg, "Output settings: Darkroom");
        snprintf(line, sizeof line, "A/R: Capture   Zoom %ux (Circle Pad)", zoom);
        text_at(20, 163, 0.54f, gold, line);
        text_at(20, 183, 0.48f, fg, "L: lens   Y: camera controls");
    } else if (app.ui.page == FILM_ROLL) {
        draw_roll(bg,fg,gold,dim);
    } else draw_darkroom(bg, fg, gold, dim);
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
    const char *scene_qa_token=NULL;bool scene_qa_gray=false;
    const char *qa_token = NULL;
    const char *raw_qa_token = NULL;
    const char *raw_recover_token = NULL;
    bool raw_recover_only=false;
    bool qa_frames = false;
    for (int i = 1; i < argc; ++i) {
        if(!strcmp(argv[i],"f3scene-gray"))scene_qa_gray=true;
        if(!strncmp(argv[i],"f3sceneqa=",10)&&film_raw_qa_token_valid(argv[i]+10))scene_qa_token=argv[i]+10;
        if (!strncmp(argv[i], "f3qa=", 5) && strlen(argv[i] + 5) == 32 &&
            strspn(argv[i] + 5, "0123456789abcdef") == 32) qa_token = argv[i] + 5;
        if (!strcmp(argv[i], "f3qa-frames")) qa_frames = true;
        if (!strncmp(argv[i],"f3rawqa=",8)&&film_raw_qa_token_valid(argv[i]+8))raw_qa_token=argv[i]+8;
        if (!strncmp(argv[i],"f3rawrecover=",13)&&film_raw_qa_token_valid(argv[i]+13))raw_recover_token=argv[i]+13;
        if (!strcmp(argv[i],"f3rawrecover-only"))raw_recover_only=true;
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
    for (unsigned i = 0; i < ROLL_VISIBLE_TEXTURES; ++i) {
        RollThumbnail *thumbnail = &app.thumbnails[i];
        thumbnail->allocated = C3D_TexInit(&thumbnail->texture, ROLL_TEX_SIDE, ROLL_TEX_SIDE, GPU_RGB8);
        if (thumbnail->allocated) C3D_TexSetFilter(&thumbnail->texture, GPU_LINEAR, GPU_LINEAR);
    }
    memset(app.texture.data, 0, PREVIEW_TEX_SIDE * PREVIEW_TEX_SIDE * 3);
    mkdir("sdmc:/3ds", 0777);
    mkdir(ROOT, 0777);
    mkdir(LOGS,0777);
    mkdir(ROLLS, 0777);
    mkdir(EXPORTS, 0777);
    mkdir(PREVIEWS,0777);
    mkdir(LUTS, 0777);
    mkdir(RAWS,0777);
    if(raw_qa_token) {
        app.probe_title="RAW capture automation";
        snprintf(app.probe_rows[0],sizeof app.probe_rows[0],"Production capture; returning evidence...");
        draw(top,bottom);
        bool reported=film_raw_capture_qa(raw_qa_token,raw_recover_token,raw_recover_only);
        snprintf(app.probe_rows[0],sizeof app.probe_rows[0],"%s",reported?"Report delivered; inspect host result":"Report failed; SD trace retained");
        snprintf(app.probe_rows[1],sizeof app.probe_rows[1],"START: exit to Homebrew Launcher");
        while(aptMainLoop()){hidScanInput();if(hidKeysDown()&KEY_START)break;draw(top,bottom);}
        goto cleanup;
    }
    app.log=fopen(LOGS "film3ds.log","a");
    app.camera.runtime_log=app.log;
    if(app.log){setvbuf(app.log,NULL,_IOLBF,0);fprintf(app.log,"APP startup ms=%llu\n",(unsigned long long)osGetTime());fflush(app.log);}
    bool romfs_mounted = R_SUCCEEDED(romfsInit());
    if(scene_qa_token){
        if(!romfs_mounted){update_status("Scene probe ROMFS failed",FILM_IO,0);goto cleanup;}
        char dir[256];snprintf(dir,sizeof dir,ROOT "scene-%s",scene_qa_token);
        FilmRecipe recipe=film_default_recipe();recipe.grain_amount=0;
        FilmDevelopOptions options={.raw_local=.35f,.raw_local_limit=1.5f};
        FilmStatus result=film_raw_scene_probe("film-raw-scene-probe/v1",scene_qa_gray?"romfs:/rawqa/gray.F3S":"romfs:/rawqa/color.F3S",scene_qa_gray?"romfs:/calibration/display-gray.f3isp":"romfs:/calibration/display-color.f3isp",dir,&recipe,&options);
        app.probe_title="RAW scene consistency probe";
        snprintf(app.probe_rows[0],sizeof app.probe_rows[0],"%s; camera untouched",film_status_name(result));
        snprintf(app.probe_rows[1],sizeof app.probe_rows[1],"FILMRAM HTTP 5000; START returns");draw(top,bottom);
        if(result==FILM_OK){
            char files[4][300];const char *paths[4];const char *names[]={"canvas.rgb16","samples.f32","preview.jpg","report.json"};
            for(unsigned i=0;i<4;++i){snprintf(files[i],sizeof files[i],"%s/%s",dir,names[i]);paths[i]=files[i];}
            film_app_evidence_serve(scene_qa_token,paths);
        }
        goto cleanup;
    }
    if (romfs_mounted) {
        unsigned installed=0;FilmStatus installed_status=film_lut_install_missing("film-lut-install/v1",PACKED_LUTS,LUTS,&installed);
        film_log(app.log,"film-lut-install/v1",installed_status,installed);
        for (unsigned i = 0; i < FILM_GRAIN_COUNT; ++i) {
            struct stat profile;
            app.grain_reference[i] = stat(grain_paths[i], &profile) == 0 && S_ISREG(profile.st_mode);
        }
    }
    scan_looks(LUTS);
    load_roll();
    app.grain = FILM_GRAIN_MEDIUM;
    app.ae_region=film_ae_region_default();
    app.control=FILM_CAM_EXPOSURE_MODE;
    app.raw_shoulder=.75f;
    app.grain_size=1;app.grain_shadow=.8f;app.grain_mid=1.1f;app.grain_highlight=.7f;
    FILE *atlas=fopen("romfs:/grain/pan400.f3gt","rb");
    if(atlas){FilmStatus gs=film_grain_texture_load(atlas,&app.grain_texture);fclose(atlas);film_log(app.log,"Grain texture",gs,0);}
    if (!qa_token) run_commands();
    bool calibration_logged=false;
    show_camera_closed();
    update_status("L: camera R: RAW SELECT+R: gray pair X: pages", FILM_OK, 0);
    if (qa_token) run_hardware_qa(qa_token, qa_frames, top, bottom);

    while (aptMainLoop()) {
        FilmPage previous_page = app.ui.page;
        unsigned previous_photo = app.ui.selection;
        FilmAspect previous_aspect = app.ui.recipe.aspect;
        bool saved = finish_capture(false);
        hidScanInput();
        u32 keys = hidKeysDown();
        if(keys){
            unsigned key=keys==KEY_A?FILM_DEBUG_A:keys==KEY_B?FILM_DEBUG_B:
                (keys&KEY_UP)&&!(keys&~KEY_UP)?FILM_DEBUG_UP:(keys&KEY_DOWN)&&!(keys&~KEY_DOWN)?FILM_DEBUG_DOWN:FILM_DEBUG_OTHER;
            bool was_debug=app.debug.debug_mode;
            if(film_debug_mode_key(&app.debug,key)){
                atomic_store(&app.camera.debug_mode,app.debug.debug_mode);
                if(app.log){fprintf(app.log,"DEBUG %s ms=%llu\n",app.debug.debug_mode?"ON":"OFF",(unsigned long long)osGetTime());fflush(app.log);}
            }
            if((was_debug||app.debug.debug_mode)&&app.log){fprintf(app.log,"KEY ms=%llu down=%08lx held=%08lx page=%u\n",
                (unsigned long long)osGetTime(),(unsigned long)keys,(unsigned long)hidKeysHeld(),app.ui.page);fflush(app.log);}
        }
        if(app.save.thread){
            unsigned phase=atomic_load(&app.save.phase);
            if(phase==CAPTURE_ACQUIRING){
                static const char *const names[]={"Manual/AE verify","RAW preconditions","Lock AE/AWB","Native gate","RAW switch","RAW settle","RAW retain","Camera restore"};
                unsigned step=atomic_load(&app.camera.capture_phase);
                snprintf(app.status,sizeof app.status,"%s #%04u",step<8?names[step]:"RAW capture",app.save.id);
                draw(top,bottom);continue;
            }
            app.live=app.camera.initialized;app.preview_failed=!app.live;
            if(phase==CAPTURE_SAVING)snprintf(app.status,sizeof app.status,"Saving DNG #%04u",app.save.id);
        }

        if(app.roll_decode.decode_active){
            if(keys&KEY_START)break;
            if(keys&KEY_B){film_roll_decode_cancel(&app.roll_decode);update_status("Decode stopped",FILM_OK,0);}
            else{
                snprintf(app.status,sizeof app.status,"Decode %u/%u #%04u; B stops",
                    app.roll_decode.decode_position+1,app.roll_decode.decode_count,
                    app.roll_decode.decode_ids[app.roll_decode.decode_position]);
                draw(top,bottom);decode_roll_step();
            }
            roll_prepare_grid();draw(top,bottom);continue;
        }
        if(keys==KEY_B&&app.ui.page==FILM_ROLL&&!app.roll.deleting&&app.count){
            film_roll_decode_toggle(&app.roll_decode,app.photos[app.ui.selection]);
            draw(top,bottom);continue;
        }
        /* ROLL_MODAL_INPUT_BEGIN */
        if (app.ui.page==FILM_ROLL && app.roll.deleting) {
            roll_delete_input(keys);draw(top,bottom);continue;
        }
        /* ROLL_MODAL_INPUT_END */
        /* DARKROOM_PREVIEW_INPUT_BEGIN */
        u32 held=hidKeysHeld(),repeated=hidKeysDownRepeat();
        if (app.ui.page==FILM_DARKROOM && app.room.wb_editor) {
            white_balance_input(keys,repeated);draw(top,bottom);continue;
        }
        if (app.ui.page==FILM_DARKROOM && app.room.preview.pixels && keys==KEY_SELECT) {
            save_darkroom_preview();draw(top,bottom);continue;
        }
        if (app.room.preview.pixels && (keys&(KEY_X|KEY_L|KEY_R|KEY_START|KEY_ZL|KEY_ZR)))
            film_darkroom_discard(&app.room);
        /* DARKROOM_PREVIEW_INPUT_END */
        if(keys==KEY_SELECT && app.ui.page==FILM_ROLL) {
            if(app.count)prepare_selected_preview();else import_raw_on_device();
            draw(top,bottom);continue;
        }
        if(app.ui.page==FILM_ROLL&&(keys&KEY_Y)&&(held&KEY_SELECT)) {
            import_raw_on_device();draw(top,bottom);continue;
        }
        if((keys&KEY_START)&&(held&KEY_SELECT)&&(app.ui.page==FILM_VIEW||app.ui.page==FILM_ROLL)){
            serve_selected_evidence(top,bottom);continue;
        }
        if (keys & KEY_START) break;
        if (keys & KEY_ZL) {
            if (app.ui.page == FILM_DARKROOM && (hidKeysHeld() & KEY_SELECT) && !app.save.thread) {
                app.probe_title = "Develop quality";
                memset(app.probe_rows, 0, sizeof app.probe_rows);
                snprintf(app.probe_rows[0], sizeof app.probe_rows[0], "Testing both export sizes...");
                draw(top, bottom);
                develop_quality_probe();
            } else if (app.ui.page == FILM_DARKROOM && !app.probe_title && !(hidKeysHeld() & KEY_SELECT)) {
                film_darkroom_select(&app.room, DARKROOM_ASPECT);
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
            FilmSensor next = app.live ? FILM_CAMERA_FRONT + 1 : FILM_CAMERA_REAR1;
            film_camera_close(&app.camera);
            reset_preview_tracking();
            app.preview_failed = false;
            if (next > FILM_CAMERA_FRONT) {
                app.live = false;
                show_camera_closed();
                update_status("Camera closed; L opens RAW",FILM_OK,0);
            } else {
                FilmStatus status = film_camera_open(&app.camera, FILM_YUYV, next);
                app.live = status == FILM_OK;
                app.preview_failed = !app.live;
                update_status("CAM init", status, app.camera.last_result);
                if (status == FILM_OK) {
                    status = film_camera_request_15fps(&app.camera);
                    app.live = status == FILM_OK;
                    app.preview_failed = !app.live;
                    film_log(app.log, "film-camera-native-preview/v1 15fps", status, app.camera.last_result);
                    if (status != FILM_OK) {
                        update_status("Native camera 15fps", status, app.camera.last_result);
                        film_camera_close(&app.camera);
                    }
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
                    app.camera_auto = film_camera_auto_state(&app.camera);
                    if(app.camera.sensor!=FILM_CAMERA_REAR1 &&
                       app.camera_values[FILM_CAM_EXPOSURE_MODE]==FILM_EXPOSURE_MANUAL) {
                        app.camera_values[FILM_CAM_EXPOSURE_MODE]=FILM_EXPOSURE_NATIVE_AE;
                        update_status("Lens supports Native AE only",FILM_OK,0);
                    }
                    FilmStatus exposure = film_camera_select_exposure_mode(&app.camera,
                        (FilmExposureMode)app.camera_values[FILM_CAM_EXPOSURE_MODE],app.camera_values[FILM_CAM_EV_COMP],app.log);
                    if(exposure==FILM_OK&&app.camera.exposure_mode==FILM_EXPOSURE_NATIVE_AE)
                        exposure=film_camera_set_ae_window(&app.camera,app.ae_region);
                    if(exposure!=FILM_OK){update_status("Exposure mode/AE window",exposure,app.camera.last_result);film_camera_close(&app.camera);}
                    else app.camera_auto=film_camera_auto_state(&app.camera);
                    if (!app.camera.initialized) {
                        app.live = false;
                        app.preview_failed = true;
                    }
                }
            }
        }
        if ((keys & KEY_Y) && (hidKeysHeld() & KEY_SELECT)) y2r_check();
        else if ((keys & KEY_Y) && app.ui.page == FILM_VIEW) app.camera_controls = !app.camera_controls;
        if (keys & KEY_X) film_ui_key(&app.ui, UI_NEXT_PAGE);
        if ((keys & KEY_B) && app.ui.page!=FILM_DARKROOM) {
            if (app.ui.page == FILM_VIEW && app.camera_controls) app.camera_controls = false;
            else film_ui_key(&app.ui, UI_BACK);
        }
        sync_darkroom();
        if (app.ui.page == FILM_VIEW && app.camera_controls) {
            if (keys & KEY_UP) app.control = (app.control + 2) % 3;
            if (keys & KEY_DOWN) app.control = (app.control + 1) % 3;
            if (keys & KEY_LEFT) adjust_camera(-1);
            if (keys & KEY_RIGHT) adjust_camera(1);
        } else if (app.ui.page == FILM_DARKROOM) {
            /* DARKROOM_CONTROLS_BEGIN */
            if (held&KEY_B) {
                int dx=((repeated&KEY_DRIGHT)!=0)-((repeated&KEY_DLEFT)!=0);
                int dy=((repeated&KEY_DDOWN)!=0)-((repeated&KEY_DUP)!=0);
                bool circle=dx==0 && dy==0;
                if (circle) {
                    dx=((repeated&KEY_CPAD_RIGHT)!=0)-((repeated&KEY_CPAD_LEFT)!=0);
                    dy=((repeated&KEY_CPAD_DOWN)!=0)-((repeated&KEY_CPAD_UP)!=0);
                }
                if (dx || dy) {
                    film_darkroom_pan(&app.darkroom,app.source_width,app.source_height,dx,dy,circle);
                    film_darkroom_update_geometry(&app.room,&app.darkroom);
                }
            } else {
                if (keys & KEY_UP) film_darkroom_move(&app.room, -1);
                if (keys & KEY_DOWN) film_darkroom_move(&app.room, 1);
                if (keys & KEY_LEFT) adjust_darkroom(-1);
                if (keys & KEY_RIGHT) adjust_darkroom(1);
                if ((keys&KEY_Y) && !(held&KEY_SELECT) && app.room.row==DARKROOM_WB) {
                    app.darkroom.white_balance.kelvin=0;film_darkroom_discard(&app.room);
                }
            }
            /* DARKROOM_CONTROLS_END */
        } else {
            /* ROLL_CONTROLS_BEGIN */
            if (app.ui.page == FILM_ROLL) {
                int dx=((repeated&KEY_RIGHT)!=0)-((repeated&KEY_LEFT)!=0);
                int dy=((repeated&KEY_DOWN)!=0)-((repeated&KEY_UP)!=0);
                film_roll_move(&app.roll,&app.ui,dx,dy);
                if (keys&KEY_Y) {
                    FilmStatus status=app.save.thread?FILM_STATE:
                        film_roll_request_delete(&app.roll,&app.ui,app.photos,app.count);
                    if (status!=FILM_OK) update_status("Delete original",status,0);
                    else keys&=~(KEY_A|KEY_TOUCH|KEY_R);
                }
            }
            /* ROLL_CONTROLS_END */
            bool live_view = app.ui.page == FILM_VIEW && app.live;

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
            /* ROLL_TOUCH_BEGIN */
            if (app.ui.page==FILM_ROLL && touch.py<204) {
                film_roll_touch(&app.roll,&app.ui,touch.px,touch.py);
            } else
            /* ROLL_TOUCH_END */
            if (app.ui.page == FILM_DARKROOM && touch.py >= FILM_DARKROOM_ROW_Y &&
                touch.py < FILM_DARKROOM_ROW_Y + FILM_DARKROOM_VISIBLE_ROWS * FILM_DARKROOM_ROW_HEIGHT) {
                unsigned row = app.room.first_row + (touch.py - FILM_DARKROOM_ROW_Y) / FILM_DARKROOM_ROW_HEIGHT;
                if (row < DARKROOM_ROW_COUNT) {
                    bool selected = app.room.row == (FilmDarkroomRow)row;
                    film_darkroom_select(&app.room, (FilmDarkroomRow)row);
                    if (selected && touch.px >= 133) adjust_darkroom(touch.px < 222 ? -1 : 1);
                }
            } else if (app.ui.page == FILM_DARKROOM && touch.py < 204) {
                if (touch.py >= 181) action = UI_EXPORT;
            } else if (app.ui.page == FILM_VIEW && !app.camera_controls && touch.py < 154) {
                /* Output controls live in darkroom; tapping here does not edit them. */
            } else if (!app.camera_controls || app.ui.page != FILM_VIEW || touch.py >= 154)
                action = film_ui_touch(&app.ui, touch.px, touch.py);
        }
        sync_darkroom();
        bool camera_failed = false;
        bool capture_requested=app.ui.page==FILM_VIEW&&(action==UI_CAPTURE||(keys&KEY_R));
        if (app.live && app.ui.page == FILM_VIEW && !capture_requested) {
            if (app.ui.recipe.aspect != previous_aspect) reset_exposure_tracking();
            FilmStatus status = update_preview();
            if (status != FILM_OK) {
                update_status("CAM preview", status, app.camera.last_result);
                snprintf(app.status, sizeof app.status, "CAM %s %s %08lX",
                         app.camera.preview_stage ? app.camera.preview_stage : "preview",
                         film_status_name(status), (unsigned long)app.camera.last_result);
                film_camera_close(&app.camera);
                app.live = false;
                app.preview_failed = true;
                camera_failed = true;
            }
        } else if (app.live && app.ui.page != FILM_VIEW && previous_page == FILM_VIEW) {
            FilmStatus status = film_camera_preview_stop(&app.camera);
            reset_preview_tracking();
            if (status != FILM_OK) {
                update_status("CAM stop", status, app.camera.last_result);
                app.live = false;
                app.preview_failed = true;
                camera_failed = true;
            }
        }
        if (!camera_failed && capture_requested)
            saved = capture_photo((keys&KEY_R)&&(held&KEY_SELECT)) || saved;
        else if (action == UI_EXPORT && !app.save.thread) {
            snprintf(app.status, sizeof app.status, "%s",
                     app.darkroom.lut_id[0] && app.darkroom.profile.strength > 0 ? "Applying LUT..." : "Developing...");
            draw(top, bottom);
            preview_darkroom();
        }
        /* ROLL_REFRESH_BEGIN */
        if ((saved || app.ui.page!=previous_page || app.ui.selection!=previous_photo) && app.ui.page!=FILM_VIEW) {
            roll_show_selection();
        }
        else if (app.ui.page==FILM_VIEW && previous_page!=FILM_VIEW && !app.live) {
            show_camera_closed();
        }
        /* ROLL_REFRESH_END */
        sync_darkroom();
        roll_prepare_grid();
        draw(top, bottom);
    }
    film_raw_scene_free(&app.decoded_scene);
    film_darkroom_discard(&app.room);
    finish_capture(true);
    film_camera_close(&app.camera);
    free(app.luts);app.luts=NULL;
    film_grain_texture_free(&app.grain_texture);
    if (romfs_mounted) romfsExit();
    if (app.log) fclose(app.log);
cleanup:
    film_3ds_develop_release();
    /* Shutdown must not depend on another VBlank after an APT close.
     * C3D_Fini drains submitted GPU work before its resources are freed. */
    C3D_Fini();
    if (texture) C3D_TexDelete(&app.texture);
    for (unsigned i = 0; i < ROLL_VISIBLE_TEXTURES; ++i)
        if (app.thumbnails[i].allocated) C3D_TexDelete(&app.thumbnails[i].texture);
    if (app.text) C2D_TextBufDelete(app.text);
    C2D_Fini();
    gfxExit();
    return top && bottom && texture && app.text ? 0 : 1;
}
