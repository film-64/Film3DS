#include "ui.h"

FilmUi film_ui_new(unsigned photos)
{
    return (FilmUi){.recipe = film_default_recipe(), .photos = photos};
}

void film_ui_add_photo(FilmUi *ui)
{
    if (ui->photos < 9999) ++ui->photos;
    if (ui->photos) ui->selection = ui->photos - 1;
}

FilmAction film_ui_key(FilmUi *ui, FilmKey key)
{
    if (key == UI_NEXT_PAGE) ui->page = (ui->page + 1) % 3;
    if (key == UI_BACK) ui->page = FILM_VIEW;
    if (ui->page == FILM_VIEW) {
        if (key == UI_LEFT || key == UI_RIGHT)
            ui->recipe.aspect = (ui->recipe.aspect + (key == UI_RIGHT ? 1 : 3)) % 4;
        if (key == UI_UP || key == UI_DOWN)
            ui->recipe.scale = (ui->recipe.scale + (key == UI_UP ? 1 : 2)) % 3;
        if (key == UI_ACCEPT) return UI_CAPTURE;
    } else if (ui->page == FILM_ROLL && ui->photos) {
        if (key == UI_LEFT && ui->selection) --ui->selection;
        if (key == UI_RIGHT && ui->selection + 1 < ui->photos) ++ui->selection;
        if (key == UI_ACCEPT) ui->page = FILM_DARKROOM;
    } else if (ui->page == FILM_DARKROOM) {
        if (key == UI_LEFT || key == UI_RIGHT) ui->look = (ui->look + (key == UI_RIGHT ? 1 : 3)) % 4;
        if (key == UI_UP || key == UI_DOWN) {
            float strength = ui->recipe.profile.strength + (key == UI_UP ? 0.1f : -0.1f);
            ui->recipe.profile.strength = strength < 0 ? 0 : strength > 1 ? 1 : strength;
        }
        if (key == UI_ACCEPT && ui->photos) return UI_EXPORT;
    }
    return UI_NONE;
}

FilmAction film_ui_touch(FilmUi *ui, unsigned x, unsigned y)
{
    if (x >= 320 || y >= 240) return UI_NONE;
    if (y >= 204) {
        ui->page = x < 107 ? FILM_VIEW : x < 214 ? FILM_ROLL : FILM_DARKROOM;
        return UI_NONE;
    }
    if (y >= 154) return film_ui_key(ui, UI_ACCEPT);
    if (y >= 102) return film_ui_key(ui, x < 160 ? UI_DOWN : UI_UP);
    if (y >= 57) return film_ui_key(ui, x < 160 ? UI_LEFT : UI_RIGHT);
    return UI_NONE;
}

void film_ui_apply_look(FilmRecipe *recipe, unsigned look, float strength)
{
    recipe->profile = film_default_recipe().profile;
    recipe->profile.strength = strength;
    recipe->lut_id[0] = 0;
    if (look == 1) recipe->profile.global.saturation = -strength;
    if (look == 2) {
        recipe->profile.exposure = 0.18f * strength;
        recipe->profile.zones[0].hue = 6 * strength;
        recipe->profile.zones[1].saturation = 0.25f * strength;
        recipe->grain_amount = 0.18f * strength;
    }
}
