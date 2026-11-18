#include "platform_camera.h"
#include <3ds.h>
#include <string.h>

#ifndef FILM_CAMERA_VERBOSE
#define FILM_CAMERA_VERBOSE 0
#endif

enum { WIDTH = 640, HEIGHT = 480, CAMERA_BYTES = WIDTH * HEIGHT * 2 };

static int32_t register_open(void *context, const char *service, uint32_t *handle)
{
    (void)context;
    return srvGetServiceHandle(handle, service);
}
static int32_t register_exchange(void *context, uint32_t handle,
                                 const uint32_t request[4], uint32_t reply[3])
{
    (void)context;
    u32 *buffer = getThreadCommandBuffer();
    memset(buffer, 0, 64 * sizeof *buffer);
    memcpy(buffer, request, 4 * sizeof *request);
    Result rc = svcSendSyncRequest(handle);
    /* Copy before clocks, filesystem or other services reuse this buffer. */
    memcpy(reply, buffer, 3 * sizeof *reply);
    return rc;
}
static void register_close(void *context, uint32_t handle)
{
    (void)context;
    svcCloseHandle(handle);
}

static FilmStatus hardware_exchange(FilmCamera *cam, bool writing, bool mcu,
                                    uint16_t address, uint16_t *value)
{
    if(atomic_load(&cam->debug_mode)&&cam->exposure_trace){fprintf(cam->exposure_trace,"MANUAL begin %s %s %04x\n",writing?"write":"read",mcu?"mcu":"reg",address);fflush(cam->exposure_trace);}
    FilmStatus status = film_register_access(&cam->exposure_transport, writing, mcu, address, value);
    cam->last_result = cam->exposure_transport.native_result;
    if (cam->exposure_trace && (FILM_CAMERA_VERBOSE || atomic_load(&cam->debug_mode) || status != FILM_OK))
        fprintf(cam->exposure_trace, "HW15 ms=%llu %s %s %04x %u %08lX\n",
                (unsigned long long)osGetTime(), writing ? "write" : "read",
                mcu ? "mcu" : "reg", address, *value, (unsigned long)cam->last_result);
    return status;
}

static FilmStatus hardware_read(void *context, bool mcu, uint16_t a, uint16_t *v)
{
    *v = 0;
    return hardware_exchange(context, false, mcu, a, v);
}
static FilmStatus hardware_write(void *context, bool mcu, uint16_t a, uint16_t v)
{
    return hardware_exchange(context, true, mcu, a, &v);
}
static void hardware_delay(void *context, unsigned ms)
{
    (void)context;
    svcSleepThread((s64)ms * 1000000);
}

static FilmStatus hardware_seed(FilmCamera *cam)
{
    /* The proven setup captures native AE/AWB seed frames before locking.
     * One temporary DMA buffer, fixed frame count, no retry or saved image. */
    FilmImage image = {WIDTH, HEIGHT, cam->format, linearMemAlign(CAMERA_BYTES, 0x80)};
    if (!image.pixels) return FILM_NOMEM;
    FilmStatus status = FILM_OK;
    for (unsigned i = 0; i < FILM_HW_SEED_FRAMES; ++i) {
        status = film_camera_receive(cam, &image, true, NULL);
        if (cam->exposure_trace) fprintf(cam->exposure_trace, "HW15 native_seed frame=%u status=%s result=%08lX\n",
                i, film_status_name(status), (unsigned long)cam->last_result);
        if (status != FILM_OK) break;
    }
    if (cam->initialized) {
        Result stopped = film_camera_stop_port(cam->port,cam->exposure_trace,&cam->dma_safe);
        Result cleared = 0;
        cam->streaming = false;
        if (status == FILM_OK && (R_FAILED(stopped) || R_FAILED(cleared))) {
            cam->last_result = R_FAILED(stopped) ? stopped : cleared;
            status = FILM_DEVICE;
        }
    }
    if(cam->dma_safe)linearFree(image.pixels);
    return status;
}

