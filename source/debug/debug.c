#include "debug.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static bool number(const char *str, uint32_t *out)
{
    if (!str || !*str) return false;
    uint32_t value = 0;
    for (; *str; ++str) {
        if (*str < '0' || *str > '9') return false;
        unsigned digit = (unsigned)(*str - '0');
        if (value > (UINT32_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

void film_log(FILE *log, const char *operation, FilmStatus status, uint32_t result)
{
    if (log) {
        fprintf(log, "%s %s 0x%08" PRIx32 "\n", operation, film_status_name(status), result);
        fflush(log);
    }
}

void film_debug_clear(FilmDebug *d)
{
    film_frame_free(&d->frame);
    film_image_free(&d->developed);
}

static FilmStatus execute(FilmDebug *d, const char *cmd, const char *arg, const char *second, char *data)
{
    if (!strcmp(cmd, "STATUS") && !arg) {
        snprintf(data, 128, "ready=%u captured=%u developed=%u", d->ready,
                 d->frame.source.pixels != NULL, d->developed.pixels != NULL);
        return FILM_OK;
    }
    if (!strcmp(cmd, "ERROR") && !arg) {
        uint32_t rc = d->ops.device_result ? d->ops.device_result(d->context) : 0;
        snprintf(data, 128, "%s device=0x%08" PRIx32, film_status_name(d->last_error), rc);
        return FILM_OK;
    }
    if (!strcmp(cmd, "TIME") && !arg) {
        snprintf(data, 128, "us=%" PRIu64, d->elapsed_us);
        return FILM_OK;
    }
    if (!strcmp(cmd, "MEM") && !arg) {
        FilmImage a = d->frame.source, b = d->developed;
        size_t bytes = film_image_bytes(a.width, a.height, a.format);
        unsigned scaled_w, scaled_h;
        if (b.pixels && film_scaled_dims(a.width, a.height, d->frame.recipe.scale,
                                         &scaled_w, &scaled_h) == FILM_OK)
            bytes += film_image_bytes(scaled_w, scaled_h, b.format);
        if (d->lut) bytes += (size_t)d->lut->size * d->lut->size * d->lut->size * sizeof(FilmRgb);
        snprintf(data, 128, "logical_buffer_bytes=%zu", bytes);
        return FILM_OK;
    }
    if (!strcmp(cmd, "INIT") && !second) {
        if (arg && strcmp(arg, "YUYV") && strcmp(arg, "RGB565")) return FILM_INVALID;
        FilmPixelFormat format = arg && !strcmp(arg, "RGB565") ? FILM_RGB565_LE : FILM_YUYV;
        FilmStatus status = d->ops.init(d->context, format);
        if (status == FILM_OK) d->ready = true;
        return status;
    }
    if (!strcmp(cmd, "CAPTURE") && !arg) {
        if (!d->ready) return FILM_STATE;
        FilmFrame frame = {.recipe = d->ui.recipe};
        FilmStatus status = d->ops.capture(d->context, &frame);
        if (status == FILM_OK) {
            film_debug_clear(d);
            d->frame = frame;
        } else film_frame_free(&frame);
        return status;
    }
    if (!strcmp(cmd, "LOAD") && !arg) {
        FilmFrame frame = {0};
        FilmStatus status = film_frame_read(d->source_path, &frame);
        if (status == FILM_OK) {
            film_debug_clear(d);
            d->frame = frame;
        }
        return status;
    }
    if (!strcmp(cmd, "INFO") && !arg) {
        FilmImage i = d->frame.source;
        if (!i.pixels) return FILM_STATE;
        snprintf(data, 128, "%u %u %u %zu", i.width, i.height, i.format,
                 film_image_bytes(i.width, i.height, i.format));
        return FILM_OK;
    }
    if (!strcmp(cmd, "READ")) {
        uint32_t offset, count;
        if (!number(arg, &offset) || !number(second, &count) || !count || count > 256) return FILM_INVALID;
        FilmImage i = d->frame.source;
        if (!i.pixels) return FILM_STATE;
        size_t bytes = film_image_bytes(i.width, i.height, i.format);
        if (offset >= bytes || count > bytes - offset) return FILM_INVALID;
        static const char hex[] = "0123456789abcdef";
        for (uint32_t at = 0; at < count; ++at) {
            uint8_t b = i.pixels[offset + at];
            data[at * 2] = hex[b >> 4];
            data[at * 2 + 1] = hex[b & 15];
        }
        data[count * 2] = 0;
        return FILM_OK;
    }
    if (!strcmp(cmd, "DEVELOP") && !second) {
        if (!d->frame.source.pixels) return FILM_STATE;
        if (arg && strcmp(arg, "LUT") && strcmp(arg, "PLAIN") && strcmp(arg, "MONO") &&
            strcmp(arg, "WARM")) return FILM_INVALID;
        const FilmLut *lut = arg && !strcmp(arg, "PLAIN") ? NULL : d->lut;
        if (arg && !strcmp(arg, "LUT") && !lut) return FILM_STATE;
        FilmFrame frame = d->frame;
        if (arg && !strcmp(arg, "PLAIN")) frame.recipe.lut_id[0] = 0;
        if (arg && (!strcmp(arg, "MONO") || !strcmp(arg, "WARM"))) {
            film_ui_apply_look(&frame.recipe, !strcmp(arg, "MONO") ? 1 : 2,
                              d->ui.recipe.profile.strength);
            lut = NULL;
        }
        FilmImage image = {0};
        FilmStatus status = film_develop(&frame, lut, &image);
        if (status == FILM_OK) {
            film_image_free(&d->developed);
            d->developed = image;
            snprintf(data, 128, "%u %u", image.width, image.height);
        }
        return status;
    }
    if (!strcmp(cmd, "SAVE") && !arg) {
        if (!d->developed.pixels) return FILM_STATE;
        return film_jpeg_write(d->jpeg_path, &d->developed, d->frame.recipe.jpeg_quality);
    }
    if (!strcmp(cmd, "SOURCE") && !arg) {
        if (!d->frame.source.pixels) return FILM_STATE;
        return film_frame_write(d->source_path, &d->frame);
    }
    if (!strcmp(cmd, "UI")) {
        FilmAction action = UI_NONE;
        uint32_t a, b;
        if (arg && !strcmp(arg, "ADD") && !second) film_ui_add_photo(&d->ui);
        else if (second && number(arg, &a) && number(second, &b)) action = film_ui_touch(&d->ui, a, b);
        else if (arg && number(arg, &a) && !second && a <= UI_ACCEPT) action = film_ui_key(&d->ui, a);
        else if (arg) return FILM_INVALID;
        snprintf(data, 128, "%u %u %u %u %u %u %u %u", d->ui.page, d->ui.recipe.aspect, d->ui.recipe.scale,
                 d->ui.selection, d->ui.photos, d->ui.look, (unsigned)(d->ui.recipe.profile.strength * 100 + 0.5f),
                 action);
        return FILM_OK;
    }
    return FILM_INVALID;
}

void film_debug_execute(FilmDebug *d, const char *line, char reply[FILM_DEBUG_LINE])
{
    uint32_t id = 0;
    FilmStatus status = FILM_INVALID;
    char data[513] = {0}, request[256];
    size_t length = strlen(line);
    if (length < sizeof request) {
        memcpy(request, line, length + 1);
        const char *delimiters = " \t\r\n";
        char *save;
        char *magic = strtok_r(request, delimiters, &save);
        char *serial = strtok_r(NULL, delimiters, &save);
        char *cmd = strtok_r(NULL, delimiters, &save);
        char *arg = strtok_r(NULL, delimiters, &save);
        char *second = strtok_r(NULL, delimiters, &save);
        if (magic && !strcmp(magic, "F3D1") && number(serial, &id) && cmd &&
            !strtok_r(NULL, delimiters, &save)) {
            bool preserve = !strcmp(cmd, "TIME") || !strcmp(cmd, "ERROR") ||
                            !strcmp(cmd, "MEM") || !strcmp(cmd, "STATUS") ||
                            !strcmp(cmd, "INFO") || !strcmp(cmd, "READ");
            uint64_t start = d->ops.microseconds(d->context);
            status = execute(d, cmd, arg, second, data);
            if (!preserve) {
                uint64_t end = d->ops.microseconds(d->context);
                d->elapsed_us = end >= start ? end - start : 0;
                d->last_error = status;
            }
        }
    }
    if (status != FILM_OK) snprintf(data, sizeof data, "%s", film_status_name(status));
    snprintf(reply, FILM_DEBUG_LINE, "F3D1 %" PRIu32 " %u%s%s\n", id, status, *data ? " " : "", data);
}

void film_debug_serve(FilmDebug *d, FILE *input, FILE *output)
{
    char line[256], reply[FILM_DEBUG_LINE];
    while (fgets(line, sizeof line, input)) {
        if (!strchr(line, '\n') && !feof(input)) {
            int c;
            while ((c = fgetc(input)) != '\n' && c != EOF) {}
            line[0] = '\0';
        }
        film_debug_execute(d, line, reply);
        if (fputs(reply, output) == EOF || fflush(output) != 0) break;
    }
}
