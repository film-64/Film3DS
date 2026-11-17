#include "hardware_exposure.h"

static const uint16_t saved_address[] = {
    0xa102, 0xa355, 0xa11f, 0x3012,
    0x3056, 0x3058, 0x305a, 0x305c, 0x32d4, 0x32d6, 0x32d8, 0x32da
};

static bool mcu(uint16_t a) { return (a & 0x8000) || a == 0x221f; }
static FilmStatus read_value(FilmHardwareExposure *h, uint16_t a, uint16_t *v)
{
    return h->io.read(h->io.context, mcu(a), a, v);
}
static FilmStatus expect(FilmHardwareExposure *h, uint16_t a, uint16_t target)
{
    uint16_t actual;
    FilmStatus s = read_value(h, a, &actual);
    return s != FILM_OK ? s : actual == target ? FILM_OK : FILM_STATE;
}
static FilmStatus write_value(FilmHardwareExposure *h, uint16_t a, uint16_t v)
{
    return h->io.write(h->io.context, mcu(a), a, v);
}
static FilmStatus set(FilmHardwareExposure *h, uint16_t a, uint16_t v)
{
    FilmStatus s = write_value(h, a, v);
    if (s == FILM_OK && a == 0xa355) {
        uint16_t actual;
        s = read_value(h, a, &actual);
        if (s == FILM_OK && (actual & FILM_HW_AWB_MASK) != (v & FILM_HW_AWB_MASK)) s = FILM_STATE;
        return s;
    }
    return s == FILM_OK ? expect(h, a, v) : s;
}
static FilmStatus reject(FilmHardwareExposure *h, FilmStatus s)
{
    h->failed = true;
    h->ready = false;
    return s;
}

static FilmStatus refresh(FilmHardwareExposure *h, bool restoring)
{
    uint16_t before_command, before_state;
    FilmStatus s = read_value(h, 0xa103, &before_command);
    if (s != FILM_OK) return s;
    s = read_value(h, 0xa104, &before_state);
    if (s != FILM_OK) return s;
    if (before_command || (before_state != 3 && before_state != 7)) return FILM_STATE;
    s = write_value(h, 0xa103, 5);
    if (s != FILM_OK) return s;
    unsigned stable = 0;
    for (unsigned i = 0; i < FILM_HW_REFRESH_POLLS; ++i) {
        uint16_t cmd, state, seq, awb, preview;
        if ((s = read_value(h, 0xa103, &cmd)) != FILM_OK ||
            (s = read_value(h, 0xa104, &state)) != FILM_OK ||
            (s = read_value(h, 0xa102, &seq)) != FILM_OK ||
            (s = read_value(h, 0xa355, &awb)) != FILM_OK ||
            (s = read_value(h, 0xa11f, &preview)) != FILM_OK) return s;
        bool match = !cmd && state == before_state && seq == (restoring ? h->saved[0] : ((h->saved[0] & ~5u) | 4u)) &&
            (awb & FILM_HW_AWB_MASK) == ((restoring ? h->saved[1] : h->awb) & FILM_HW_AWB_MASK) &&
            preview == (restoring ? h->saved[2] : 1);
        stable = match ? stable + 1 : 0;
        if (stable == FILM_HW_REFRESH_STABLE) return FILM_OK;
        h->io.delay(h->io.context, 4);
    }
    return FILM_STATE;
}

