#include "darkroom.h"
#include "ui.h"
#include "film_runtime_contract.h"

#include <errno.h>
#include <math.h>
#include <sys/stat.h>

const char *film_darkroom_label(FilmDarkroomRow row)
{
    static const char *const labels[] = {
        "Aspect ratio", "Composition", "Image size", "White balance", "WB shift", "Film sim", "Exposure", "Highlights",
        "Midtones", "Shadows", "Grain effect", "Grain type", "Color",
        "Sharpness", "LUT strength", "Resampling", "Halation",
        "Grain size","Grain shadows","Grain midtones","Grain highlights","Grain backend","Chroma denoise","RAW calibration","RAW shadows","RAW midtones","RAW highlights","RAW shoulder","RAW local exposure"
    };
    return (unsigned)row < DARKROOM_ROW_COUNT ? labels[row] : "";
}

void film_darkroom_select(FilmDarkroom *room, FilmDarkroomRow row)
{
    if (!room || (unsigned)row >= DARKROOM_ROW_COUNT) return;
    room->row = row;
    if (room->first_row > (unsigned)row) room->first_row = row;
    if ((unsigned)row >= room->first_row + FILM_DARKROOM_VISIBLE_ROWS)
        room->first_row = row + 1 - FILM_DARKROOM_VISIBLE_ROWS;
}

void film_darkroom_move(FilmDarkroom *room, int direction)
{
    if (!room || !direction) return;
    unsigned next = (room->row + DARKROOM_ROW_COUNT + (direction > 0 ? 1 : -1)) % DARKROOM_ROW_COUNT;
    film_darkroom_select(room, next);
}

bool film_darkroom_adjust(FilmRecipe *r, FilmDarkroomRow row, int direction)
{
    if (!r || !direction) return false;
    int step = direction > 0 ? 1 : -1;
    FilmToneControl control;
    switch (row) {
    case DARKROOM_ASPECT:
        r->aspect = (r->aspect + 4 + step) % 4;
        film_composition_reset(r); return true;
    case DARKROOM_SIZE: r->scale = (r->scale + 3 + step) % 3; return true;
    case DARKROOM_RESAMPLING: r->filter = r->filter == FILM_BILINEAR ? FILM_BICUBIC : FILM_BILINEAR; return true;
    case DARKROOM_WB:
        if (!r->white_balance.kelvin) r->white_balance.kelvin=6500;
        else if (step<0 && r->white_balance.kelvin<=1000) r->white_balance.kelvin=0;
        else {
            int kelvin=(int)r->white_balance.kelvin+100*step;
            if (kelvin<1000) kelvin=1000;
            if (kelvin>10000) kelvin=10000;
            r->white_balance.kelvin=(unsigned)kelvin;
        }
        return true;
    case DARKROOM_EXPOSURE:
        r->input_tone.value[FILM_TONE_EV] = film_tone_limit(FILM_TONE_EV,
            (roundf(r->input_tone.value[FILM_TONE_EV] * 3) + step) / 3);
        return true;
    case DARKROOM_HIGHLIGHTS: control = FILM_TONE_HIGH; break;
    case DARKROOM_MIDTONES: control = FILM_TONE_MID; break;
    case DARKROOM_SHADOWS: control = FILM_TONE_SHADOWS; break;
    case DARKROOM_SATURATION:
        r->tone.value[FILM_TONE_SATURATION] = film_tone_limit(FILM_TONE_SATURATION,
            (roundf(r->tone.value[FILM_TONE_SATURATION] * 20) + step) / 20);
        return true;
    case DARKROOM_GRAIN_AMOUNT:
        r->grain_amount = fminf(1, fmaxf(0, (roundf(r->grain_amount * 10) + step) / 10));
        return true;
    case DARKROOM_STRENGTH:
        r->profile.strength = fminf(1, fmaxf(0, (roundf(r->profile.strength * 10) + step) / 10));
        return true;
    case DARKROOM_SHARPNESS:
        r->sharpness += step;
        if (r->sharpness < -5) r->sharpness = -5;
        if (r->sharpness > 5) r->sharpness = 5;
        return true;
    default: return false;
    }
    r->output_tone.value[control] = film_ui_curve_adjust(r->output_tone.value[control], step);
    return true;
}

