#include "platform_camera.h"

#include <3ds.h>
#include <limits.h>
#include <string.h>
#include <time.h>

enum { WIDTH = 640, HEIGHT = 480, CAMERA_BYTES = WIDTH * HEIGHT * 2 };
static const s64 capture_timeout = INT64_C(1500000000);
static const u32 selectors[] = {SELECT_OUT1, SELECT_OUT2, SELECT_IN1};
static const Y2RU_StandardCoefficient coefficients[] = {
    COEFFICIENT_ITU_R_BT_601, COEFFICIENT_ITU_R_BT_601_SCALING,
    COEFFICIENT_ITU_R_BT_709, COEFFICIENT_ITU_R_BT_709_SCALING
};

static unsigned frame_bytes(const FilmCamera *cam)
{
    return CAMERA_BYTES / (cam->zoom * cam->zoom);
}

static Result set_window(FilmCamera *cam, unsigned zoom)
{
    s16 width = WIDTH / zoom, height = HEIGHT / zoom;
    u32 transfer = 0;
    Result rc = CAMU_GetMaxBytes(&transfer, width, height);
    if (R_FAILED(rc)) return rc;
    if (!transfer || transfer > INT16_MAX || (unsigned)width * height * 2 % transfer)
        return MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_CAM, RD_INVALID_SIZE);
    rc = CAMU_SetTransferBytes(cam->port, transfer, width, height);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetTrimming(cam->port, zoom != 1);
    if (R_SUCCEEDED(rc) && zoom != 1) {
        s16 x = (WIDTH - width) / 2, y = (HEIGHT - height) / 2;
        rc = CAMU_SetTrimmingParams(cam->port, x, y, x + width, y + height);
    }
    if (R_SUCCEEDED(rc)) {
        cam->zoom = zoom;
        cam->transfer = transfer;
    }
    return rc;
}

static void release_preview(FilmCamera *cam)
{
    if (cam->preview_event) svcCloseHandle(cam->preview_event);
    cam->preview_event = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (cam->preview_dma[i]) linearFree(cam->preview_dma[i]);
        cam->preview_dma[i] = NULL;
    }
    cam->preview_ready = false;
    cam->preview_signaled = false;
}

void film_camera_close(FilmCamera *cam)
{
    if (!cam->initialized) return;
    CAMU_StopCapture(cam->port);
    CAMU_ClearBuffer(cam->port);
    CAMU_Activate(SELECT_NONE);
    camExit();
    cam->initialized = false;
    cam->streaming = false;
    release_preview(cam);
}

FilmStatus film_camera_open(FilmCamera *cam, FilmPixelFormat format, FilmSensor sensor)
{
    if (!cam || (format != FILM_YUYV && format != FILM_RGB565_LE) ||
        sensor < FILM_CAMERA_REAR1 || sensor > FILM_CAMERA_FRONT) return FILM_INVALID;
    if (cam->initialized) return cam->format == format && cam->sensor == sensor ? FILM_OK : FILM_STATE;
    cam->last_result = 0;
    Result rc = camInit();
    if (R_FAILED(rc)) { cam->last_result = rc; return FILM_DEVICE; }
    cam->initialized = true;
    cam->sensor = sensor;
    cam->port = sensor == FILM_CAMERA_REAR2 ? PORT_CAM2 : PORT_CAM1;
    u32 select = selectors[sensor - FILM_CAMERA_REAR1];
    rc = CAMU_Activate(select);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetSize(select, SIZE_VGA, CONTEXT_A);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetOutputFormat(select,
        format == FILM_YUYV ? OUTPUT_YUV_422 : OUTPUT_RGB_565, CONTEXT_A);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetAutoExposure(select, false);
    bool auto_exposure = true;
    if (R_SUCCEEDED(rc)) rc = CAMU_IsAutoExposure(&auto_exposure, select);
    if (R_SUCCEEDED(rc) && auto_exposure) {
        film_camera_close(cam);
        return FILM_UNSUPPORTED;
    }
    if (R_SUCCEEDED(rc)) rc = CAMU_SetExposure(select, 0);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetAutoWhiteBalance(select, true);
    if (R_SUCCEEDED(rc)) rc = set_window(cam, 1);
    cam->last_result = rc;
    if (R_FAILED(rc)) { film_camera_close(cam); return FILM_DEVICE; }
    cam->format = format;
    return FILM_OK;
}