FilmStatus film_hardware_begin(FilmHardwareExposure *h, FilmHardwareIo io, const char *source)
{
    if (!h || !io.read || !io.write || !io.delay || !source ||
        (strcmp(source, "film-camera-preview/v1") && strcmp(source, "film-camera-manual-setup/v1")) ||
        !film_runtime_source_valid(source)) return FILM_INVALID;
    if (h->active || h->saved_valid) return FILM_STATE;
    *h = (FilmHardwareExposure){.io = io};
    FilmStatus s;
    if ((s = expect(h, FILM_HW_CHIP_ADDRESS, FILM_HW_CHIP)) != FILM_OK) return reject(h, s);
    unsigned stable = 0;
    for (unsigned i = 0; i < FILM_HW_PROFILE_POLLS; ++i) {
        uint16_t lines, cmd, state;
        if ((s = read_value(h, 0x300a, &lines)) != FILM_OK ||
            (s = read_value(h, 0xa103, &cmd)) != FILM_OK ||
            (s = read_value(h, 0xa104, &state)) != FILM_OK) return reject(h, s);
        stable = lines == FILM_HW_LINES && !cmd && state == 3 ? stable + 1 : 0;
        if (stable == 2) break;
        io.delay(io.context, 10);
    }
    if (stable != 2) return reject(h, FILM_STATE);
    for (unsigned i = 0; i < 12; ++i)
        if ((s = read_value(h, saved_address[i], &h->saved[i])) != FILM_OK) return reject(h, s);
    if (h->saved[0] > 255 || h->saved[1] > 255 || h->saved[2] > 1) return reject(h, FILM_STATE);
    for (unsigned i = 4; i < 8; ++i)
        if (h->saved[i] & 0xf000) return reject(h, FILM_STATE);
    for (unsigned i = 8; i < 12; ++i)
        if (h->saved[i] < 1 || h->saved[i] > 512) return reject(h, FILM_STATE);
    h->saved_valid = true;
    h->seq = h->saved[0] & ~15u;
    h->awb = (h->saved[1] | 32u) & ~64u;
    /* Same order as exposure_rebuild rb_normalization -> prepare_arm:
     * force unity AND clear bit64 before refresh; verify only control bits. */
    if ((s = set(h, 0xa355, h->awb)) != FILM_OK ||
        (s = set(h, 0xa102, (h->saved[0] & ~5u) | 4u)) != FILM_OK ||
        (s = set(h, 0xa11f, 1)) != FILM_OK ||
        (s = refresh(h, false)) != FILM_OK) return reject(h, s);
    for (unsigned a = 0xa34e; a <= 0xa350; ++a)
        if ((s = expect(h, a, 128)) != FILM_OK) return reject(h, s);
    if ((s = set(h, 0xa11f, 0)) != FILM_OK ||
        (s = set(h, 0xa102, h->saved[0] & ~5u)) != FILM_OK ||
        (s = set(h, 0xa102, h->seq)) != FILM_OK ||
        (s = set(h, 0x3012, film_hw_coarse[3])) != FILM_OK) return reject(h, s);
    for (unsigned a = 0x3056; a <= 0x305c; a += 2)
        if ((s = set(h, a, film_hw_core[3])) != FILM_OK) return reject(h, s);
    /* Rebuild pins the observed timing/digital gain after manual setup. It
     * never assumes 221f is128 and never accepts later drift as a new baseline. */
    if ((s = read_value(h, 0x300c, &h->line)) != FILM_OK || !h->line ||
        (s = read_value(h, 0x3014, &h->fine)) != FILM_OK ||
        (s = read_value(h, 0x221f, &h->dgain)) != FILM_OK)
        return reject(h, s == FILM_OK ? FILM_STATE : s);
    if (h->dgain < 1 || h->dgain > 512) return reject(h, FILM_STATE);
    /* Fixed FD-off handoff: observe eight frames, then one ISP write and
     * eight strict frames. Never rewrite in response to drift. */
    h->active = true;
    h->remaining = FILM_HW_NORMALIZE;
    return FILM_OK;
}

