#include "platform_qa.h"
#include "platform_camera.h"

#include <3ds.h>
#include <3ds/3dslink.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

static int send_report(void *context, const char *data, size_t length)
{
    FilmHardwareQA *qa = context;
    u64 deadline = osGetTime() + 8000;
    size_t total = 0;
    while (total < length) {
        u64 now = osGetTime();
        if (now >= deadline) { errno = ETIMEDOUT; return -1; }
        struct pollfd socket = {qa->socket_fd, POLLOUT, 0};
        int ready = poll(&socket, 1, (int)(deadline - now));
        if (ready < 0) return -1;
        if (!ready) { errno = ETIMEDOUT; return -1; }
        if (!(socket.revents & POLLOUT)) { errno = EPIPE; return -1; }
        ssize_t sent = send(qa->socket_fd, data + total, length - total, 0);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (sent <= 0) return -1;
        total += sent;
    }
    return (int)total;
}

bool film_hardware_qa_open(FilmHardwareQA *qa)
{
    enum { SOC_BYTES = 0x100000 };
    qa->socket_fd = -1;
    if (!__3dslink_host.s_addr) return false;
    qa->soc_buffer = memalign(0x1000, SOC_BYTES);
    if (!qa->soc_buffer) return false;
    if (R_FAILED(socInit(qa->soc_buffer, SOC_BYTES))) {
        free(qa->soc_buffer);
        qa->soc_buffer = NULL;
        return false;
    }
    qa->socket_fd = link3dsConnectToHost(false, false);
    if (qa->socket_fd >= 0 && fcntl(qa->socket_fd, F_SETFL, O_NONBLOCK) == 0) {
        qa->output = funopen(qa, NULL, send_report, NULL, NULL);
        if (qa->output) return true;
    }
    film_hardware_qa_close(qa);
    return false;
}

void film_hardware_qa_close(FilmHardwareQA *qa)
{
    if (qa->output) fclose(qa->output);
    if (qa->socket_fd >= 0) close(qa->socket_fd);
    if (qa->soc_buffer) {
        socExit();
        free(qa->soc_buffer);
    }
    *qa = (FilmHardwareQA){.socket_fd = -1};
}

bool film_hardware_qa_confirm(FilmHardwareQA *qa)
{
    if (fflush(qa->output) != 0 || ferror(qa->output)) return false;
    struct pollfd socket = {qa->socket_fd, POLLIN, 0};
    char reply;
    return poll(&socket, 1, 8000) > 0 && (socket.revents & POLLIN) &&
           recv(qa->socket_fd, &reply, sizeof reply, 0) == 1 && reply == 'K';
}

typedef struct {
    const char *name;
    FilmCameraControl control;
    int value;
    int restore;
} CameraCase;

static const CameraCase cases[] = {
    {"base", FILM_CAM_EXPOSURE, 0, 0},
    {"exposure_low", FILM_CAM_EXPOSURE, -5, 0},
    {"exposure_high", FILM_CAM_EXPOSURE, 5, 0},
    {"contrast_low", FILM_CAM_CONTRAST, -1, 0},
    {"contrast_high", FILM_CAM_CONTRAST, 1, 0},
};

static unsigned percentile(const uint32_t hist[256], uint32_t count, unsigned percent)
{
    uint32_t cutoff = (count * percent + 99) / 100;
    for (unsigned i = 0; i < 256; ++i) {
        if (hist[i] >= cutoff) return i;
        cutoff -= hist[i];
    }
    return 255;
}