FilmStatus film_camera_request_30fps(FilmCamera *cam)
{
    if (!cam->initialized || cam->streaming) return FILM_STATE;
    Result rc = CAMU_SetFrameRate(selectors[cam->sensor - FILM_CAMERA_REAR1], FRAME_RATE_30);
    cam->last_result = rc;
    return R_SUCCEEDED(rc) ? FILM_OK : FILM_DEVICE;
}

FilmStatus film_camera_yuv_matrix(FilmCamera *cam, FilmYuv *matrix)
{
    if (!cam->initialized || cam->format != FILM_YUYV) return FILM_STATE;
    Y2RU_StandardCoefficient coefficient;
    Result rc = CAMU_GetSuitableY2rStandardCoefficient(&coefficient);
    cam->last_result = rc;
    if (R_FAILED(rc)) return FILM_DEVICE;
    for (unsigned i = 0; i < sizeof coefficients / sizeof *coefficients; ++i) {
        if (coefficients[i] == coefficient) {
            *matrix = i;
            return FILM_OK;
        }
    }
    return FILM_UNSUPPORTED;
}

FilmStatus film_camera_set_control(FilmCamera *cam, FilmCameraControl control, int value)
{
    if (!cam->initialized) return FILM_STATE;
    const u32 sensor = selectors[cam->sensor - FILM_CAMERA_REAR1];
    Result rc;
    switch (control) {
    case FILM_CAM_EXPOSURE:
        if (value < FILM_EXPOSURE_MIN || value > FILM_EXPOSURE_MAX) return FILM_INVALID;
        rc = CAMU_SetExposure(sensor, (s8)value);
        break;
    case FILM_CAM_WB:
        if (value < WHITE_BALANCE_AUTO || value > WHITE_BALANCE_7000K) return FILM_INVALID;
        rc = CAMU_SetWhiteBalance(sensor, (CAMU_WhiteBalance)value);
        if (R_SUCCEEDED(rc)) rc = CAMU_SetAutoWhiteBalance(sensor, value == WHITE_BALANCE_AUTO);
        break;
    case FILM_CAM_CONTRAST:
        if (value < -1 || value > 1) return FILM_INVALID;
        rc = CAMU_SetContrast(sensor, (CAMU_Contrast)(CONTRAST_NORMAL + value));
        break;
    case FILM_CAM_SHARPNESS:
        if (value < FILM_SHARPNESS_MIN || value > FILM_SHARPNESS_MAX) return FILM_INVALID;
        rc = CAMU_SetSharpness(sensor, (s8)value);
        break;
    default: return FILM_INVALID;
    }
    cam->last_result = rc;
    return R_SUCCEEDED(rc) ? FILM_OK : FILM_DEVICE;
}

FilmCameraAutoState film_camera_auto_state(const FilmCamera *cam)
{
    FilmCameraAutoState state = {0};
    if (!cam->initialized) {
        state.exposure_result = state.white_balance_result =
            MAKERESULT(RL_PERMANENT, RS_INVALIDSTATE, RM_CAM, RD_NOT_INITIALIZED);
        return state;
    }
    u32 select = selectors[cam->sensor - FILM_CAMERA_REAR1];
    state.exposure_result = CAMU_IsAutoExposure(&state.exposure, select);
    state.white_balance_result = CAMU_IsAutoWhiteBalance(&state.white_balance, select);
    return state;
}

void film_camera_log_calibration(FILE *log)
{
    if (!log) return;
    CAMU_ImageQualityCalibrationData data = {0};
    Result rc = CAMU_GetImageQualityCalibrationData(&data);
    if (R_SUCCEEDED(rc))
        fprintf(log, "CAM factory AE %d CCM %d %d %d pos %d R %u %u %u L %u %u %u\n",
                data.aeBaseTarget, data.kRL, data.kGL, data.kBL, data.ccmPosition,
                data.awbCcmL9Right, data.awbCcmL10Right, data.awbX0Right,
                data.awbCcmL9Left, data.awbCcmL10Left, data.awbX0Left);
    else fprintf(log, "CAM factory calibration unavailable: %08lX\n", (unsigned long)(u32)rc);
    fflush(log);
}

static unsigned elapsed_us(u64 start, u64 end)
{
    return (end - start) * UINT64_C(1000000) / SYSCLOCK_ARM11;
}

static Result start_capture(FilmCamera *cam)
{
    if (cam->streaming) return 0;
    Result rc = CAMU_ClearBuffer(cam->port);
    if (R_SUCCEEDED(rc)) rc = CAMU_StartCapture(cam->port);
    cam->streaming = R_SUCCEEDED(rc);
    return rc;
}

