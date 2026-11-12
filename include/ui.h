#ifndef FILM_UI_H
#define FILM_UI_H
#include "film.h"

typedef enum { FILM_VIEW, FILM_ROLL, FILM_DARKROOM } FilmPage;
typedef enum { UI_NEXT_PAGE, UI_BACK, UI_LEFT, UI_RIGHT, UI_UP, UI_DOWN, UI_ACCEPT } FilmKey;
typedef enum { UI_NONE, UI_CAPTURE, UI_EXPORT } FilmAction;
typedef enum { CONSOLE_CAMERA, CONSOLE_TOOLS, CONSOLE_MANUAL, CONSOLE_ROLL, CONSOLE_DARKROOM } FilmConsolePage;
typedef enum {
    CONSOLE_NONE, CONSOLE_TAKE_LEFT, CONSOLE_TAKE_RIGHT, CONSOLE_LENS, CONSOLE_LEVER,
    CONSOLE_PHOTOS, CONSOLE_SETTINGS, CONSOLE_FRAME, CONSOLE_SIZE, CONSOLE_ZOOM,
    CONSOLE_CLOSE, CONSOLE_PREV, CONSOLE_NEXT, CONSOLE_LESS, CONSOLE_MORE,
    CONSOLE_THUMB_0, CONSOLE_THUMB_1, CONSOLE_THUMB_2, CONSOLE_PRIMARY,
    CONSOLE_LOOK, CONSOLE_GRAIN, CONSOLE_TONE, CONSOLE_HALATION, CONSOLE_SAVE
} FilmConsoleControl;
enum { FILM_ROLL_THUMBNAILS = CONSOLE_THUMB_2 - CONSOLE_THUMB_0 + 1 };

typedef struct {
    FilmPage page;
    FilmRecipe recipe;
    unsigned selection, photos;
    unsigned look, looks;
    bool aspect_selected;
} FilmUi;

FilmUi film_ui_new(unsigned photos);
FilmAction film_ui_key(FilmUi *ui, FilmKey key);
FilmAction film_ui_touch(FilmUi *ui, unsigned x, unsigned y);
FilmRect film_console_rect(FilmConsolePage page, FilmConsoleControl control);
FilmConsoleControl film_console_hit(FilmConsolePage page, unsigned x, unsigned y);
void film_ui_apply_look(FilmRecipe *recipe, unsigned look, float strength);
void film_ui_select_look(FilmRecipe *recipe, const FilmRecipe *saved, const char *id,
                         const FilmProfile *preset);
void film_ui_add_photo(FilmUi *ui);
FilmAspect film_ui_output_aspect(const FilmUi *ui, FilmAspect saved);
void film_ui_change_aspect(FilmUi *ui, FilmAspect current, bool next);
#endif
