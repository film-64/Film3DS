#ifndef FILM_UI_H
#define FILM_UI_H
#include "film.h"

typedef enum { FILM_VIEW, FILM_ROLL, FILM_DARKROOM } FilmPage;
typedef enum { UI_NEXT_PAGE, UI_BACK, UI_LEFT, UI_RIGHT, UI_UP, UI_DOWN, UI_ACCEPT } FilmKey;
typedef enum { UI_NONE, UI_CAPTURE, UI_EXPORT } FilmAction;

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
void film_ui_apply_look(FilmRecipe *recipe, unsigned look, float strength);
void film_ui_select_look(FilmRecipe *recipe, const FilmRecipe *saved, const char *id,
                         const FilmProfile *preset);
void film_ui_add_photo(FilmUi *ui);
FilmAspect film_ui_output_aspect(const FilmUi *ui, FilmAspect saved);
void film_ui_change_aspect(FilmUi *ui, FilmAspect current, bool next);
#endif
