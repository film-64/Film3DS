#include "platform_camera.h"

#include <3ds.h>
#include <limits.h>
#include <string.h>
#include <time.h>

enum { WIDTH = 640, HEIGHT = 480, CAMERA_BYTES = WIDTH * HEIGHT * 2 };
static const s64 capture_timeout = INT64_C(1500000000);

void film_camera_close(FilmCamera *cam)
{
    if (!cam->initialized) return;
    CAMU_StopCapture(PORT_CAM1);
    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_Activate(SELECT_NONE);
    camExit();
    cam->initialized = false;
}

FilmStatus film_camera_open(FilmCamera *cam, FilmPixelFormat format)
{
    if (!cam || (format != FILM_YUYV && format != FILM_RGB565_LE)) return FILM_INVALID;
    if (cam->initialized) return cam->format == format ? FILM_OK : FILM_STATE;
    cam->last_result = 0;
    Result rc = camInit();
    if (R_FAILED(rc)) { cam->last_result = rc; return FILM_DEVICE; }
    cam->initialized = true;
    u32 transfer = 0;
    rc = CAMU_Activate(SELECT_OUT1);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetSize(SELECT_OUT1, SIZE_VGA, CONTEXT_A);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetOutputFormat(SELECT_OUT1,
        format == FILM_YUYV ? OUTPUT_YUV_422 : OUTPUT_RGB_565, CONTEXT_A);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetAutoExposure(SELECT_OUT1, true);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetTrimming(PORT_CAM1, false);
    if (R_SUCCEEDED(rc)) rc = CAMU_GetMaxBytes(&transfer, WIDTH, HEIGHT);
    if (R_FAILED(rc) || !transfer || transfer > INT16_MAX || CAMERA_BYTES % transfer) {
        cam->last_result = rc;
        film_camera_close(cam);
        return FILM_DEVICE;
    }
    rc = CAMU_SetTransferBytes(PORT_CAM1, transfer, WIDTH, HEIGHT);
    cam->last_result = rc;
    if (R_FAILED(rc)) { film_camera_close(cam); return FILM_DEVICE; }
    cam->transfer = transfer;
    cam->format = format;
    return FILM_OK;
}

FilmStatus film_camera_capture(FilmCamera *cam, FilmFrame *frame)
{
    if (!cam || !frame) return FILM_INVALID;
    if (!cam->initialized) return FILM_STATE;
    cam->last_result = 0;
    FilmImage source = {0};
    FilmStatus status = film_image_alloc(&source, WIDTH, HEIGHT, cam->format);
    if (status != FILM_OK) return status;
    void *dma = linearMemAlign(CAMERA_BYTES, 0x80);
    if (!dma) { film_image_free(&source); return FILM_NOMEM; }
    Handle complete = 0;
    Result rc = GSPGPU_FlushDataCache(dma, CAMERA_BYTES);
    if (R_SUCCEEDED(rc)) rc = CAMU_ClearBuffer(PORT_CAM1);
    if (R_SUCCEEDED(rc)) rc = CAMU_StartCapture(PORT_CAM1);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetReceiving(&complete, dma, PORT_CAM1, CAMERA_BYTES, (s16)cam->transfer);
    if (R_SUCCEEDED(rc)) rc = svcWaitSynchronization(complete, capture_timeout);
    bool received = false;
    if (R_SUCCEEDED(rc)) rc = CAMU_IsFinishedReceiving(&received, PORT_CAM1);
    Result stopped = CAMU_StopCapture(PORT_CAM1);
    Result cleared = CAMU_ClearBuffer(PORT_CAM1);
    if (complete) svcCloseHandle(complete);
    if (R_FAILED(stopped) || R_FAILED(cleared)) {
        if (R_SUCCEEDED(rc)) rc = R_FAILED(stopped) ? stopped : cleared;
        film_camera_close(cam);
    }
    if (R_SUCCEEDED(rc) && received) rc = GSPGPU_InvalidateDataCache(dma, CAMERA_BYTES);
    cam->last_result = rc;
    if (R_SUCCEEDED(rc) && received) {
        memcpy(source.pixels, dma, CAMERA_BYTES);
        frame->source = source;
        frame->camera = 1;
        time_t now = time(NULL);
        frame->captured_at = now < 0 ? 0 : (uint64_t)now;
    } else {
        film_image_free(&source);
        status = FILM_DEVICE;
    }
    linearFree(dma);
    return status;
}

FilmStatus film_y2r_convert(const FilmImage *image, FilmYuv matrix, FilmImage *out, uint32_t *result)
{
    if (result) *result = 0;
    if (!image || !out || !image->pixels || image->width != WIDTH || image->height != HEIGHT ||
        image->format != FILM_YUYV || (unsigned)matrix > FILM_709_LIMITED) return FILM_INVALID;
    const u32 output_bytes = WIDTH * HEIGHT * 3;
    FilmImage rgb = {0};
    FilmStatus status = film_image_alloc(&rgb, WIDTH, HEIGHT, FILM_RGB24);
    if (status != FILM_OK) return status;
    uint8_t *input = linearMemAlign(CAMERA_BYTES, 0x80);
    uint8_t *output = linearMemAlign(output_bytes, 0x80);
    if (!input || !output) {
        if (input) linearFree(input);
        if (output) linearFree(output);
        film_image_free(&rgb);
        return FILM_NOMEM;
    }
    memcpy(input, image->pixels, CAMERA_BYTES);
    Result rc = y2rInit();
    bool initialized = R_SUCCEEDED(rc), done = false;
    const Y2RU_StandardCoefficient coefficients[] = {COEFFICIENT_ITU_R_BT_601, COEFFICIENT_ITU_R_BT_601_SCALING,
        COEFFICIENT_ITU_R_BT_709, COEFFICIENT_ITU_R_BT_709_SCALING};
    Y2RU_ConversionParams params = {.input_format = INPUT_YUV422_BATCH, .output_format = OUTPUT_RGB_24,
        .block_alignment = BLOCK_LINE, .rotation = ROTATION_NONE, .input_line_width = WIDTH,
        .input_lines = HEIGHT, .standard_coefficient = coefficients[matrix]};
    if (R_SUCCEEDED(rc)) rc = GSPGPU_FlushDataCache(input, CAMERA_BYTES);
    if (R_SUCCEEDED(rc)) rc = GSPGPU_FlushDataCache(output, output_bytes);
    if (R_SUCCEEDED(rc)) rc = Y2RU_SetConversionParams(&params);
    if (R_SUCCEEDED(rc)) rc = Y2RU_SetSendingYUYV(input, CAMERA_BYTES, WIDTH * 2 * 4, 0);
    if (R_SUCCEEDED(rc)) rc = Y2RU_SetReceiving(output, output_bytes, WIDTH * 3 * 4, 0);
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
    if (R_SUCCEEDED(rc) && done) {
        // Packed 3DS RGB8 uses little-endian B, G, R bytes; verify against the CPU on-device.
        for (u32 i = 0; i < output_bytes; i += 3) {
            rgb.pixels[i] = output[i + 2];
            rgb.pixels[i + 1] = output[i + 1];
            rgb.pixels[i + 2] = output[i];
        }
        *out = rgb;
    } else {
        film_image_free(&rgb);
        status = FILM_DEVICE;
    }
    linearFree(input);
    linearFree(output);
    return status;
}
