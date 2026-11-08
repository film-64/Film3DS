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
    unsigned look;
} FilmUi;

FilmUi film_ui_new(unsigned photos);
FilmAction film_ui_key(FilmUi *ui, FilmKey key);
FilmAction film_ui_touch(FilmUi *ui, unsigned x, unsigned y);
void film_ui_apply_look(FilmRecipe *recipe, unsigned look, float strength);
void film_ui_add_photo(FilmUi *ui);
#endif