static FilmCameraColorStats measure_color(FILE *output, const FilmImage *raw, unsigned sensor, const char *name,
                                         FilmYuv matrix, bool matrix_known, unsigned elapsed, unsigned frame)
{
    uint32_t hist[256] = {0}, sum_y = 0, sum_cb = 0, sum_cr = 0;
    uint64_t edges = 0;
    unsigned count = raw->width * raw->height;
    unsigned row_end = raw->width * 2;
    for (unsigned i = 0; i < count * 2; i += 4) {
        const uint8_t *p = raw->pixels + i;
        ++hist[p[0]];
        ++hist[p[2]];
        sum_y += p[0] + p[2];
        sum_cb += p[1];
        sum_cr += p[3];
        edges += abs(p[0] - p[2]);
        if (i + 4 < row_end) edges += abs(p[2] - p[4]);
        else row_end += raw->width * 2;
    }
    bool limited = matrix == FILM_601_LIMITED || matrix == FILM_709_LIMITED;
    unsigned low = 0, high = 0;
    for (unsigned i = 0; i <= (limited ? 16u : 0u); ++i) low += hist[i];
    for (unsigned i = limited ? 235 : 255; i < 256; ++i) high += hist[i];
    FilmCameraColorStats stats = {.median = percentile(hist, count, 50),
        .cb = sum_cb / (count / 2.0), .cr = sum_cr / (count / 2.0)};
    fprintf(output, "COLOR sensor=%u case=%s frame=%u zoom=%u matrix=%u matrix_known=%d capture_us=%u "
            "y_p01=%u y_p50=%u y_p99=%u y_mean=%.3f cb_mean=%.3f cr_mean=%.3f "
            "y_low=%u y_high=%u pixels=%u y_edge=%.3f\n",
            sensor, name, frame, 640 / raw->width, matrix, matrix_known, elapsed, percentile(hist, count, 1),
            stats.median, percentile(hist, count, 99), sum_y / (double)count,
            stats.cb, stats.cr, low, high, count, edges / (double)(count - raw->height));
    return stats;
}

static bool sample_control(FILE *output, FilmCamera *camera, FilmImage *raw, const char *name,
                           FilmYuv matrix, bool matrix_known)
{
    enum { FRAMES = 10 };
    FilmCameraStability window = {0};
    for (unsigned i = 0; i < FRAMES; ++i) {
        FilmCaptureTiming timing = {0};
        FilmStatus status = film_camera_receive(camera, raw, i + 1 < FRAMES, &timing);
        if (status != FILM_OK) {
            fprintf(output, "ERROR sensor=%u case=%s frame=%u capture=%s result=%08" PRIx32 "\n",
                    camera->sensor, name, i, film_status_name(status), camera->last_result);
            return camera->initialized && fflush(output) == 0;
        }
        film_camera_stability_push(&window,
            measure_color(output, raw, camera->sensor, name, matrix, matrix_known, timing.total_us, i));
    }
    FilmCameraStabilityResult stable = film_camera_stability_read(&window);
    fprintf(output, "STABILITY sensor=%u case=%s zoom=%u frames=%u window=%u "
            "y_span=%u cb_span=%.3f cr_span=%.3f ae_stable=%u awb_stable=%u\n",
            camera->sensor, name, camera->zoom, FRAMES, FILM_STABILITY_WINDOW,
            stable.y_span, stable.cb_span, stable.cr_span, stable.exposure, stable.white_balance);
    return fflush(output) == 0;
}

static bool probe_result(FILE *out, const FilmCamera *camera, const char *name, const char *operation, Result rc)
{
    fprintf(out, "CAM_CALL sensor=%u case=%s zoom=%u operation=%s result=%08" PRIx32 "\n",
            camera->sensor, name, camera->zoom, operation, (uint32_t)rc);
    return R_SUCCEEDED(rc);
}

static bool verify_auto(FILE *out, const FilmCamera *camera, const char *name, bool ae, bool enabled)
{
    FilmCameraAutoState state = film_camera_auto_state(camera);
    fprintf(out, "CAM_AUTO sensor=%u case=%s ae=%u ae_result=%08" PRIx32
            " awb=%u awb_result=%08" PRIx32 "\n", camera->sensor, name,
            state.exposure, state.exposure_result, state.white_balance, state.white_balance_result);
    return ae ? R_SUCCEEDED((Result)state.exposure_result) && state.exposure == enabled
              : R_SUCCEEDED((Result)state.white_balance_result) && state.white_balance == enabled;
}

