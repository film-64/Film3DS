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
        float cell = clamp01((in[i] - low[i]) / (high[i] - low[i])) * (n - 1);
        unsigned index = (unsigned)cell;
        if (index == n - 1) --index;
        base += index * stride[i];
        fraction[i] = cell - index;
        for (unsigned j = i; j && fraction[order[j]] > fraction[order[j - 1]]; --j) {
            unsigned swap = order[j];
            order[j] = order[j - 1];
            order[j - 1] = swap;
        }
    }
    // Walk the tetrahedron from its lowest to highest corner in fraction order.
    FilmRgb result = {0, 0, 0};
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

static float to_lut(float value, FilmTransfer transfer)
{
    if (transfer == FILM_SRGB) return value;
    float linear = decode_srgb(value);
    if (transfer == FILM_REC709)
        return linear < 0.018f ? 4.5f * linear : 1.099f * powf(linear, 0.45f) - 0.099f;
    return linear;
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
    if (!p || (unsigned)p->input > FILM_REC709 || (unsigned)p->output > FILM_REC709 ||
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

FilmRgb film_grade(FilmRgb color, const FilmLut *lut, const FilmProfile *p)
{
    if (p->exposure) {
        float gain = exp2f(p->exposure);
        color = (FilmRgb){encode_srgb(decode_srgb(color.r) * gain),
                         encode_srgb(decode_srgb(color.g) * gain),
                         encode_srgb(decode_srgb(color.b) * gain)};
    }
    if (lut && p->strength) {
        FilmRgb input = {to_lut(color.r, p->input), to_lut(color.g, p->input), to_lut(color.b, p->input)};
        FilmRgb mapped = film_lut_sample(lut, input);
        mapped = (FilmRgb){from_lut(mapped.r, p->output), from_lut(mapped.g, p->output),
                          from_lut(mapped.b, p->output)};
        color = (FilmRgb){color.r * (1 - p->strength) + mapped.r * p->strength,
                          color.g * (1 - p->strength) + mapped.g * p->strength,
                          color.b * (1 - p->strength) + mapped.b * p->strength};
    }
    color = (FilmRgb){clamp01(color.r), clamp01(color.g), clamp01(color.b)};
    return adjust_hsl(color, p);
}
