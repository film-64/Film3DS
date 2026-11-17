#include "film.h"
#include "film_runtime_contract.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static float luma(FilmRgb p){return .2126f*p.r+.7152f*p.g+.0722f*p.b;}
static unsigned edge(int x,unsigned n){return x<0?0:(unsigned)x>=n?n-1:(unsigned)x;}
static float median(float *v)
{for(unsigned i=1;i<9;++i){float t=v[i];unsigned j=i;while(j&&v[j-1]>t){v[j]=v[j-1];--j;}v[j]=t;}return v[4];}
FilmStatus film_darkroom_denoise(const char *source,FilmRgb *pixels,unsigned w,unsigned h,float amount)
{
    if(!source||strcmp(source,"film-darkroom-denoise/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!pixels||!w||!h||(size_t)w>FILM_RAW_MAX_PIXELS/h||!isfinite(amount)||amount<0||amount>1)return FILM_INVALID;
    if(!amount)return FILM_OK;
    size_t n=(size_t)w*h;
    for(size_t i=0;i<n;++i)if(!isfinite(pixels[i].r)||!isfinite(pixels[i].g)||!isfinite(pixels[i].b))return FILM_INVALID;
    FilmRgb *clean=malloc(n*sizeof *clean);if(!clean)return FILM_NOMEM;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){float cr[9],cb[9];unsigned count=0;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){FilmRgb p=pixels[(size_t)edge((int)y+dy,h)*w+edge((int)x+dx,w)];float l=luma(p);cr[count]=p.r-l;cb[count++]=p.b-l;}
        size_t i=(size_t)y*w+x;FilmRgb p=pixels[i];float l=luma(p);
        float r=l+(p.r-l)*(1-amount)+median(cr)*amount,b=l+(p.b-l)*(1-amount)+median(cb)*amount;
        clean[i]=(FilmRgb){r,(l-.2126f*r-.0722f*b)/.7152f,b};
    }
    memcpy(pixels,clean,n*sizeof *pixels);free(clean);return FILM_OK;
}