static Result arm_preview(FilmCamera *cam)
{
    cam->preview_signaled = false;
    Result rc = CAMU_SetReceiving(&cam->preview_event, cam->preview_dma[cam->preview_pending],
                                  cam->port, frame_bytes(cam), (s16)cam->transfer);
    cam->preview_deadline = osGetTime() + 1500;
    return rc;
}

FilmStatus film_camera_preview_stop(FilmCamera *cam)
{
    if (!cam->preview_dma[0]) return FILM_OK;
    Result rc = CAMU_StopCapture(cam->port);
    Result cleared = CAMU_ClearBuffer(cam->port);
    if (R_SUCCEEDED(rc)) rc = cleared;
    if (R_FAILED(rc)) film_camera_close(cam);
    else {
        cam->streaming = false;
        release_preview(cam);
    }
    cam->last_result = rc;
    return R_SUCCEEDED(rc) ? FILM_OK : FILM_DEVICE;
}

FilmStatus film_camera_set_zoom(FilmCamera *cam, unsigned zoom)
{
    if (!cam || (zoom != 1 && zoom != 2)) return FILM_INVALID;
    if (!cam->initialized) return FILM_STATE;
    if (zoom == cam->zoom) return FILM_OK;
    unsigned old_zoom = cam->zoom;
    FilmStatus status = film_camera_preview_stop(cam);
    if (status != FILM_OK) return status;
    if (cam->streaming) {
        Result stopped = CAMU_StopCapture(cam->port);
        if (R_SUCCEEDED(stopped)) stopped = CAMU_ClearBuffer(cam->port);
        if (R_FAILED(stopped)) {
            film_camera_close(cam);
            cam->last_result = stopped;
            return FILM_DEVICE;
        }
        cam->streaming = false;
    }
    Result rc = set_window(cam, zoom);
    if (R_FAILED(rc) && R_FAILED(set_window(cam, old_zoom))) film_camera_close(cam);
    cam->last_result = rc;
    return R_SUCCEEDED(rc) ? FILM_OK : FILM_DEVICE;
}

FilmStatus film_camera_preview_start(FilmCamera *cam)
{
    if (!cam->initialized) return FILM_STATE;
    if (cam->preview_event) return FILM_OK;
    cam->last_result = 0;
    cam->preview_stage = "DMA";
    for (unsigned i = 0; i < 2; ++i) {
        cam->preview_dma[i] = linearMemAlign(frame_bytes(cam), 0x80);
        if (!cam->preview_dma[i]) { release_preview(cam); return FILM_NOMEM; }
        Result rc = GSPGPU_FlushDataCache(cam->preview_dma[i], frame_bytes(cam));
        if (R_FAILED(rc)) { cam->last_result = rc; release_preview(cam); return FILM_DEVICE; }
    }
    cam->preview_pending = 0;
    cam->preview_stage = "start";
    Result rc = start_capture(cam);
    if (R_SUCCEEDED(rc)) {
        cam->preview_stage = "arm";
        rc = arm_preview(cam);
    }
    if (R_FAILED(rc)) film_camera_preview_stop(cam);
    cam->last_result = rc;
    return R_SUCCEEDED(rc) ? FILM_OK : FILM_DEVICE;
}

FilmStatus film_camera_preview_poll(FilmCamera *cam, FilmImage *image)
{
    *image = (FilmImage){0};
    if (!cam->preview_event) return FILM_STATE;
    cam->preview_stage = "wait";
    Result rc = cam->preview_signaled ? 0 : svcWaitSynchronization(cam->preview_event, 0);
    if (R_DESCRIPTION(rc) == RD_TIMEOUT && osGetTime() < cam->preview_deadline) return FILM_OK;
    bool received = false;
    if (R_SUCCEEDED(rc)) {
        cam->preview_stage = "receive";
        cam->preview_signaled = true;
        rc = CAMU_IsFinishedReceiving(&received, cam->port);
    }
    if (R_SUCCEEDED(rc) && !received && osGetTime() < cam->preview_deadline) return FILM_OK;
    if (R_SUCCEEDED(rc) && received) {
        uint8_t *pixels = cam->preview_dma[cam->preview_pending];
        svcCloseHandle(cam->preview_event);
        cam->preview_event = 0;
        cam->preview_stage = "cache";
        rc = GSPGPU_InvalidateDataCache(pixels, frame_bytes(cam));
        if (R_SUCCEEDED(rc)) {
            cam->preview_pending ^= 1;
            cam->preview_stage = "rearm";
            rc = arm_preview(cam);
        }
        if (R_SUCCEEDED(rc)) {
            cam->preview_ready = true;
            *image = (FilmImage){WIDTH / cam->zoom, HEIGHT / cam->zoom, cam->format, pixels};
            return FILM_OK;
        }
    }
    film_camera_preview_stop(cam);
    cam->last_result = rc;
    return FILM_DEVICE;
}

