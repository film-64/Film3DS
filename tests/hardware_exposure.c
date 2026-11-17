#include "hardware_exposure.h"
#include "platform_camera.h"
#include "ui.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Register device model, not a stub of the verifier: every read/write passes
 * through the real controller. Refresh models normalization only. */
typedef struct {
    uint16_t reg[65536];
    unsigned operations, fail_at, writes, isp_writes, refreshes, delays;
    bool stalled_refresh, dynamic_awb_low;
} Device;
static Device device;

static FilmStatus read_reg(void *ctx, bool mcu, uint16_t a, uint16_t *v)
{
    Device *d = ctx;
    assert(mcu == ((a & 0x8000) != 0 || a == 0x221f));
    if (++d->operations == d->fail_at) return FILM_DEVICE;
    if (a == 0x3000) return FILM_DEVICE; /* Not the CAM service chip-ID address. */
    *v = d->reg[a];
    return FILM_OK;
}
static FilmStatus write_reg(void *ctx, bool mcu, uint16_t a, uint16_t v)
{
    Device *d = ctx;
    assert(mcu == ((a & 0x8000) != 0 || a == 0x221f));
    if (++d->operations == d->fail_at) return FILM_DEVICE;
    ++d->writes;
    d->reg[a] = v;
    if (a == 0xa355 && d->dynamic_awb_low) d->reg[a] = (v & 96) | ((v + 1) & 31);
    if (a >= 0x32d4 && a <= 0x32da) ++d->isp_writes;
    if (a == 0xa103 && v == 5) {
        ++d->refreshes;
        if (!d->stalled_refresh) {
            d->reg[a] = 0;
            if (d->reg[0xa11f] == 1 && (d->reg[0xa355] & 96) == 32) {
                for (unsigned i = 0xa34e; i <= 0xa350; ++i) d->reg[i] = 128;
            }
        }
    }
    return FILM_OK;
}
static void delay(void *ctx, unsigned ms)
{
    assert(ms == 4 || ms == 10);
    ++((Device *)ctx)->delays;
}
static FilmHardwareIo reset_device(void)
{
    device = (Device){0};
    device.reg[0x0000] = 0x2280;
    device.reg[0x300a] = 704;
    device.reg[0x300c] = 900;
    device.reg[0x3014] = 20;
    device.reg[0x221f] = 128;
    device.reg[0xa104] = 3;
    device.reg[0xa102] = 2;
    device.reg[0xa355] = 64;
    device.reg[0x3012] = 100;
    for (unsigned a = 0x3056; a <= 0x305c; a += 2) device.reg[a] = 544;
    for (unsigned a = 0x32d4; a <= 0x32da; a += 2) device.reg[a] = 133;
    return (FilmHardwareIo){&device, read_reg, write_reg, delay};
}
static FilmHardwareExposure start(void)
{
    FilmHardwareExposure h = {0};
    assert(film_hardware_begin(&h, reset_device(), "film-camera-preview/v1") == FILM_OK);
    assert(!h.ready && h.active);
    return h;
}
static void settle(FilmHardwareExposure *h, unsigned frames)
{
    for (unsigned i = 0; i < frames; ++i) {
        assert(!h->ready);
        assert(film_hardware_frame(h) == FILM_OK);
    }
    assert(h->ready);
}
static void lifecycle(void)
{
    FilmHardwareExposure h = start();
    assert(film_hardware_step(&h, 1, "film-camera-exposure/v1") == FILM_STATE);
    for (unsigned i = 0; i < FILM_HW_NORMALIZE; ++i) {
        assert(device.isp_writes == 0);
        assert(film_hardware_frame(&h) == FILM_OK);
    }
    assert(device.isp_writes == 4 && device.refreshes == 1 && !h.ready);
    settle(&h, FILM_HW_GAIN_SETTLE);
    for (int step = FILM_HW_MIN; step <= FILM_HW_MAX; ++step) {
        bool gain = film_hw_core[step - FILM_HW_MIN] != film_hw_core[h.step - FILM_HW_MIN];
        assert(film_hardware_step(&h, step, "film-camera-exposure/v1") == FILM_OK);
        assert(device.reg[0x3012] == film_hw_coarse[step - FILM_HW_MIN]);
        assert(device.reg[0x3056] == film_hw_core[step - FILM_HW_MIN]);
        settle(&h, gain ? FILM_HW_GAIN_SETTLE : FILM_HW_COARSE_SETTLE);
    }
    assert(device.isp_writes == 4); /* No hidden correction writes. */
    assert(film_hardware_restore(&h) == FILM_OK);
    assert(device.reg[0x3012] == 100 && device.reg[0x3056] == 544);
    assert(device.reg[0xa102] == 2 && device.reg[0xa355] == 64);
    assert(device.reg[0x32d4] == 133 && !h.active && !h.ready);
}
static void fail_closed(void)
{
    FilmHardwareExposure h = {0};
    FilmHardwareIo io = reset_device();
    assert(!film_runtime_source_valid(NULL) && !film_runtime_source_valid("scheduler_decision:admitted"));
    assert(film_hardware_begin(&h, io, "startup_warmup") == FILM_INVALID);
    assert(film_hardware_begin(&h, io, "film-camera-capture/v1") == FILM_INVALID);
    assert(device.operations == 0);
    device.reg[0x0000] = 0xffff;
    assert(film_hardware_begin(&h, io, "film-camera-preview/v1") == FILM_STATE);
    assert(device.writes == 0);
    h = (FilmHardwareExposure){0}; io = reset_device();
    device.reg[0x300a] = 2114;
    assert(film_hardware_begin(&h, io, "film-camera-preview/v1") == FILM_STATE);
    assert(device.writes == 0 && device.delays == FILM_HW_PROFILE_POLLS);
    h = (FilmHardwareExposure){0}; io = reset_device(); device.stalled_refresh = true;
    assert(film_hardware_begin(&h, io, "film-camera-preview/v1") == FILM_STATE);
    assert(device.refreshes == 1 && h.failed && !h.ready);
    h = start(); settle(&h, FILM_HW_NORMALIZE + FILM_HW_GAIN_SETTLE);
    unsigned before = device.operations;
    assert(film_hardware_step(&h, 0, "scheduler_decision:admitted") == FILM_INVALID);
    assert(film_hardware_step(&h, -4, "film-camera-exposure/v1") == FILM_INVALID);
    assert(film_hardware_step(&h, 4, "film-camera-exposure/v1") == FILM_INVALID);
    assert(device.operations == before);
    unsigned writes = device.writes;
    device.reg[0x32d4] = 133;
    assert(film_hardware_frame(&h) == FILM_STATE && h.failed && !h.ready);
    assert(film_hardware_step(&h, 1, "film-camera-exposure/v1") == FILM_STATE);
    assert(device.writes == writes);
}
static void faults(void)
{
    FilmHardwareExposure reference = start();
    unsigned begin_operations = device.operations;
    assert(film_hardware_restore(&reference) == FILM_OK);
    for (unsigned i = 1; i <= begin_operations; ++i) {
        FilmHardwareExposure h = {0}; FilmHardwareIo io = reset_device(); device.fail_at = i;
        assert(film_hardware_begin(&h, io, "film-camera-preview/v1") == FILM_DEVICE);
        assert(h.failed && !h.ready);
        assert(film_hardware_frame(&h) == FILM_STATE);
        assert(film_hardware_restore(&h) == FILM_OK);
    }
    reference = start(); settle(&reference, FILM_HW_NORMALIZE + FILM_HW_GAIN_SETTLE);
    unsigned start_count = device.operations;
    assert(film_hardware_step(&reference, 3, "film-camera-exposure/v1") == FILM_OK);
    unsigned transition_operations = device.operations - start_count;
    for (unsigned i = 1; i <= transition_operations; ++i) {
        FilmHardwareExposure h = start(); settle(&h, FILM_HW_NORMALIZE + FILM_HW_GAIN_SETTLE);
        device.fail_at = device.operations + i;
        assert(film_hardware_step(&h, 3, "film-camera-exposure/v1") == FILM_DEVICE);
        assert(h.failed && !h.ready);
        assert(film_hardware_restore(&h) == FILM_OK);
    }
    reference = start();
    for (unsigned i = 1; i < FILM_HW_NORMALIZE; ++i) assert(film_hardware_frame(&reference) == FILM_OK);
    start_count = device.operations;
    assert(film_hardware_frame(&reference) == FILM_OK);
    unsigned handoff_operations = device.operations - start_count;
    for (unsigned n = 1; n <= handoff_operations; ++n) {
        FilmHardwareExposure state = start();
        for (unsigned i = 1; i < FILM_HW_NORMALIZE; ++i) assert(film_hardware_frame(&state) == FILM_OK);
        device.fail_at = device.operations + n;
        assert(film_hardware_frame(&state) == FILM_DEVICE);
        assert(state.failed && !state.ready);
        unsigned writes = device.writes;
        assert(film_hardware_frame(&state) == FILM_STATE && device.writes == writes);
        assert(film_hardware_restore(&state) == FILM_OK);
    }
    FilmHardwareExposure h = start();
    device.fail_at = device.operations + 1;
    assert(film_hardware_restore(&h) == FILM_DEVICE);
    assert(device.reg[0xa102] == 2 && device.reg[0x32d4] == 133); /* Remaining restore attempts run. */
}
static void original_handoff_contract(void)
{
    FilmHardwareExposure h = {0};
    FilmHardwareIo io = reset_device();
    device.dynamic_awb_low = true;
    device.reg[0x221f] = 192; /* Original rebuild pins this observed value. */
    device.reg[0x3012] = 600; /* Native snapshot is not a manual request. */
    assert(film_hardware_begin(&h, io, "film-camera-preview/v1") == FILM_OK);
    assert((device.reg[0xa355] & 96) == 32 && h.dgain == 192);
    assert(device.reg[0xa34e] == 128); /* Normalizes only with bit64 cleared. */
    settle(&h, FILM_HW_NORMALIZE + FILM_HW_GAIN_SETTLE);
    assert(film_hardware_verify(&h) == FILM_OK);
    device.reg[0x221f] = 193;
    unsigned writes = device.writes;
    assert(film_hardware_frame(&h) == FILM_STATE && h.failed && !h.ready);
    assert(device.writes == writes); /* Pin once, no rebasing or correction. */
    assert(film_hardware_restore(&h) == FILM_OK);
    assert(device.reg[0x3012] == 600 && (device.reg[0xa355] & 96) == 64);
}
static void darkroom(void)
{
    assert(film_ui_curve_adjust(-1, 1) == -.8f && film_ui_curve_adjust(0, 1) == .2f);
    assert(film_ui_curve_adjust(1, 1) == 1 && film_ui_curve_adjust(1, -1) == .8f);
    assert(film_ui_curve_adjust(0, -1) == -.2f && film_ui_curve_adjust(-1, -1) == -1);
    assert(film_ui_curve_adjust(.3f, -1) == .2f && film_ui_curve_adjust(-.3f, 1) == -.2f);
    FilmUi ui = film_ui_new(1);
    ui.recipe.grain_amount = 1; ui.recipe.base_saturation = 1;
    ui.recipe.tone.value[FILM_TONE_MID] = 1;
    ui.recipe.input_tone.value[FILM_TONE_HIGH] = .8f;
    ui.recipe.sharpness = 5;
    strcpy(ui.recipe.lut_id, "previous.cube");
    ui.recipe.profile.exposure = 2;
    FilmRecipe raw = film_ui_capture_recipe(&ui);
    assert(!raw.lut_id[0] && raw.grain_amount == 0 && raw.base_saturation == 0);
    assert(raw.profile.exposure == 0 && raw.tone.value[FILM_TONE_MID] == 0);
    assert(raw.input_tone.value[FILM_TONE_HIGH] == 0 && raw.sharpness == 0);
    for (int l = -1; l <= 1; ++l) for (int s = -1; s <= 1; ++s) for (int m = -1; m <= 1; ++m) {
        FilmFrame frame = {.recipe = film_default_recipe()};
        frame.recipe.tone.value[FILM_TONE_SHADOWS] = l;
        frame.recipe.tone.value[FILM_TONE_HIGH] = s;
        frame.recipe.tone.value[FILM_TONE_MID] = m;
        FilmDevelopGrade grade = film_develop_grade_prepare(NULL, &frame.recipe);
        float previous = -1;
        for (unsigned n = 0; n <= 1000; ++n) {
            float v = n / 1000.f;
            FilmRgb out = film_develop_grade_pixel((FilmRgb){v, v, v}, &grade);
            assert(isfinite(out.r) && out.r >= previous - 1e-6f && out.r >= 0 && out.r <= 1);
            previous = out.r;
        }
        assert(film_image_alloc(&frame.source, 4, 3, FILM_YUYV) == FILM_OK);
        memset(frame.source.pixels, 128, 24);
        const char *path = "build/hardware-recipe.f3s";
        assert(film_frame_write(path, &frame) == FILM_OK);
        FilmFrame read = {0};
        assert(film_frame_read(path, &read) == FILM_OK);
        assert(!memcmp(&read.recipe.tone, &frame.recipe.tone, sizeof(FilmTone)));
        assert(!memcmp(read.source.pixels, frame.source.pixels, 24));
        assert(unlink(path) == 0);
        film_frame_free(&read); film_frame_free(&frame);
    }
}
int main(void)
{
    lifecycle(); fail_closed(); faults(); original_handoff_contract(); darkroom();
    puts("PASS HW15 source gates, fixed handoff, seven steps, drift, RPC faults, restore, 27 curves and F3S");
    return 0;
}