FilmStatus film_hardware_verify(FilmHardwareExposure *h)
{
    if (!h || !h->active || h->failed) return FILM_STATE;
    FilmStatus s;
    const uint16_t addresses[] = {0xa102, 0xa103, 0xa104, 0xa11d, 0xa11f, 0xa34e,
        0xa34f, 0xa350, 0x221f, 0x300a, 0x300c, 0x3014, 0x3012};
    const uint16_t values[] = {h->seq, 0, 3, 0, 0, 128, 128, 128, h->dgain, FILM_HW_LINES,
        h->line, h->fine, film_hw_coarse[h->step - FILM_HW_MIN]};
    for (unsigned i = 0; i < sizeof addresses / sizeof *addresses; ++i)
        if ((s = expect(h, addresses[i], values[i])) != FILM_OK) return reject(h, s);
    uint16_t awb;
    if ((s = read_value(h, 0xa355, &awb)) != FILM_OK) return reject(h, s);
    if ((awb & FILM_HW_AWB_MASK) != FILM_HW_AWB_TARGET) return reject(h, FILM_STATE);
    for (unsigned a = 0x3056; a <= 0x305c; a += 2)
        if ((s = expect(h, a, film_hw_core[h->step - FILM_HW_MIN])) != FILM_OK) return reject(h, s);
    for (unsigned a = 0x32d4; a <= 0x32da; a += 2) {
        uint16_t v;
        if ((s = read_value(h, a, &v)) != FILM_OK) return reject(h, s);
        if (v < 1 || v > 512 || (h->phase && v != FILM_HW_ISP)) return reject(h, FILM_STATE);
    }
    return FILM_OK;
}

FilmStatus film_hardware_frame(FilmHardwareExposure *h)
{
    FilmStatus s = film_hardware_verify(h);
    if (s != FILM_OK) return s;
    if (h->remaining) --h->remaining;
    if (!h->remaining && !h->phase) {
        for (unsigned a = 0x32d4; a <= 0x32da; a += 2)
            if ((s = set(h, a, FILM_HW_ISP)) != FILM_OK) return reject(h, s);
        h->phase = 1;
        h->remaining = FILM_HW_GAIN_SETTLE;
    }
    h->ready = h->phase && !h->remaining;
    return FILM_OK;
}

FilmStatus film_hardware_step(FilmHardwareExposure *h, int step, const char *source)
{
    if (!h || !source || strcmp(source, "film-camera-exposure/v1") ||
        !film_runtime_source_valid(source) || step < FILM_HW_MIN || step > FILM_HW_MAX) return FILM_INVALID;
    if (!h->ready || h->failed) return FILM_STATE;
    FilmStatus s = film_hardware_verify(h);
    if (s != FILM_OK || step == h->step) return s;
    unsigned index = step - FILM_HW_MIN;
    bool gain = film_hw_core[index] != film_hw_core[h->step - FILM_HW_MIN];
    h->ready = false;
    if ((s = set(h, 0x3012, film_hw_coarse[index])) != FILM_OK) return reject(h, s);
    if (gain) for (unsigned a = 0x3056; a <= 0x305c; a += 2)
        if ((s = set(h, a, film_hw_core[index])) != FILM_OK) return reject(h, s);
    h->step = step;
    h->remaining = gain ? FILM_HW_GAIN_SETTLE : FILM_HW_COARSE_SETTLE;
    return FILM_OK;
}

FilmStatus film_hardware_restore(FilmHardwareExposure *h)
{
    if (!h || !h->saved_valid) return FILM_OK;
    h->ready = h->active = false;
    FilmStatus result = FILM_OK;
    /* Restore controls first while drivers are off; then restore the MCU
     * configuration with one documented refresh. Attempt every saved write. */
    for (unsigned i = 3; i < 12; ++i) {
        FilmStatus s = set(h, saved_address[i], h->saved[i]);
        if (s != FILM_OK) result = s;
    }
    for (unsigned i = 0; i < 3; ++i) {
        FilmStatus s = set(h, saved_address[i], h->saved[i]);
        if (s != FILM_OK) result = s;
    }
    FilmStatus s = refresh(h, true);
    if (s != FILM_OK) result = s;
    h->saved_valid = false;
    return result;
}