static void copy_capture(FilmCamera *cam, FilmFrame *frame, FilmImage source, const uint8_t *dma)
{
    memcpy(source.pixels, dma, frame_bytes(cam));
    frame->source = source;
    frame->camera = cam->sensor;
    time_t now = time(NULL);
    frame->captured_at = now < 0 ? 0 : (uint64_t)now;
}

FilmStatus film_camera_preview_snapshot(FilmCamera *cam, FilmFrame *frame)
{
    if (!cam->preview_ready) return FILM_STATE;
    FilmImage source = {0};
    FilmStatus status = film_image_alloc(&source, WIDTH / cam->zoom, HEIGHT / cam->zoom, cam->format);
    if (status == FILM_OK)
        copy_capture(cam, frame, source, cam->preview_dma[cam->preview_pending ^ 1]);
    return status;
}

static FilmStatus capture(FilmCamera *cam, FilmFrame *frame, uint8_t *linear,
                          bool keep_stream, FilmCaptureTiming *timing)
{
    if (!cam->initialized || cam->preview_dma[0]) return FILM_STATE;
    u64 start = timing ? svcGetSystemTick() : 0;
    cam->last_result = 0;
    unsigned bytes = frame_bytes(cam);
    FilmImage source = {0};
    FilmStatus status = FILM_OK;
    if (frame) status = film_image_alloc(&source, WIDTH / cam->zoom, HEIGHT / cam->zoom, cam->format);
    if (status != FILM_OK) return status;
    void *dma = linear ? linear : linearMemAlign(bytes, 0x80);
    if (!dma) { film_image_free(&source); return FILM_NOMEM; }
    Handle complete = 0;
    Result rc = GSPGPU_FlushDataCache(dma, bytes);
    if (R_SUCCEEDED(rc)) rc = start_capture(cam);
    u64 setup = timing ? svcGetSystemTick() : 0;
    if (R_SUCCEEDED(rc)) rc = CAMU_SetReceiving(&complete, dma, cam->port, bytes, (s16)cam->transfer);
    u64 armed = timing ? svcGetSystemTick() : 0;
    if (R_SUCCEEDED(rc)) rc = svcWaitSynchronization(complete, capture_timeout);
    u64 waited = timing ? svcGetSystemTick() : 0;
    bool received = false;
    if (R_SUCCEEDED(rc)) rc = CAMU_IsFinishedReceiving(&received, cam->port);
    if (!keep_stream || R_FAILED(rc) || !received) {
        Result stopped = CAMU_StopCapture(cam->port);
        Result cleared = CAMU_ClearBuffer(cam->port);
        cam->streaming = false;
        if (R_FAILED(stopped) || R_FAILED(cleared)) {
            if (R_SUCCEEDED(rc)) rc = R_FAILED(stopped) ? stopped : cleared;
            film_camera_close(cam);
        }
    }
    if (complete) svcCloseHandle(complete);
    if (R_SUCCEEDED(rc) && received) rc = GSPGPU_InvalidateDataCache(dma, bytes);
    cam->last_result = rc;
    if (R_FAILED(rc) || !received) {
        film_image_free(&source);
        status = FILM_DEVICE;
    } else if (frame) copy_capture(cam, frame, source, dma);
    if (!linear) linearFree(dma);
    if (timing) {
        u64 end = svcGetSystemTick();
        *timing = (FilmCaptureTiming){elapsed_us(start, setup), elapsed_us(setup, armed),
                                     elapsed_us(armed, waited), elapsed_us(waited, end), elapsed_us(start, end)};
    }
    return status;
}

static bool valid_linear(const FilmImage *image, FilmPixelFormat format)
{
    return image && image->pixels && (uintptr_t)image->pixels % 0x80 == 0 &&
           image->width == WIDTH && image->height == HEIGHT && image->format == format;
}

FilmStatus film_camera_measure(FilmCamera *cam, FilmFrame *frame, bool keep_stream, FilmCaptureTiming *timing)
{
    if (!cam || !frame) return FILM_INVALID;
    return capture(cam, frame, NULL, keep_stream, timing);
}

