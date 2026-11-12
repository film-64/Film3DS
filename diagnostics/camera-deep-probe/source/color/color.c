#include "film.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float clamp01(float value)
{
    return fminf(fmaxf(value, 0), 1);
}

static char *skip_space(char *line)
{
    while (isspace((unsigned char)*line)) ++line;
    return line;
}

static bool values(char *line, float *out, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        char *end;
        const char *start = line + (*line == '+' || *line == '-');
        if ((!isdigit((unsigned char)*start) && *start != '.') ||
            (*start == '0' && (start[1] == 'x' || start[1] == 'X'))) return false;
        errno = 0;
        out[i] = strtof(line, &end);
        if (end == line || errno == ERANGE || !isfinite(out[i]) ||
            (*end && !isspace((unsigned char)*end))) return false;
        line = skip_space(end);
    }
    return *line == '\0';
}

static bool directive(char **line, const char *keyword)
{
    size_t length = strlen(keyword);
    if (strncmp(*line, keyword, length) ||
        ((*line)[length] && !isspace((unsigned char)(*line)[length]))) return false;
    *line = skip_space(*line + length);
    return true;
}

FilmStatus film_camera_color_load(FILE *file, FilmCameraColor *out)
{
    if (!file || !out) return FILM_INVALID;
    FilmCameraColor color;
    char row[160];
    for (unsigned i = 0; i < 3; ++i) {
        if (!fgets(row, sizeof row, file)) return ferror(file) ? FILM_IO : FILM_CORRUPT;
        if (!strchr(row, '\n') && !feof(file)) return FILM_CORRUPT;
        if (!values(skip_space(row), color.rows[i], 3)) return FILM_CORRUPT;
        for (unsigned c = 0; c < 3; ++c) if (fabsf(color.rows[i][c]) > 4) return FILM_CORRUPT;
    }
    int ch;
    do ch = fgetc(file); while (ch != EOF && isspace((unsigned char)ch));
    if (ch != EOF || ferror(file)) return ferror(file) ? FILM_IO : FILM_CORRUPT;
    *out = color;
    return FILM_OK;
}

