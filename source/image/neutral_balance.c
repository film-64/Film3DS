#include "raw_darkroom.h"
#include "film_runtime_contract.h"
#include <math.h>

static bool overlaps(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    if(an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y)return true;
    return an&&bn&&x<y+bn&&y<x+an;
}

FilmStatus film_raw_scene_neutral(const FilmRawScene *scene,const FilmNeutralPatch *patch,FilmNeutralBalance *out)
{
    if(!film_runtime_source_valid("film-neutral-balance/v1"))return FILM_STATE;
    if(!scene||!scene->pixels||!patch||!out||!scene->width||!scene->height||
       (size_t)scene->width>FILM_RAW_MAX_PIXELS/scene->height||
       !patch->width||!patch->height||patch->x>=scene->width||patch->y>=scene->height||
       patch->width>scene->width-patch->x||patch->height>scene->height-patch->y||
       !isfinite(patch->minimum)||!isfinite(patch->maximum)||
       patch->minimum<=0||patch->maximum<=patch->minimum)return FILM_INVALID;
    size_t bytes=(size_t)scene->width*scene->height*sizeof *scene->pixels;
    if(overlaps(out,sizeof *out,scene->pixels,bytes)||overlaps(out,sizeof *out,scene,sizeof *scene)||
       overlaps(out,sizeof *out,patch,sizeof *patch))return FILM_INVALID;
    FilmNeutralBalance result={0};
    double mean[2]={0},m2[2]={0};
    for(unsigned y=patch->y;y<patch->y+patch->height;++y)
        for(unsigned x=patch->x;x<patch->x+patch->width;++x){
            FilmRgb p=scene->pixels[(size_t)y*scene->width+x];
            if(!isfinite(p.r)||!isfinite(p.g)||!isfinite(p.b)||
               p.r<patch->minimum||p.g<patch->minimum||p.b<patch->minimum||
               p.r>patch->maximum||p.g>patch->maximum||p.b>patch->maximum){++result.rejected;continue;}
            double ratio[2]={log2((double)p.r/p.g),log2((double)p.b/p.g)};
            ++result.accepted;
            for(unsigned c=0;c<2;++c){double d=ratio[c]-mean[c];mean[c]+=d/result.accepted;m2[c]+=d*(ratio[c]-mean[c]);}
        }
    if(result.accepted<9||result.accepted*4u<(result.accepted+result.rejected)*3u)return FILM_INVALID;
    result.spread_stops=(float)sqrt(fmax(m2[0],m2[1])/result.accepted);
    if(result.spread_stops>.15f)return FILM_INVALID;
    /* Normalize chromaticity before fitting: exposure and gray-card reflectance
     * must not influence the correction. The caller identifies a neutral patch. */
    double rgb[3]={exp2(mean[0]),1,exp2(mean[1])},best=INFINITY;
    for(unsigned candidate=0;candidate<=91;++candidate){
        unsigned kelvin=candidate?900+candidate*100:0;
        float matrix[3][3];
        if(film_white_balance_matrix((FilmWhiteBalance){kelvin,0,0},matrix)!=FILM_OK)return FILM_STATE;
        double v[3]={0};
        for(unsigned c=0;c<3;++c)for(unsigned j=0;j<3;++j)v[c]+=matrix[c][j]*rgb[j];
        if(v[0]<=0||v[1]<=0||v[2]<=0)continue;
        double r=log2(v[0]/v[1]),b=log2(v[2]/v[1]);
        int red=(int)lround(fmax(-9,fmin(9,-12*r)));
        int blue=(int)lround(fmax(-9,fmin(9,-12*b)));
        double error=fmax(fabs(r+red/12.),fabs(b+blue/12.));
        if(error<best){best=error;result.balance=(FilmWhiteBalance){kelvin,red,blue};}
    }
    if(best>.08)return FILM_UNSUPPORTED;
    result.residual_stops=(float)best;
    *out=result;
    return FILM_OK;
}