static void probe_software_meter(FILE *out, FilmCamera *camera, FilmImage *raw, FilmYuv matrix,
                                 FilmMeterMode mode, int ev_thirds)
{
    enum { FRAMES = 6, MAX_READINGS = 14 };
    const char *name = mode == FILM_METER_AVERAGE ? "sw_average" : "sw_center";
    FilmExposureMeter meter = {0};
    int step = 0, applied = FILM_EXPOSURE_MIN - 1;
    for (unsigned i = 0; i < MAX_READINGS && camera->initialized && !ferror(out); ++i) {
        if (step != applied) {
            FilmStatus status = film_camera_set_control(camera, FILM_CAM_EXPOSURE, step);
            if (status != FILM_OK) {
                fprintf(out, "ERROR sensor=%u case=%s step=%d set=%s result=%08" PRIx32 "\n",
                        camera->sensor, name, step, film_status_name(status), camera->last_result);
                return;
            }
            applied = step;
            svcSleepThread(INT64_C(350000000));
        }
        FilmCameraStability window = {0};
        FilmMeterReading reading = {0};
        for (unsigned frame = 0; frame < FRAMES; ++frame) {
            FilmStatus status = film_camera_receive(camera, raw, frame + 1 < FRAMES, NULL);
            if (status != FILM_OK) {
                fprintf(out, "ERROR sensor=%u case=%s step=%d capture=%s result=%08" PRIx32 "\n",
                        camera->sensor, name, step, film_status_name(status), camera->last_result);
                return;
            }
            reading = film_camera_sample_preview(&window, raw, matrix, FILM_4_3, mode);
        }
        FilmCameraStabilityResult stable = film_camera_stability_read(&window);
        int next = stable.metering ? film_camera_meter_step(&meter, step, reading, ev_thirds) : step;
        fprintf(out, "SW_METER sensor=%u case=%s zoom=%u ev_thirds=%d reading=%u step=%d next=%d "
                "luma=%.5f clip=%.5f stable=%u\n", camera->sensor, name, camera->zoom, ev_thirds, i,
                step, next, reading.luma, reading.highlights, stable.metering);
        if (fflush(out) != 0 || (stable.metering && next == step)) return;
        step = next;
    }
}

static bool restore_auto_wb(FILE *out, const FilmCamera *camera, u32 select)
{
    bool mode = probe_result(out, camera, "restore_awb", "SetWhiteBalance",
                             CAMU_SetWhiteBalance(select, WHITE_BALANCE_AUTO));
    bool enabled = probe_result(out, camera, "restore_awb", "SetAutoWhiteBalance",
                                CAMU_SetAutoWhiteBalance(select, true));
    return verify_auto(out, camera, "restore_awb", false, true) && mode && enabled;
}

