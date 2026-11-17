#include "ui.h"

typedef struct { FilmConsoleControl control; FilmRect rect; } ConsoleRegion;

static const ConsoleRegion camera[] = {
    {CONSOLE_TAKE_LEFT, {10, 54, 102, 104}},
    {CONSOLE_TAKE_RIGHT, {208, 54, 102, 104}},
    {CONSOLE_LENS, {119, 76, 82, 60}},
    {CONSOLE_PHOTOS, {10, 186, 230, 44}},
    {CONSOLE_LEVER, {254, 180, 56, 50}},
    {CONSOLE_NONE, {0}},
};

static const ConsoleRegion tools[] = {
    {CONSOLE_TAKE_LEFT, {10, 42, 102, 76}},
    {CONSOLE_TAKE_RIGHT, {208, 42, 102, 76}},
    {CONSOLE_LENS, {119, 50, 82, 58}},
    {CONSOLE_SETTINGS, {10, 126, 82, 47}},
    {CONSOLE_FRAME, {97, 126, 68, 47}},
    {CONSOLE_SIZE, {170, 126, 65, 47}},
    {CONSOLE_ZOOM, {240, 126, 70, 47}},
    {CONSOLE_PHOTOS, {10, 186, 230, 44}},
    {CONSOLE_LEVER, {254, 180, 56, 50}},
    {CONSOLE_NONE, {0}},
};

static const ConsoleRegion manual[] = {
    {CONSOLE_PREV, {10, 53, 48, 51}},
    {CONSOLE_NEXT, {262, 53, 48, 51}},
    {CONSOLE_LESS, {10, 115, 56, 52}},
    {CONSOLE_MORE, {254, 115, 56, 52}},
    {CONSOLE_CLOSE, {10, 187, 300, 43}},
    {CONSOLE_NONE, {0}},
};

static const ConsoleRegion roll[] = {
    {CONSOLE_PREV, {6, 58, 34, 85}},
    {CONSOLE_THUMB_0, {45, 58, 74, 85}},
    {CONSOLE_THUMB_1, {123, 58, 74, 85}},
    {CONSOLE_THUMB_2, {201, 58, 74, 85}},
    {CONSOLE_NEXT, {280, 58, 34, 85}},
    {CONSOLE_CLOSE, {10, 185, 117, 45}},
    {CONSOLE_PRIMARY, {136, 185, 174, 45}},
    {CONSOLE_NONE, {0}},
};

static const ConsoleRegion darkroom[] = {
    {CONSOLE_LOOK, {10, 43, 97, 33}},
    {CONSOLE_GRAIN, {112, 43, 96, 33}},
    {CONSOLE_TONE, {213, 43, 97, 33}},
    {CONSOLE_PREV, {10, 84, 45, 40}},
    {CONSOLE_NEXT, {265, 84, 45, 40}},
    {CONSOLE_LESS, {10, 130, 45, 37}},
    {CONSOLE_MORE, {265, 130, 45, 37}},
    {CONSOLE_FRAME, {10, 174, 91, 23}},
    {CONSOLE_HALATION, {105, 174, 98, 23}},
    {CONSOLE_SAVE, {207, 174, 103, 23}},
    {CONSOLE_CLOSE, {10, 202, 95, 31}},
    {CONSOLE_PRIMARY, {112, 202, 198, 31}},
    {CONSOLE_NONE, {0}},
};

static const ConsoleRegion *const regions[] = {camera, tools, manual, roll, darkroom};

FilmRect film_console_rect(FilmConsolePage page, FilmConsoleControl control)
{
    if ((unsigned)page >= sizeof regions / sizeof *regions) return (FilmRect){0};
    for (const ConsoleRegion *region = regions[page]; region->control; ++region)
        if (region->control == control) return region->rect;
    return (FilmRect){0};
}

FilmConsoleControl film_console_hit(FilmConsolePage page, unsigned x, unsigned y)
{
    if ((unsigned)page >= sizeof regions / sizeof *regions) return CONSOLE_NONE;
    for (const ConsoleRegion *region = regions[page]; region->control; ++region) {
        FilmRect r = region->rect;
        if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
            return region->control;
    }
    return CONSOLE_NONE;
}
