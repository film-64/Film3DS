#include "film.h"

void film_composition_reset(FilmRecipe *r)
{
    r->crop_percent = r->crop_x = r->crop_y = 0;
    r->crop_positioned = false;
}

FilmStatus film_composition_rect(unsigned w, unsigned h, const FilmRecipe *r, FilmRect *out)
{
    if (!r || !out || !film_image_bytes(w,h,FILM_RGB24) || r->crop_percent > 100)
        return FILM_INVALID;
    FilmRect maximum;
    FilmStatus status = film_crop_rect(w,h,r->aspect,&maximum);
    if (status != FILM_OK) return status;
    static const unsigned ratios[][2] = {{4,3},{3,2},{1,1},{16,9}};
    unsigned a=ratios[r->aspect][0], b=ratios[r->aspect][1];
    unsigned percent = r->crop_percent ? r->crop_percent : 100;
    unsigned units = ((maximum.width/a)*percent + 50)/100;
    if (!units) units=1;
    FilmRect rect = {(w-units*a)/2, (h-units*b)/2, units*a, units*b};
    if (r->crop_positioned) { rect.x=r->crop_x; rect.y=r->crop_y; }
    if (rect.x>w-rect.width || rect.y>h-rect.height) return FILM_INVALID;
    *out=rect;
    return FILM_OK;
}

static unsigned bounded(int value, unsigned maximum)
{
    return value<0 ? 0 : (unsigned)value>maximum ? maximum : (unsigned)value;
}

FilmStatus film_composition_percent(FilmRecipe *r, unsigned w, unsigned h, int direction)
{
    FilmRect before,after;
    if (film_composition_rect(w,h,r,&before)!=FILM_OK) return FILM_INVALID;
    int p=(r->crop_percent ? (int)r->crop_percent : 100) + (direction>0 ? 1 : direction<0 ? -1 : 0);
    if (p<1) p=1;
    if (p>100) p=100;
    FilmRecipe changed=*r;
    changed.crop_percent=(unsigned)p;
    changed.crop_positioned=false;
    if (film_composition_rect(w,h,&changed,&after)!=FILM_OK) return FILM_INVALID;
    changed.crop_x=bounded((int)after.x+(int)before.x-(int)((w-before.width)/2),w-after.width);
    changed.crop_y=bounded((int)after.y+(int)before.y-(int)((h-before.height)/2),h-after.height);
    changed.crop_positioned=true;
    *r=changed;
    return FILM_OK;
}

FilmStatus film_composition_pan(FilmRecipe *r, unsigned w, unsigned h, int dx, int dy)
{
    FilmRect rect;
    if (film_composition_rect(w,h,r,&rect)!=FILM_OK) return FILM_INVALID;
    r->crop_x=bounded((int)rect.x+dx,w-rect.width);
    r->crop_y=bounded((int)rect.y+dy,h-rect.height);
    r->crop_positioned=true;
    return FILM_OK;
}
