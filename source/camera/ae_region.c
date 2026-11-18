#include "ae_region.h"

static unsigned cap(unsigned n,unsigned max){return n>max?max:n;}
FilmAeRegion film_ae_region_default(void)
{return (FilmAeRegion){0,0,FILM_AE_VGA_WIDTH,FILM_AE_VGA_HEIGHT};}
bool film_ae_region_valid(FilmAeRegion r)
{
    return r.width>=80&&r.height>=60&&r.width<=640&&r.height<=480&&
        r.width*3==r.height*4&&!(r.x&1)&&!(r.y&1)&&
        r.x<=640-r.width&&r.y<=480-r.height;
}
bool film_ae_region_is_full(FilmAeRegion r)
{return r.x==0&&r.y==0&&r.width==640&&r.height==480;}
static FilmAeRegion resize_at(FilmAeRegion r,unsigned level)
{
    const unsigned sizes[]={640,480,320,160,80};
    unsigned width=sizes[level],height=width*3/4;
    unsigned cx=r.x+r.width/2,cy=r.y+r.height/2;
    unsigned nx=cx>width/2?cx-width/2:0,ny=cy>height/2?cy-height/2:0;
    return (FilmAeRegion){cap((nx/2)*2,640-width),cap((ny/2)*2,480-height),width,height};
}
FilmAeRegion film_ae_region_resize(FilmAeRegion r,int direction)
{
    if(!film_ae_region_valid(r))return film_ae_region_default();
    const unsigned sizes[]={640,480,320,160,80};
    unsigned level=0;
    while(level+1<FILM_AE_LEVELS&&sizes[level]!=r.width)++level;
    int next=(int)level+direction;
    if(next<0)next=0;
    if(next>=FILM_AE_LEVELS)next=FILM_AE_LEVELS-1;
    return resize_at(r,(unsigned)next);
}
FilmAeRegion film_ae_region_move(FilmAeRegion r,int dx,int dy)
{
    if(!film_ae_region_valid(r))return film_ae_region_default();
    int x=(int)r.x+dx*40,y=(int)r.y+dy*30;
    if(x<0)x=0;if(y<0)y=0;
    if(x>(int)(640-r.width))x=(int)(640-r.width);
    if(y>(int)(480-r.height))y=(int)(480-r.height);
    r.x=(unsigned)x&~1u;r.y=(unsigned)y&~1u;
    return r;
}
