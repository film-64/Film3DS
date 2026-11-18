#ifndef FILM_DARKROOM_H
#define FILM_DARKROOM_H
#include "film.h"
#include "raw_darkroom.h"

typedef enum {
    DARKROOM_ASPECT, DARKROOM_COMPOSITION, DARKROOM_SIZE, DARKROOM_WB, DARKROOM_WB_SHIFT, DARKROOM_LOOK, DARKROOM_EXPOSURE,
    DARKROOM_HIGHLIGHTS, DARKROOM_MIDTONES, DARKROOM_SHADOWS, DARKROOM_GRAIN_AMOUNT,
    DARKROOM_GRAIN_KIND, DARKROOM_SATURATION, DARKROOM_SHARPNESS,
    DARKROOM_STRENGTH, DARKROOM_RESAMPLING, DARKROOM_HALATION,
    DARKROOM_GRAIN_SIZE,DARKROOM_GRAIN_SHADOW,DARKROOM_GRAIN_MID,DARKROOM_GRAIN_HIGH,DARKROOM_GRAIN_BACKEND,DARKROOM_DENOISE,DARKROOM_RAW_CALIBRATION,DARKROOM_RAW_CURVE,DARKROOM_RAW_SHADOWS,DARKROOM_RAW_MIDTONES,DARKROOM_RAW_HIGHLIGHTS,DARKROOM_RAW_SHOULDER,DARKROOM_RAW_LOCAL,DARKROOM_ROW_COUNT
} FilmDarkroomRow;

enum { FILM_DARKROOM_VISIBLE_ROWS = 7, FILM_DARKROOM_ROW_Y = 51, FILM_DARKROOM_ROW_HEIGHT = 18 };

typedef struct {
    FilmDarkroomRow row;
    unsigned first_row;
    FilmImage preview, composition_view;
    unsigned quality;
    FilmDevelopCanvas canvas;
    FilmRecipe recipe;
    FilmDevelopOptions options;
    FilmGrainProfile grain_profile;
    bool has_grain_profile, wb_editor;
    FilmWhiteBalance pending_wb;
    const char *failure_phase;
} FilmDarkroom;
typedef enum { DARKROOM_INPUT_NONE, DARKROOM_INPUT_SAVE, DARKROOM_INPUT_CONTINUE } FilmDarkroomInput;

const char *film_darkroom_label(FilmDarkroomRow row);
void film_darkroom_select(FilmDarkroom *room, FilmDarkroomRow row);
void film_darkroom_move(FilmDarkroom *room, int direction);
bool film_darkroom_adjust(FilmRecipe *recipe, FilmDarkroomRow row, int direction);
bool film_darkroom_geometry_row(FilmDarkroomRow row);
void film_darkroom_update_geometry(FilmDarkroom *room, const FilmRecipe *recipe);
FilmStatus film_darkroom_pan(FilmRecipe *recipe, unsigned w, unsigned h, int dx, int dy, bool circle);
/* A preview owns final output pixels plus a full-frame composition view.
 * It never writes a file or calls a camera. SELECT only JPEG-encodes these pixels. */
FilmStatus film_darkroom_preview(FilmDarkroom *room, const FilmFrame *frame,
                                 const FilmLut *lut, const FilmDevelopOptions *options);
FilmStatus film_darkroom_scene_preview(FilmDarkroom *,const FilmRawScene *,const FilmRecipe *,
                                      const FilmLut *,const FilmDevelopOptions *);
void film_darkroom_discard(FilmDarkroom *room);
/* Non-SELECT input releases the preview and continues normal key dispatch. */
FilmDarkroomInput film_darkroom_input(FilmDarkroom *room, uint32_t keys, uint32_t select_key);
/* Only SELECT alone saves. Any other key discards, then retains its normal action.
 * The preview is released after either result, including a failed save. */
FilmStatus film_darkroom_finish(FilmDarkroom *room, bool select_only, const char *path);
#endif
