#ifndef FILM_PLATFORM_CAMERA_H
#define FILM_PLATFORM_CAMERA_H
#include "film.h"

typedef enum { FILM_CAMERA_REAR1 = 1, FILM_CAMERA_REAR2, FILM_CAMERA_FRONT } FilmSensor;
typedef enum {
    FILM_CAM_EXPOSURE, FILM_CAM_METERING, FILM_CAM_EV_COMP, FILM_CAM_WB,
    FILM_CAM_CONTRAST, FILM_CAM_SATURATION, FILM_CAM_SHARPNESS, FILM_CAM_CONTROL_COUNT
} FilmCameraControl;
typedef enum { FILM_METER_MANUAL, FILM_METER_AVERAGE, FILM_METER_CENTER } FilmMeterMode;
enum { FILM_EXPOSURE_MIN = -5, FILM_EXPOSURE_MAX = 5 };
enum { FILM_SHARPNESS_MIN = -2, FILM_SHARPNESS_MAX = 2 };
#define FILM_METER_TOLERANCE_EV .25f

typedef struct {
    bool initialized;
    bool streaming;
    FilmSensor sensor;
    unsigned port;
    unsigned transfer, zoom;
    FilmPixelFormat format;
    uint32_t last_result;
    const char *preview_stage;
    uint8_t *preview_dma[2];
    uint32_t preview_event;
    uint64_t preview_deadline;
    unsigned preview_pending;
    bool preview_ready;
    bool preview_signaled;
} FilmCamera;

typedef struct {
    unsigned setup_us, arm_us, wait_us, finish_us, total_us;
} FilmCaptureTiming;

typedef struct {
    bool exposure, white_balance;
    uint32_t exposure_result, white_balance_result;
} FilmCameraAutoState;

enum { FILM_STABILITY_WINDOW = 4 };
typedef struct { unsigned median; double cb, cr; float metered_luma; } FilmCameraColorStats;
typedef struct {
    FilmCameraColorStats samples[FILM_STABILITY_WINDOW];
    unsigned count, next;
} FilmCameraStability;
typedef struct {
    unsigned y_span;
    double cb_span, cr_span;
    bool ready, exposure, white_balance, metering;
} FilmCameraStabilityResult;
typedef struct { float luma, highlights; } FilmMeterReading;
typedef struct {
    FilmMeterReading reference;
    int previous_step;
    bool has_previous, hold;
} FilmExposureMeter;

void film_camera_stability_push(FilmCameraStability *window, FilmCameraColorStats sample);
FilmMeterReading film_camera_sample_preview(FilmCameraStability *window, const FilmImage *image,
                                            FilmYuv yuv, FilmAspect aspect, FilmMeterMode mode);
FilmCameraStabilityResult film_camera_stability_read(const FilmCameraStability *window);
float film_camera_meter_error_ev(FilmMeterReading sample, int ev_thirds);
int film_camera_meter_step(FilmExposureMeter *meter, int step, FilmMeterReading sample, int ev_thirds);

FilmStatus film_camera_open(FilmCamera *camera, FilmPixelFormat format, FilmSensor sensor);
FilmStatus film_camera_capture(FilmCamera *camera, FilmFrame *frame);
FilmStatus film_camera_measure(FilmCamera *camera, FilmFrame *frame, bool keep_stream, FilmCaptureTiming *timing);
FilmStatus film_camera_receive(FilmCamera *camera, FilmImage *linear, bool keep_stream, FilmCaptureTiming *timing);
FilmStatus film_camera_request_30fps(FilmCamera *camera);
FilmStatus film_camera_yuv_matrix(FilmCamera *camera, FilmYuv *matrix);
FilmStatus film_camera_set_control(FilmCamera *camera, FilmCameraControl control, int value);
FilmCameraAutoState film_camera_auto_state(const FilmCamera *camera);
FilmStatus film_camera_set_zoom(FilmCamera *camera, unsigned zoom);
void film_camera_log_calibration(FILE *log);
FilmStatus film_camera_preview_start(FilmCamera *camera);
FilmStatus film_camera_preview_poll(FilmCamera *camera, FilmImage *image);
FilmStatus film_camera_preview_snapshot(FilmCamera *camera, FilmFrame *frame);
FilmStatus film_camera_preview_stop(FilmCamera *camera);
void film_camera_close(FilmCamera *camera);
FilmStatus film_y2r_transfer(const FilmImage *linear_yuyv, FilmYuv matrix, unsigned transfer_lines,
                             FilmImage *linear_bgr, uint32_t *result);
FilmStatus film_y2r_convert(const FilmImage *image, FilmYuv matrix, unsigned transfer_lines,
                            FilmImage *out, uint32_t *result);
#endif