FilmStatus film_lut_load(FILE *file, FilmLut *out, unsigned *error_line)
{
    if (error_line) *error_line = 0;
    if (!file || !out) return FILM_INVALID;
    FilmLut lut = { .max = {1, 1, 1} };
    unsigned line_number = 0;
    size_t count = 0, expected = 0;
    bool have_title = false, have_min = false, have_max = false, have_range = false;
    FilmStatus status = FILM_CORRUPT;
    char buffer[1024];
    for (;;) {
        size_t length = 0;
        int ch;
        while ((ch = fgetc(file)) != EOF && ch != '\n') {
            if (!ch || length == sizeof buffer - 1) { ++line_number; goto fail; }
            buffer[length++] = (char)ch;
        }
        if (ch == EOF && !length) break;
        buffer[length] = '\0';
        ++line_number;
        char *line = skip_space(buffer);
        if (!count && !strcmp(line, "# FILM3DS_TRILINEAR")) lut.trilinear = true;
        if (!count && !strcmp(line, "# FILM3DS_SHAPER_DARK_1_5")) lut.shadow_shaper = true;
        bool quoted = false;
        for (char *c = line; *c; ++c) {
            if (*c == '"') quoted = !quoted;
            if (*c == '#' && !quoted) { *c = '\0'; break; }
        }
        if (quoted) goto fail;
        if (!*line) continue;

        if (isalpha((unsigned char)*line) || *line == '_') {
            if (count) goto fail;
            if (directive(&line, "TITLE")) {
                if (have_title || *line++ != '"') goto fail;
                char *end = strchr(line, '"');
                if (!end || *skip_space(end + 1)) goto fail;
                have_title = true;
            } else if (directive(&line, "LUT_3D_SIZE")) {
                char *end;
                if (!isdigit((unsigned char)*line)) goto fail;
                errno = 0;
                unsigned long n = strtoul(line, &end, 10);
                if (lut.size || errno == ERANGE || *skip_space(end)) goto fail;
                if (n != 17 && n != 33 && n != 65) {
                    status = FILM_UNSUPPORTED;
                    goto fail;
                }
                lut.size = (unsigned)n;
                expected = n * n * n;
                lut.samples = malloc(expected * sizeof *lut.samples);
                if (!lut.samples) { status = FILM_NOMEM; goto fail; }
            } else if (directive(&line, "DOMAIN_MIN")) {
                float v[3];
                if (have_min || have_range || !values(line, v, 3)) goto fail;
                lut.min = (FilmRgb){v[0], v[1], v[2]};
                have_min = true;
            } else if (directive(&line, "DOMAIN_MAX")) {
                float v[3];
                if (have_max || have_range || !values(line, v, 3)) goto fail;
                lut.max = (FilmRgb){v[0], v[1], v[2]};
                have_max = true;
            } else if (directive(&line, "LUT_3D_INPUT_RANGE")) {
                float v[2];
                if (have_range || have_min || have_max || !values(line, v, 2)) goto fail;
                lut.min = (FilmRgb){v[0], v[0], v[0]};
                lut.max = (FilmRgb){v[1], v[1], v[1]};
                have_range = true;
            } else {
                status = FILM_UNSUPPORTED;
                goto fail;
            }
        } else {
            float v[3];
            if (count == expected || !values(line, v, 3)) goto fail;
            lut.samples[count++] = (FilmRgb){v[0], v[1], v[2]};
        }
    }
    if (ferror(file)) { status = FILM_IO; goto fail; }
    if (!expected || count != expected || lut.min.r >= lut.max.r ||
        lut.min.g >= lut.max.g || lut.min.b >= lut.max.b ||
        !isfinite(lut.max.r - lut.min.r) || !isfinite(lut.max.g - lut.min.g) ||
        !isfinite(lut.max.b - lut.min.b)) goto fail;
    *out = lut;
    return FILM_OK;
fail:
    free(lut.samples);
    if (error_line) *error_line = line_number;
    return status;
}

void film_lut_free(FilmLut *lut)
{
    free(lut->samples);
    *lut = (FilmLut){0};
}

FilmRgb film_lut_sample(const FilmLut *lut, FilmRgb color)
{
    const float in[] = {color.r, color.g, color.b};
    const float low[] = {lut->min.r, lut->min.g, lut->min.b};
    const float high[] = {lut->max.r, lut->max.g, lut->max.b};
    unsigned n = lut->size, base = 0, stride[] = {1, n, n * n};
    unsigned order[] = {0, 1, 2};
    float fraction[3];
    for (unsigned i = 0; i < 3; ++i) {
        float position = clamp01((in[i] - low[i]) / (high[i] - low[i]));
        if (lut->shadow_shaper) position = powf(position, 2.0f / 3.0f);
        float cell = position * (n - 1);
        unsigned index = (unsigned)cell;
        if (index == n - 1) --index;
        base += index * stride[i];
        fraction[i] = cell - index;
    }
    FilmRgb result = {0, 0, 0};
    if (lut->trilinear) {
        for (unsigned corner = 0; corner < 8; ++corner) {
            float weight = 1;
            unsigned index = base;
            for (unsigned i = 0; i < 3; ++i) {
                bool upper = (corner >> i) & 1;
                if (upper) index += stride[i];
                weight *= upper ? fraction[i] : 1 - fraction[i];
            }
            FilmRgb vertex = lut->samples[index];
            result.r += weight * vertex.r;
            result.g += weight * vertex.g;
            result.b += weight * vertex.b;
        }
        return result;
    }
    for (unsigned i = 1; i < 3; ++i) {
        for (unsigned j = i; j && fraction[order[j]] > fraction[order[j - 1]]; --j) {
            unsigned swap = order[j];
            order[j] = order[j - 1];
            order[j - 1] = swap;
        }
    }
    // Walk the tetrahedron from its lowest to highest corner in fraction order.
    float previous = 1;
    for (unsigned i = 0; i < 4; ++i) {
        float next = i < 3 ? fraction[order[i]] : 0;
        float weight = previous - next;
        FilmRgb vertex = lut->samples[base];
        result.r += weight * vertex.r;
        result.g += weight * vertex.g;
        result.b += weight * vertex.b;
        if (i < 3) base += stride[order[i]];
        previous = next;
    }
    return result;
}