FilmStatus film_camera_enable_manual_exposure(FilmCamera *cam, FILE *local_trace)
{
    if (!cam || !cam->initialized || cam->streaming || cam->preview_event) return FILM_STATE;
    if (cam->exposure.active || cam->exposure.saved_valid) return FILM_STATE;
    if (cam->sensor != FILM_CAMERA_REAR1) return FILM_UNSUPPORTED;
    cam->exposure_requested = true;
    cam->exposure = (FilmHardwareExposure){0};
    cam->exposure_trace = local_trace;
    cam->preview_stage = "cam:c open";
    FilmStatus status = film_register_open(&cam->exposure_transport,
        (FilmRegisterIo){cam, register_open, register_exchange, register_close});
    cam->last_result = cam->exposure_transport.native_result;
    if (status != FILM_OK) { cam->exposure.failed = true; film_camera_close_manual_exposure(cam); return status; }
    cam->preview_stage = "native setup";
    Result rc = CAMU_SetPhotoMode(SELECT_OUT1, PHOTO_MODE_NORMAL);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_15);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetExposure(SELECT_OUT1, 0);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
    if (R_SUCCEEDED(rc)) rc = CAMU_SetAutoExposure(SELECT_OUT1, true);
    cam->last_result = rc;
    status = R_SUCCEEDED(rc) ? FILM_OK : FILM_DEVICE;
    if (status == FILM_OK) {
        cam->preview_stage = "native seed";
        status = hardware_seed(cam);
    }
    if (status == FILM_OK) {
        cam->preview_stage = "native lock";
        rc = CAMU_SetAutoWhiteBalance(SELECT_OUT1, false);
        if (R_SUCCEEDED(rc)) rc = CAMU_SetAutoExposure(SELECT_OUT1, false);
        cam->last_result = rc;
        if (R_FAILED(rc)) status = FILM_DEVICE;
    }
    if(status==FILM_OK){bool ae=true;rc=film_camera_read_bool(0x1a,SELECT_OUT1,&ae,local_trace);cam->last_result=rc;
        if(R_FAILED(rc)||ae)status=FILM_DEVICE;}
    if (status == FILM_OK) {
        cam->preview_stage = "register init";
        status = film_hardware_begin(&cam->exposure,
        (FilmHardwareIo){cam, hardware_read, hardware_write, hardware_delay},
        "film-camera-manual-setup/v1");
    }
    if(local_trace){fprintf(local_trace, "MANUAL source=film-camera-manual-setup/v1 begin=%s nominal_EV_only=1\n", film_status_name(status));
    fflush(local_trace);}
    if (status != FILM_OK) { cam->exposure.failed = true; film_camera_close_manual_exposure(cam); }
    return status;
}


FilmStatus film_camera_progress_manual_exposure(FilmCamera *cam)
{
    if (!cam->exposure_requested || !cam->exposure.active || cam->exposure.failed) return FILM_STATE;
    if (cam->exposure.ready) return FILM_OK;
    FilmStatus status = film_hardware_frame(&cam->exposure);
    if (cam->exposure_trace) fprintf(cam->exposure_trace, "MANUAL settle remaining=%u ready=%d status=%s\n",
        cam->exposure.remaining, cam->exposure.ready, film_status_name(status));
    return status;
}

FilmStatus film_camera_verify_manual_capture(FilmCamera *cam)
{
    if (!cam->exposure_requested || !cam->exposure.ready || cam->exposure.failed) return FILM_STATE;
    bool ae=true;
    Result rc=film_camera_read_bool(0x1a,SELECT_OUT1,&ae,atomic_load(&cam->debug_mode)?cam->runtime_log:NULL);
    cam->last_result=rc;
    if(R_FAILED(rc)||ae)return FILM_STATE;
    return film_hardware_verify(&cam->exposure);
}

void film_camera_close_manual_exposure(FilmCamera *cam)
{
    uint32_t result = cam->last_result;
    FilmStatus restored = film_hardware_restore(&cam->exposure);
    if (cam->exposure_trace) {
        fprintf(cam->exposure_trace, "MANUAL restore=%s\n", film_status_name(restored));
        fflush(cam->exposure_trace);
    }
    film_register_close(&cam->exposure_transport);
    cam->last_result = result;
}