static bool probe_fixed_wb(FILE *out, FilmCamera *camera, FilmImage *raw, FilmYuv matrix,
                           bool matrix_known, u32 select, bool without_base_up)
{
    static const CAMU_WhiteBalance modes[] = {WHITE_BALANCE_3200K, WHITE_BALANCE_5200K, WHITE_BALANCE_7000K};
    Result (*set_mode)(u32, CAMU_WhiteBalance) = without_base_up ?
        CAMU_SetWhiteBalanceWithoutBaseUp : CAMU_SetWhiteBalance;
    const char *operation = without_base_up ? "SetWhiteBalanceWithoutBaseUp" : "SetWhiteBalance";
    for (unsigned order = 0; order < 2 && !ferror(out); ++order) {
        for (unsigned i = 0; i < sizeof modes / sizeof *modes; ++i) {
            char name[32];
            snprintf(name, sizeof name, "wb_%s%s_%d", without_base_up ? "nobase_" : "",
                     order ? "auto_first" : "mode_first", modes[i]);
            bool auto_set, mode_set;
            if (order) {
                auto_set = probe_result(out, camera, name, "SetAutoWhiteBalance", CAMU_SetAutoWhiteBalance(select, false));
                mode_set = probe_result(out, camera, name, operation, set_mode(select, modes[i]));
            } else {
                mode_set = probe_result(out, camera, name, operation, set_mode(select, modes[i]));
                auto_set = probe_result(out, camera, name, "SetAutoWhiteBalance", CAMU_SetAutoWhiteBalance(select, false));
            }
            if (verify_auto(out, camera, name, false, false) && auto_set && mode_set)
                if (!sample_control(out, camera, raw, name, matrix, matrix_known)) return false;
            if (!restore_auto_wb(out, camera, select)) return false;
        }
    }
    return !ferror(out);
}

static void probe_camera_controls(FILE *out, FilmCamera *camera, FilmImage raw, FilmYuv matrix, bool matrix_known)
{
    static const u32 selects[] = {SELECT_OUT1, SELECT_OUT2, SELECT_IN1};
    u32 select = selects[camera->sensor - FILM_CAMERA_REAR1];
    if (!verify_auto(out, camera, "manual", true, false)) return;
    if (probe_result(out, camera, "manual_hold_set", "SetExposure", CAMU_SetExposure(select, 0))) {
        if (!sample_control(out, camera, &raw, "manual_hold_a", matrix, matrix_known) ||
            !sample_control(out, camera, &raw, "manual_hold_b", matrix, matrix_known)) return;
        if (!verify_auto(out, camera, "manual_hold_after", true, false)) return;
    }
    probe_software_meter(out, camera, &raw, matrix, FILM_METER_AVERAGE, 0);
    probe_software_meter(out, camera, &raw, matrix, FILM_METER_CENTER, -3);
    probe_software_meter(out, camera, &raw, matrix, FILM_METER_CENTER, 3);
    if (!camera->initialized || ferror(out)) return;
    probe_result(out, camera, "restore_exposure", "SetExposure", CAMU_SetExposure(select, 0));

    if (!restore_auto_wb(out, camera, select)) return;
    bool locked = probe_result(out, camera, "awb_lock", "SetAutoWhiteBalance", CAMU_SetAutoWhiteBalance(select, false));
    if (verify_auto(out, camera, "awb_lock", false, false) && locked)
        if (!sample_control(out, camera, &raw, "awb_lock", matrix, matrix_known)) return;
    if (!restore_auto_wb(out, camera, select) ||
        !probe_fixed_wb(out, camera, &raw, matrix, matrix_known, select, false) ||
        !probe_fixed_wb(out, camera, &raw, matrix, matrix_known, select, true)) return;

    for (int sharpness = FILM_SHARPNESS_MIN; sharpness <= FILM_SHARPNESS_MAX && !ferror(out); ++sharpness) {
        char name[32];
        snprintf(name, sizeof name, "sharpness_%+d", sharpness);
        FilmStatus status = film_camera_set_control(camera, FILM_CAM_SHARPNESS, sharpness);
        if (probe_result(out, camera, name, "SetSharpness", (Result)camera->last_result) && status == FILM_OK)
            if (!sample_control(out, camera, &raw, name, matrix, matrix_known)) return;
    }
    film_camera_set_control(camera, FILM_CAM_SHARPNESS, 0);
    probe_result(out, camera, "restore_sharpness", "SetSharpness", (Result)camera->last_result);
}