static float decode_srgb(float v)
{
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static float encode_srgb(float v)
{
    return v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

float film_srgb_linear_luma(FilmRgb code)
{
    return .2126f * decode_srgb(code.r) + .7152f * decode_srgb(code.g) + .0722f * decode_srgb(code.b);
}

FilmRgb film_srgb_add_halation(FilmRgb code, float energy)
{
    if (energy <= 0) return code;
    return (FilmRgb){encode_srgb(decode_srgb(code.r) + energy),
                     encode_srgb(decode_srgb(code.g) + energy * .27f),
                     encode_srgb(decode_srgb(code.b) + energy * .06f)};
}

static float to_lut(float value, FilmTransfer transfer)
{
    if (transfer == FILM_SRGB) return value;
    float linear = decode_srgb(value);
    if (transfer == FILM_REC709)
        return linear < 0.018f ? 4.5f * linear : 1.099f * powf(linear, 0.45f) - 0.099f;
    return linear;
}

static float encode_flog2c(float linear)
{
    if (linear < 0.000889f) return 8.799461f * linear + 0.092864f;
    return 0.245281f * log10f(5.555556f * linear + 0.064829f) + 0.384316f;
}

static FilmRgb lut_input(FilmRgb color, FilmTransfer transfer)
{
    if (transfer != FILM_FLOG2C)
        return (FilmRgb){to_lut(color.r, transfer), to_lut(color.g, transfer), to_lut(color.b, transfer)};

    // Linear BT.709 to F-Gamut C, derived from Fujifilm's published D65 primaries.
    float r = decode_srgb(color.r), g = decode_srgb(color.g), b = decode_srgb(color.b);
    return (FilmRgb){encode_flog2c(.517069024f * r + .412934676f * g + .069996300f * b),
                     encode_flog2c(.088617162f * r + .809263150f * g + .102119689f * b),
                     encode_flog2c(.017750040f * r + .109447621f * g + .872802339f * b)};
}

static float from_lut(float value, FilmTransfer transfer)
{
    if (transfer == FILM_SRGB) return value;
    if (transfer == FILM_REC709)
        value = value < 0.081f ? value / 4.5f : powf((value + 0.099f) / 1.099f, 1 / 0.45f);
    return encode_srgb(value);
}

static bool valid_hsl(FilmHsl a)
{
    return isfinite(a.hue) && fabsf(a.hue) <= 180 &&
           isfinite(a.saturation) && fabsf(a.saturation) <= 1 &&
           isfinite(a.lightness) && fabsf(a.lightness) <= 1;
}

FilmStatus film_profile_validate(const FilmProfile *p)
{
    if (!p || (unsigned)p->input > FILM_FLOG2C || (unsigned)p->output > FILM_REC709 ||
        !isfinite(p->strength) || p->strength < 0 || p->strength > 1 ||
        !isfinite(p->exposure) || fabsf(p->exposure) > 8 || !valid_hsl(p->global)) return FILM_INVALID;
    for (unsigned i = 0; i < 12; ++i) if (!valid_hsl(p->zones[i])) return FILM_INVALID;
    if (p->input == FILM_VENDOR_LOG || p->output == FILM_VENDOR_LOG) return FILM_UNSUPPORTED;
    return FILM_OK;
}

static FilmRgb adjust_hsl(FilmRgb c, const FilmProfile *p)
{
    float max = fmaxf(c.r, fmaxf(c.g, c.b));
    float min = fminf(c.r, fminf(c.g, c.b));
    float delta = max - min, lightness = (max + min) * 0.5f;
    float hue = 0, saturation = 0;
    FilmHsl adjustment = p->global;
    if (delta > 1e-6f) {
        saturation = delta / (1 - fabsf(2 * lightness - 1));
        if (max == c.r) hue = (c.g - c.b) / delta;
        else if (max == c.g) hue = 2 + (c.b - c.r) / delta;
        else hue = 4 + (c.r - c.g) / delta;
        if (hue < 0) hue += 6;
        float zone = hue * 2;
        unsigned a = (unsigned)zone % 12, b = (a + 1) % 12;
        float t = zone - floorf(zone);
        adjustment.hue += (1 - t) * p->zones[a].hue + t * p->zones[b].hue;
        adjustment.saturation += (1 - t) * p->zones[a].saturation + t * p->zones[b].saturation;
        adjustment.lightness += (1 - t) * p->zones[a].lightness + t * p->zones[b].lightness;
    }
    if (!adjustment.hue && !adjustment.saturation && !adjustment.lightness) return c;
    lightness = clamp01(lightness + adjustment.lightness);
    if (delta <= 1e-6f) return (FilmRgb){lightness, lightness, lightness};
    saturation = clamp01(saturation * (1 + adjustment.saturation));
    hue = fmodf(hue + adjustment.hue / 60 + 6, 6);
    float chroma = (1 - fabsf(2 * lightness - 1)) * saturation;
    float secondary = chroma * (1 - fabsf(fmodf(hue, 2) - 1));
    float match = lightness - chroma * 0.5f;
    const FilmRgb sectors[] = {{chroma, secondary, 0}, {secondary, chroma, 0},
        {0, chroma, secondary}, {0, secondary, chroma}, {secondary, 0, chroma}, {chroma, 0, secondary}};
    FilmRgb rgb = sectors[(unsigned)hue];
    return (FilmRgb){rgb.r + match, rgb.g + match, rgb.b + match};
}

FilmGrade film_grade_prepare(const FilmLut *lut, const FilmProfile *p)
{
    FilmGrade grade = {p->strength ? lut : NULL, p, p->exposure ? exp2f(p->exposure) : 1,
                       p->global.hue || p->global.saturation || p->global.lightness};
    for (unsigned i = 0; i < 12 && !grade.has_hsl; ++i) {
        FilmHsl zone = p->zones[i];
        grade.has_hsl = zone.hue || zone.saturation || zone.lightness;
    }
    return grade;
}

FilmRgb film_camera_color_apply(FilmRgb code, const FilmCameraColor *color)
{
    if (!color) return code;
    const float rgb[] = {decode_srgb(code.r), decode_srgb(code.g), decode_srgb(code.b)};
    float mapped[3];
    for (unsigned i = 0; i < 3; ++i)
        mapped[i] = encode_srgb(color->rows[i][0] * rgb[0] +
                                color->rows[i][1] * rgb[1] + color->rows[i][2] * rgb[2]);
    return (FilmRgb){mapped[0], mapped[1], mapped[2]};
}

static FilmRgb sample_lut_extended(const FilmLut *lut, FilmRgb color)
{
    float input[] = {color.r, color.g, color.b};
    float low[] = {lut->min.r, lut->min.g, lut->min.b};
    float high[] = {lut->max.r, lut->max.g, lut->max.b};
    float boundary[3];
    for (unsigned i = 0; i < 3; ++i)
        boundary[i] = fminf(high[i], fmaxf(low[i], input[i]));
    FilmRgb base = film_lut_sample(lut, (FilmRgb){boundary[0], boundary[1], boundary[2]});
    FilmRgb result = base;
    // Use the adjacent cell to extend the edge in the same encoding.
    for (unsigned i = 0; i < 3; ++i) {
        if (input[i] == boundary[i]) continue;
        float inside[3] = {boundary[0], boundary[1], boundary[2]};
        float cell = (high[i] - low[i]) / (lut->size - 1);
        inside[i] += input[i] < low[i] ? cell : -cell;
        float step = inside[i] - boundary[i];
        if (!step) continue;
        FilmRgb next = film_lut_sample(lut, (FilmRgb){inside[0], inside[1], inside[2]});
        float ratio = (input[i] - boundary[i]) / step;
        result.r += ratio * (next.r - base.r);
        result.g += ratio * (next.g - base.g);
        result.b += ratio * (next.b - base.b);
    }
    return result;
}

static FilmRgb apply_lut(FilmRgb color, const FilmGrade *grade, bool extended)
{
    const FilmProfile *p = grade->profile;
    if (p->exposure) {
        float gain = grade->exposure_gain;
        color = (FilmRgb){encode_srgb(decode_srgb(color.r) * gain),
                         encode_srgb(decode_srgb(color.g) * gain),
                         encode_srgb(decode_srgb(color.b) * gain)};
    }
    if (grade->lut) {
        FilmRgb input = lut_input(color, p->input);
        const FilmLut *lut = grade->lut;
        bool outside = input.r < lut->min.r || input.r > lut->max.r ||
                       input.g < lut->min.g || input.g > lut->max.g ||
                       input.b < lut->min.b || input.b > lut->max.b;
        FilmRgb mapped = extended && outside && p->input == p->output && !lut->shadow_shaper
            ? sample_lut_extended(lut, input) : film_lut_sample(lut, input);
        mapped = (FilmRgb){from_lut(mapped.r, p->output), from_lut(mapped.g, p->output),
                          from_lut(mapped.b, p->output)};
        color = (FilmRgb){color.r * (1 - p->strength) + mapped.r * p->strength,
                          color.g * (1 - p->strength) + mapped.g * p->strength,
                          color.b * (1 - p->strength) + mapped.b * p->strength};
    }
    return color;
}

FilmRgb film_grade_pixel(FilmRgb color, const FilmGrade *grade)
{
    color = apply_lut(color, grade, false);
    color = (FilmRgb){clamp01(color.r), clamp01(color.g), clamp01(color.b)};
    return grade->has_hsl ? adjust_hsl(color, grade->profile) : color;
}

float film_tone_limit(FilmToneControl control, float value)
{
    float max = control == FILM_TONE_EV ? 4 : 1;
    float min = control == FILM_TONE_SHOULDER ? 0 : -max;
    return fmaxf(min, fminf(value, max));
}

FilmDevelopGrade film_develop_grade_prepare(const FilmLut *lut, const FilmRecipe *recipe)
{
    FilmDevelopGrade grade = {.grade = film_grade_prepare(lut, &recipe->profile),
        .tone = recipe->tone, .post_gain = exp2f(recipe->tone.value[FILM_TONE_EV])};
    grade.tone.value[FILM_TONE_SATURATION] +=
        recipe->base_saturation * (1 + grade.tone.value[FILM_TONE_SATURATION]);
    for (unsigned i = 0; i < FILM_TONE_COUNT; ++i) grade.active |= grade.tone.value[i] != 0;
    return grade;
}

static float linear_luma(FilmRgb rgb)
{
    return .2126f * rgb.r + .7152f * rgb.g + .0722f * rgb.b;
}

static FilmRgb decode_rgb(FilmRgb code)
{
    return (FilmRgb){decode_srgb(code.r), decode_srgb(code.g), decode_srgb(code.b)};
}

static FilmRgb encode_rgb(FilmRgb rgb)
{
    return (FilmRgb){encode_srgb(rgb.r), encode_srgb(rgb.g), encode_srgb(rgb.b)};
}

static bool in_gamut(FilmRgb rgb)
{
    return rgb.r >= 0 && rgb.r <= 1 && rgb.g >= 0 && rgb.g <= 1 && rgb.b >= 0 && rgb.b <= 1;
}

static float tone_luma(float y, const FilmTone *tone)
{
    const float *v = tone->value;
    if (y >= 0 && y < .18f) {
        float x = y / .18f;
        y += .18f * v[FILM_TONE_SHADOWS] * x * (1 - x) * (1 - x);
    }
    // The three offsets taper to adjacent anchors. The bounded offsets keep their order.
    float black = .035f * v[FILM_TONE_BLACK];
    float mid = .075f * v[FILM_TONE_MID];
    float high = .12f * v[FILM_TONE_HIGH];
    if (y <= 0) y += black;
    else if (y < .18f) y += black + (mid - black) * (y / .18f);
    else if (y < .65f) y += mid + (high - mid) * ((y - .18f) / .47f);
    else if (y < 1) y += high * ((1 - y) / .35f);

    float width = .55f * v[FILM_TONE_SHOULDER];
    float knee = 1 - width;
    if (width > 0 && y > knee) {
        float above = y - knee;
        y = knee + width * (above / (width + above));
    }
    return y;
}

static FilmRgb extended_hsl(FilmRgb color, const FilmProfile *profile)
{
    float low = fminf(0, fminf(color.r, fminf(color.g, color.b)));
    float high = fmaxf(1, fmaxf(color.r, fmaxf(color.g, color.b)));
    float range = high - low;
    FilmRgb normalized = {(color.r - low) / range, (color.g - low) / range, (color.b - low) / range};
    FilmRgb adjusted = adjust_hsl(normalized, profile);
    return (FilmRgb){low + adjusted.r * range, low + adjusted.g * range, low + adjusted.b * range};
}

FilmRgb film_fit_chroma(FilmRgb rgb, float y, float chroma)
{
    float target = clamp01(y);
    float high = fmaxf(rgb.r, fmaxf(rgb.g, rgb.b)) - y;
    float low = y - fminf(rgb.r, fminf(rgb.g, rgb.b));
    if (high > 0) chroma = fminf(chroma, (1 - target) / high);
    if (low > 0) chroma = fminf(chroma, target / low);
    return (FilmRgb){target + chroma * (rgb.r - y), target + chroma * (rgb.g - y),
                     target + chroma * (rgb.b - y)};
}

FilmRgb film_develop_grade_pixel(FilmRgb color, const FilmDevelopGrade *grade)
{
    color = apply_lut(color, &grade->grade, true);
    bool bounded = in_gamut(color);
    if (!bounded) {
        // Keep arbitrary LUT samples within the numeric range of the linear math.
        const float limit = 4096;
        color = (FilmRgb){fmaxf(-limit, fminf(color.r, limit)),
                          fmaxf(-limit, fminf(color.g, limit)),
                          fmaxf(-limit, fminf(color.b, limit))};
    }
    if (grade->active) {
        FilmRgb rgb = decode_rgb(color);
        float gain = grade->post_gain;
        rgb = (FilmRgb){rgb.r * gain, rgb.g * gain, rgb.b * gain};
        float y = linear_luma(rgb);
        float mapped = tone_luma(y, &grade->tone);
        if (mapped != y) {
            if (y > 1e-6f && mapped >= 0) {
                float scale = mapped / y;
                rgb = (FilmRgb){rgb.r * scale, rgb.g * scale, rgb.b * scale};
            } else {
                float shift = mapped - y;
                rgb = (FilmRgb){rgb.r + shift, rgb.g + shift, rgb.b + shift};
            }
        }
        float saturation = grade->tone.value[FILM_TONE_SATURATION];
        if (!grade->grade.has_hsl) return encode_rgb(film_fit_chroma(rgb, linear_luma(rgb), 1 + saturation));
        color = extended_hsl(encode_rgb(rgb), grade->grade.profile);
        rgb = decode_rgb(color);
        return encode_rgb(film_fit_chroma(rgb, linear_luma(rgb), 1 + saturation));
    }
    if (grade->grade.has_hsl) {
        color = extended_hsl(color, grade->grade.profile);
        bounded = in_gamut(color);
    }
    if (bounded) return color;
    FilmRgb rgb = decode_rgb(color);
    return encode_rgb(film_fit_chroma(rgb, linear_luma(rgb), 1));
}

FilmRgb film_grade(FilmRgb color, const FilmLut *lut, const FilmProfile *p)
{
    FilmGrade grade = film_grade_prepare(lut, p);
    return film_grade_pixel(color, &grade);
}