void film_darkroom_discard(FilmDarkroom *room)
{
    if (!room) return;
    film_image_free(&room->preview);
    film_image_free(&room->composition_view);
    film_develop_canvas_free(&room->canvas);
    room->quality = 0;
}

bool film_darkroom_geometry_row(FilmDarkroomRow row)
{
    return row==DARKROOM_ASPECT || row==DARKROOM_COMPOSITION || row==DARKROOM_SIZE || row==DARKROOM_RESAMPLING;
}

void film_darkroom_update_geometry(FilmDarkroom *room, const FilmRecipe *r)
{
    /* Geometry edits move the frame on the full original view. They never
     * render or allow an outdated final crop to be saved. A rebuilds it. */
    if(room->recipe.aspect!=r->aspect || room->recipe.scale!=r->scale || room->recipe.filter!=r->filter ||
       room->recipe.crop_percent!=r->crop_percent || room->recipe.crop_x!=r->crop_x ||
       room->recipe.crop_y!=r->crop_y || room->recipe.crop_positioned!=r->crop_positioned)
        film_image_free(&room->preview);
    room->recipe.aspect=r->aspect; room->recipe.scale=r->scale; room->recipe.filter=r->filter;
    room->recipe.crop_percent=r->crop_percent; room->recipe.crop_x=r->crop_x; room->recipe.crop_y=r->crop_y;
    room->recipe.crop_positioned=r->crop_positioned;
}

FilmStatus film_darkroom_pan(FilmRecipe *r, unsigned w, unsigned h, int dx, int dy, bool circle)
{
    int step=circle?10:1;
    return film_composition_pan(r,w,h,(dx>0?step:dx<0?-step:0),(dy>0?step:dy<0?-step:0));
}

FilmDarkroomInput film_darkroom_input(FilmDarkroom *room, uint32_t keys, uint32_t select_key)
{
    if (!room || !room->preview.pixels || !keys) return DARKROOM_INPUT_NONE;
    if (select_key && keys == select_key) return DARKROOM_INPUT_SAVE;
    film_darkroom_discard(room);
    return DARKROOM_INPUT_CONTINUE;
}

FilmStatus film_darkroom_preview(FilmDarkroom *room, const FilmFrame *frame,
                                 const FilmLut *lut, const FilmDevelopOptions *options)
{
    if (!room || !frame) return FILM_INVALID;
    film_darkroom_discard(room);
    if (!film_runtime_source_valid("film-darkroom/v1")) return FILM_STATE;
    room->failure_phase="color";
    FilmStatus status = film_develop_canvas(frame,lut,options,&room->canvas);
    if (status==FILM_OK) {room->failure_phase="composition";status=film_develop_canvas_render(&room->canvas,&frame->recipe,options,true,&room->composition_view);}
    if (status==FILM_OK) {room->failure_phase="render";status=film_develop_canvas_render(&room->canvas,&frame->recipe,options,false,&room->preview);}
    if (status == FILM_OK) {
        room->failure_phase=NULL;
        room->quality = frame->recipe.jpeg_quality;
        room->recipe=frame->recipe;
        room->options=options?*options:(FilmDevelopOptions){0};
        room->options.camera_color=NULL;
        room->options.raw_isp=NULL; /* calibration already baked into this display cache */
        room->has_grain_profile=options && options->grain_profile;
        if (room->has_grain_profile) room->grain_profile=*options->grain_profile;
        room->options.grain_profile=NULL;
    } else film_darkroom_discard(room);
    return status;
}

FilmStatus film_darkroom_finish(FilmDarkroom *room, bool select_only, const char *path)
{
    if (!room || !room->preview.pixels) return FILM_STATE;
    FilmStatus status = FILM_OK;
    if (select_only) {
        struct stat existing;
        if (!film_runtime_source_valid("film-darkroom-save/v1")) status = FILM_STATE;
        else if (!path || !*path) status = FILM_INVALID;
        else if (stat(path, &existing) == 0) status = FILM_STATE;
        else if (errno != ENOENT) status = FILM_IO;
        else {
            status=film_jpeg_write(path,&room->preview,room->quality);
        }
    }
    film_darkroom_discard(room);
    return status;
}