FilmStatus film_camera_set_ae_compensation(FilmCamera *cam,int value)
{
    if(!film_runtime_source_valid("film-camera-exposure-mode/v1"))return FILM_STATE;
    if(!cam||!cam->initialized||cam->exposure_requested||cam->exposure_mode!=FILM_EXPOSURE_NATIVE_AE)return FILM_STATE;
    if(value<FILM_EXPOSURE_MIN||value>FILM_EXPOSURE_MAX)return FILM_INVALID;
    film_camera_raw_invalidate(cam);
    u32 select=cam->sensor==FILM_CAMERA_REAR1?SELECT_OUT1:cam->sensor==FILM_CAMERA_REAR2?SELECT_OUT2:SELECT_IN1;
    Result rc=CAMU_SetExposure(select,(s8)value);bool enabled=false;
    if(R_SUCCEEDED(rc))rc=film_camera_read_bool(0x1a,select,&enabled,atomic_load(&cam->debug_mode)?cam->runtime_log:NULL);
    cam->last_result=rc;
    FilmStatus status=R_SUCCEEDED(rc)&&enabled?FILM_OK:FILM_DEVICE;
    if(status!=FILM_OK)film_camera_close(cam);
    return status;
}

FilmStatus film_camera_select_exposure_mode(FilmCamera *cam,FilmExposureMode mode,int compensation,FILE *trace)
{
    if(!film_runtime_source_valid("film-camera-exposure-mode/v1"))return FILM_STATE;
    if(!cam||!cam->initialized||(unsigned)mode>FILM_EXPOSURE_MANUAL||
       compensation<FILM_EXPOSURE_MIN||compensation>FILM_EXPOSURE_MAX)return FILM_INVALID;
    if(mode==FILM_EXPOSURE_MANUAL&&cam->sensor!=FILM_CAMERA_REAR1)return FILM_UNSUPPORTED;
    film_camera_raw_invalidate(cam);
    cam->ae_window_applied=false;
    FilmStatus status=film_camera_preview_stop(cam);
    if(status!=FILM_OK)return status;
    if(cam->exposure_requested){
        status=film_hardware_restore(&cam->exposure);
        film_register_close(&cam->exposure_transport);cam->exposure_requested=false;
        cam->exposure_trace=NULL;
        cam->exposure=(FilmHardwareExposure){0};
        if(status!=FILM_OK){film_camera_close(cam);return status;}
    }
    if(mode==FILM_EXPOSURE_MANUAL)status=film_camera_enable_manual_exposure(cam,trace);
    else {
        u32 select=cam->sensor==FILM_CAMERA_REAR1?SELECT_OUT1:cam->sensor==FILM_CAMERA_REAR2?SELECT_OUT2:SELECT_IN1;
        Result rc=CAMU_SetAutoExposure(select,true);bool enabled=false;
        if(R_SUCCEEDED(rc))rc=CAMU_SetAutoWhiteBalance(select,true);
        if(R_SUCCEEDED(rc))rc=film_camera_read_bool(0x1a,select,&enabled,atomic_load(&cam->debug_mode)?cam->runtime_log:NULL);
        cam->last_result=rc;status=R_SUCCEEDED(rc)&&enabled?FILM_OK:FILM_DEVICE;
        cam->exposure_mode=FILM_EXPOSURE_NATIVE_AE;
        if(status==FILM_OK)status=film_camera_set_ae_compensation(cam,compensation);
    }
    if(status==FILM_OK)cam->exposure_mode=mode;
    if(trace){fprintf(trace,"source=film-camera-exposure-mode/v1 mode=%s status=%s native_comp=%d\n",
        mode==FILM_EXPOSURE_MANUAL?"manual":"native_AE",film_status_name(status),compensation);fflush(trace);}
    if(status!=FILM_OK)film_camera_close(cam);
    return status;
}