FilmStatus film_camera_receive(FilmCamera *cam, FilmImage *linear, bool keep_stream, FilmCaptureTiming *timing)
{
    if (!cam || !cam->initialized || !linear || !linear->pixels ||
        (uintptr_t)linear->pixels % 0x80 != 0 || linear->format != cam->format ||
        linear->width != WIDTH / cam->zoom || linear->height != HEIGHT / cam->zoom)
        return FILM_INVALID;
    return capture(cam, NULL, linear->pixels, keep_stream, timing);
}

FilmStatus film_camera_capture(FilmCamera *cam, FilmFrame *frame)
{
    return film_camera_measure(cam, frame, false, NULL);
}

FilmStatus film_y2r_transfer(const FilmImage *image, FilmYuv matrix, unsigned transfer_lines,
                             FilmImage *out, uint32_t *result)
{
    if (result) *result = 0;
    if (!valid_linear(image, FILM_YUYV) || !valid_linear(out, FILM_RGB24) ||
        image->pixels == out->pixels || (unsigned)matrix > FILM_709_LIMITED ||
        (transfer_lines != 2 && transfer_lines != 4 && transfer_lines != 8)) return FILM_INVALID;
    const u32 output_bytes = WIDTH * HEIGHT * 3;
    uint8_t *input = image->pixels;
    uint8_t *output = out->pixels;
    Result rc = y2rInit();
    bool initialized = R_SUCCEEDED(rc), done = false;
    Y2RU_ConversionParams params = {.input_format = INPUT_YUV422_BATCH, .output_format = OUTPUT_RGB_24,
        .block_alignment = BLOCK_LINE, .rotation = ROTATION_NONE, .input_line_width = WIDTH,
        .input_lines = HEIGHT, .standard_coefficient = coefficients[matrix]};
    if (R_SUCCEEDED(rc)) rc = GSPGPU_FlushDataCache(input, CAMERA_BYTES);
    if (R_SUCCEEDED(rc)) rc = GSPGPU_FlushDataCache(output, output_bytes);
    if (R_SUCCEEDED(rc)) rc = Y2RU_SetConversionParams(&params);
    if (R_SUCCEEDED(rc)) rc = Y2RU_SetSendingYUYV(input, CAMERA_BYTES, WIDTH * 2 * transfer_lines, 0);
    if (R_SUCCEEDED(rc)) rc = Y2RU_SetReceiving(output, output_bytes, WIDTH * 3 * transfer_lines, 0);
    if (R_SUCCEEDED(rc)) rc = Y2RU_StartConversion();
    u64 deadline = osGetTime() + 1000;
    while (R_SUCCEEDED(rc) && !done && osGetTime() < deadline) {
        rc = Y2RU_IsDoneReceiving(&done);
        if (!done) svcSleepThread(1000000);
    }
    if (initialized) {
        Result stopped = Y2RU_StopConversion();
        if (R_SUCCEEDED(rc)) rc = stopped;
        y2rExit();
    }
    if (R_SUCCEEDED(rc) && done) rc = GSPGPU_InvalidateDataCache(output, output_bytes);
    if (result) *result = rc;
    return R_SUCCEEDED(rc) && done ? FILM_OK : FILM_DEVICE;
}

FilmStatus film_y2r_convert(const FilmImage *image, FilmYuv matrix, unsigned transfer_lines,
                            FilmImage *out, uint32_t *result)
{
    if (result) *result = 0;
    if (!image || !out || !image->pixels || image->width != WIDTH || image->height != HEIGHT ||
        image->format != FILM_YUYV) return FILM_INVALID;
    FilmImage input = *image, bgr = {WIDTH, HEIGHT, FILM_RGB24, NULL};
    input.pixels = linearMemAlign(CAMERA_BYTES, 0x80);
    bgr.pixels = linearMemAlign(WIDTH * HEIGHT * 3, 0x80);
    FilmStatus status = FILM_NOMEM;
    if (input.pixels && bgr.pixels) {
        memcpy(input.pixels, image->pixels, CAMERA_BYTES);
        status = film_y2r_transfer(&input, matrix, transfer_lines, &bgr, result);
    }
    if (status == FILM_OK) {
        status = film_image_alloc(out, WIDTH, HEIGHT, FILM_RGB24);
        if (status == FILM_OK) {
            // Packed 3DS RGB8 stores B, G, R bytes in memory.
            for (unsigned i = 0; i < WIDTH * HEIGHT * 3; i += 3) {
                out->pixels[i] = bgr.pixels[i + 2];
                out->pixels[i + 1] = bgr.pixels[i + 1];
                out->pixels[i + 2] = bgr.pixels[i];
            }
        }
    }
    if (input.pixels) linearFree(input.pixels);
    if (bgr.pixels) linearFree(bgr.pixels);
    return status;
}