void film_hardware_qa_camera(FILE *output, bool send_frames)
{
    enum { WIDTH = 640, HEIGHT = 480, BYTES = WIDTH * HEIGHT * 2, SETTLE_FRAMES = 6 };
    FilmImage raw = {WIDTH, HEIGHT, FILM_YUYV, linearMemAlign(BYTES, 0x80)};
    if (!raw.pixels) {
        fputs("ERROR camera_dma_alloc\n", output);
        return;
    }
    for (FilmSensor sensor = FILM_CAMERA_REAR1; sensor <= FILM_CAMERA_FRONT && !ferror(output); ++sensor) {
        FilmCamera camera = {0};
        FilmStatus status = film_camera_open(&camera, raw.format, sensor);
        fprintf(output, "CAMERA sensor=%u open=%s result=%08" PRIx32 "\n",
                sensor, film_status_name(status), camera.last_result);
        if (status != FILM_OK) continue;
        FilmYuv matrix = FILM_601_FULL;
        bool matrix_known = film_camera_yuv_matrix(&camera, &matrix) == FILM_OK;
        film_camera_log_calibration(output);
        if (!verify_auto(output, &camera, "exposure_sweep_before", true, false)) {
            film_camera_close(&camera);
            continue;
        }
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0] && !ferror(output); ++i) {
            const CameraCase *test = &cases[i];
            FilmStatus set = film_camera_set_control(&camera, test->control, test->value);
            FilmCaptureTiming timing = {0};
            status = set;
            for (unsigned frame = 0; frame <= SETTLE_FRAMES && status == FILM_OK; ++frame)
                status = film_camera_receive(&camera, &raw, false, &timing);
            fprintf(output, "CONTROL sensor=%u case=%s requested=%d set=%s sample=%s result=%08" PRIx32 "\n",
                    sensor, test->name, test->value, film_status_name(set), film_status_name(status),
                    camera.last_result);
            if (status == FILM_OK) {
                measure_color(output, &raw, sensor, test->name, matrix, matrix_known, timing.total_us, SETTLE_FRAMES);
                if (send_frames && i == 0) {
                    fprintf(output, "FRAME sensor=%u format=YUYV matrix=%u width=%u height=%u bytes=%u\n",
                            sensor, matrix, WIDTH, HEIGHT, BYTES);
                    if (fwrite(raw.pixels, 1, BYTES, output) != BYTES || fputc('\n', output) == EOF) break;
                }
            }
            if (set == FILM_OK && test->value != test->restore) {
                FilmStatus restored = film_camera_set_control(&camera, test->control, test->restore);
                if (restored != FILM_OK) {
                    fprintf(output, "ERROR sensor=%u restore=%s result=%08" PRIx32 "\n",
                            sensor, test->name, camera.last_result);
                    break;
                }
            }
            fflush(output);
            if (!camera.initialized) break;
        }
        if (camera.initialized && !ferror(output)) {
            FilmFrame zoomed = {0};
            FilmStatus set = film_camera_set_zoom(&camera, 2);
            FilmStatus sample = set == FILM_OK ? film_camera_capture(&camera, &zoomed) : set;
            fprintf(output, "ZOOM sensor=%u set=%s sample=%s width=%u height=%u result=%08" PRIx32 "\n",
                    sensor, film_status_name(set), film_status_name(sample),
                    zoomed.source.width, zoomed.source.height, camera.last_result);
            film_frame_free(&zoomed);
            if (sample == FILM_OK) {
                FilmStatus restore = film_camera_set_zoom(&camera, 1);
                if (restore == FILM_OK) restore = film_camera_capture(&camera, &zoomed);
                fprintf(output, "ZOOM_RESTORE sensor=%u sample=%s width=%u height=%u result=%08" PRIx32 "\n",
                        sensor, film_status_name(restore), zoomed.source.width, zoomed.source.height, camera.last_result);
                film_frame_free(&zoomed);
            }
            fflush(output);
        }
        if (camera.initialized && !ferror(output))
            probe_camera_controls(output, &camera, raw, matrix, matrix_known);
        film_camera_close(&camera);
    }
    linearFree(raw.pixels);
}
