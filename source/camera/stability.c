#include "platform_camera.h"

#include <math.h>

void film_camera_stability_push(FilmCameraStability *window, FilmCameraColorStats sample)
{
    window->samples[window->next] = sample;
    window->next = (window->next + 1) % FILM_STABILITY_WINDOW;
    if (window->count < FILM_STABILITY_WINDOW) ++window->count;
}

static float sample_luma(const FilmYuvTransform *t, unsigned y, int cb, int cr)
{
    float code = (y - t->y_offset) * t->y_gain;
    FilmRgb rgb = {code + t->r_cr * cr, code + t->g_cb * cb + t->g_cr * cr, code + t->b_cb * cb};
    return fmaxf(0, film_srgb_linear_luma(rgb));
}

FilmMeterReading film_camera_sample_preview(FilmCameraStability *window, const FilmImage *image,
                                            FilmYuv yuv, FilmAspect aspect, FilmMeterMode mode)
{
    if (!image || !image->pixels || image->format != FILM_YUYV ||
        !film_image_bytes(image->width, image->height, image->format)) return (FilmMeterReading){0};
    FilmRect area;
    if (film_crop_rect(image->width, image->height, aspect, &area) != FILM_OK)
        return (FilmMeterReading){0};
    unsigned first_pair = (area.x + 1) / 2, end_pair = (area.x + area.width) / 2;
    if (end_pair <= first_pair) return (FilmMeterReading){0};
    unsigned pairs = end_pair - first_pair;
    unsigned step_x = pairs >= 16 ? pairs / 16 : 1;
    unsigned step_y = area.height >= 12 ? area.height / 12 : 1;
    unsigned hist[256] = {0}, count = 0, cb = 0, cr = 0;
    unsigned weights = 0, highlights = 0;
    float weighted_y = 0;
    bool limited = yuv == FILM_601_LIMITED || yuv == FILM_709_LIMITED;
    unsigned near_white = limited ? 231 : 250;
    FilmYuvTransform transform = film_yuv_transform(yuv, 0);
    for (unsigned y = step_y / 2; y < area.height; y += step_y) {
        const uint8_t *row = image->pixels + (size_t)(area.y + y) * image->width * 2;
        for (unsigned x = step_x / 2; x < pairs; x += step_x) {
            const uint8_t *p = row + 4 * (first_pair + x);
            ++hist[p[0]];
            ++hist[p[2]];
            cb += p[1];
            cr += p[3];
            ++count;
            // Center quarter of the area gets 4× weight. The rest still contributes.
            unsigned weight = mode == FILM_METER_CENTER &&
                x >= pairs / 4 && x < pairs - pairs / 4 &&
                y >= area.height / 4 && y < area.height - area.height / 4 ? 4 : 1;
            weights += 2 * weight;
            if (mode != FILM_METER_MANUAL) {
                int u = p[1] - 128, v = p[3] - 128;
                weighted_y += weight * (sample_luma(&transform, p[0], u, v) +
                                        sample_luma(&transform, p[2], u, v));
            }
            highlights += weight * ((p[0] >= near_white) + (p[2] >= near_white));
        }
    }
    unsigned median = 0, rank = count;
    while (hist[median] < rank) rank -= hist[median++];
    FilmMeterReading reading = {weighted_y / weights, highlights / (float)weights};
    film_camera_stability_push(window, (FilmCameraColorStats){median, cb / (double)count, cr / (double)count,
                                                           reading.luma});
    return reading;
}

FilmCameraStabilityResult film_camera_stability_read(const FilmCameraStability *window)
{
    FilmCameraStabilityResult result = {0};
    if (window->count < FILM_STABILITY_WINDOW) return result;
    FilmCameraColorStats low = window->samples[0], high = low;
    for (unsigned i = 1; i < FILM_STABILITY_WINDOW; ++i) {
        FilmCameraColorStats sample = window->samples[i];
        if (sample.median < low.median) low.median = sample.median;
        if (sample.median > high.median) high.median = sample.median;
        if (sample.cb < low.cb) low.cb = sample.cb;
        if (sample.cb > high.cb) high.cb = sample.cb;
        if (sample.cr < low.cr) low.cr = sample.cr;
        if (sample.cr > high.cr) high.cr = sample.cr;
        if (sample.metered_luma < low.metered_luma) low.metered_luma = sample.metered_luma;
        if (sample.metered_luma > high.metered_luma) high.metered_luma = sample.metered_luma;
    }
    result.ready = true;
    result.y_span = high.median - low.median;
    result.cb_span = high.cb - low.cb;
    result.cr_span = high.cr - low.cr;
    result.exposure = result.y_span <= 2;
    result.white_balance = result.cb_span <= 1 && result.cr_span <= 1;
    // Roughly 1/8 stop of drift over the same window used for auto exposure.
    result.metering = high.metered_luma <= 1.09f * fmaxf(.01f, low.metered_luma);
    return result;
}

float film_camera_meter_error_ev(FilmMeterReading sample, int ev_thirds)
{
    float target = .18f * exp2f(ev_thirds / 3.0f);
    return log2f(target / fmaxf(sample.luma, 1e-4f));
}

int film_camera_meter_step(FilmExposureMeter *meter, int step, FilmMeterReading sample, int ev_thirds)
{
    float error = film_camera_meter_error_ev(sample, ev_thirds);
    if (meter->hold) {
        if (fabsf(error - film_camera_meter_error_ev(meter->reference, ev_thirds)) < .5f &&
            fabsf(sample.highlights - meter->reference.highlights) < .04f) return step;
        *meter = (FilmExposureMeter){0};
    }
    int direction = error > FILM_METER_TOLERANCE_EV ? 1 : error < -FILM_METER_TOLERANCE_EV ? -1 : 0;
    int next = step + direction;
    if (!direction || next < FILM_EXPOSURE_MIN || next > FILM_EXPOSURE_MAX) {
        meter->has_previous = false;
        return step;
    }
    if (meter->has_previous && next == meter->previous_step) {
        meter->hold = true;
        float old_error = fabsf(film_camera_meter_error_ev(meter->reference, ev_thirds));
        float new_error = fabsf(error);
        if (old_error < new_error ||
            (old_error == new_error && meter->reference.highlights < sample.highlights)) return next;
        meter->reference = sample;
        return step;
    }
    meter->reference = sample;
    meter->previous_step = step;
    meter->has_previous = true;
    return next;
}
