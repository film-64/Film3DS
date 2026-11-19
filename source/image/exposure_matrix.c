#include "exposure_matrix.h"
#include "film_runtime_contract.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float clampf(float value,float lo,float hi)
{return fminf(fmaxf(value,lo),hi);}

FilmStatus film_gray_curve_read(FILE *file,FilmExposureCurve *out)
{
    if(!file||!out)return FILM_INVALID;
    char line[96],tail;
    if(!fgets(line,sizeof line,file)||strcmp(line,"F3GRAYCURVE1\n"))return FILM_CORRUPT;
    static const char *const names[]={"ev","shadows","midtones","highlights","shoulder"};
    float *value[5];
    FilmExposureCurve curve={0};
    value[0]=&curve.ev;value[1]=&curve.shadows;value[2]=&curve.midtones;
    value[3]=&curve.highlights;value[4]=&curve.shoulder;
    for(unsigned i=0;i<5;++i){
        char key[32];
        if(!fgets(line,sizeof line,file)||sscanf(line,"%31s %f %c",key,value[i],&tail)!=2||
           strcmp(key,names[i]))return FILM_CORRUPT;
    }
    if(fgetc(file)!=EOF||ferror(file)||!film_exposure_curve_valid(&curve))
        return FILM_CORRUPT;
    *out=curve;return FILM_OK;
}

FilmExposureMatrix film_exposure_matrix_default(void)
{return (FilmExposureMatrix){0};}

bool film_exposure_curve_valid(const FilmExposureCurve *curve)
{
    return curve && isfinite(curve->ev) && fabsf(curve->ev)<=4 &&
        isfinite(curve->shadows) && fabsf(curve->shadows)<=2.5f &&
        isfinite(curve->midtones) && fabsf(curve->midtones)<=2.5f &&
        isfinite(curve->highlights) && fabsf(curve->highlights)<=2.5f &&
        isfinite(curve->shoulder) && curve->shoulder>=0 && curve->shoulder<=.95f;
}

FilmStatus film_exposure_curve_apply(const FilmExposureCurve *curve,
                                    FilmRecipe *recipe,FilmDevelopOptions *options)
{
    if(!film_exposure_curve_valid(curve)||!recipe||!options)return FILM_INVALID;
    FilmRecipe candidate=*recipe;
    FilmDevelopOptions updated=*options;
    candidate.input_tone.value[FILM_TONE_EV]=curve->ev;
    updated.raw_shadows=curve->shadows;
    updated.raw_midtones=curve->midtones;
    updated.raw_highlights=curve->highlights;
    updated.raw_shoulder=curve->shoulder;
    updated.raw_local=0; /* A preset never silently introduces local exposure. */
    if(film_recipe_validate(&candidate)!=FILM_OK)return FILM_INVALID;
    *recipe=candidate;
    *options=updated;
    return FILM_OK;
}

static int ascending(const void *a,const void *b)
{
    float x=*(const float *)a,y=*(const float *)b;
    return (x>y)-(x<y);
}

/* Bounded fixed-grid estimator: one deterministic sample per 16x16 cell.
 * Fit median exposure globally, then low/high residuals in scene EV.
 * Never interpolate hardware controls, copy an ISP register or alter RAW. */
FilmStatus film_exposure_matrix_fit(const char *source,FilmExposureMatrix *matrix,const FilmRawScene *scene)
{
    if(!source||strcmp(source,"film-exposure-matrix-fit/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!matrix||!scene||!scene->pixels||scene->width<16||scene->height<16||
       !matrix->preview_valid||matrix->fitted)return FILM_INVALID;
    float *ref=matrix->preview_linear;
    if(!isfinite(ref[0])||!isfinite(ref[1])||!isfinite(ref[2])||
       ref[0]<.001f||ref[0]>ref[1]||ref[1]>ref[2]||ref[2]>1)return FILM_INVALID;
    float values[256];
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){
        unsigned px=((2*x+1)*scene->width)/32,py=((2*y+1)*scene->height)/32;
        FilmRgb v=scene->pixels[(size_t)py*scene->width+px];
        if(!isfinite(v.r)||!isfinite(v.g)||!isfinite(v.b))return FILM_CORRUPT;
        float lum=.2126f*v.r+.7152f*v.g+.0722f*v.b;
        values[y*16+x]=fmaxf(0,lum);
    }
    qsort(values,256,sizeof values[0],ascending);
    float raw[3]={values[25],values[127],values[229]};
    if(raw[0]<.001f||raw[1]<.002f||raw[2]<raw[1])return FILM_STATE;
    float ev=clampf(log2f(ref[1]/raw[1]),-2,2);
    FilmExposureCurve shot={
        .ev=ev,
        .shadows=clampf(log2f(ref[0]/raw[0])-ev,-1.5f,1.5f),
        .midtones=0,
        .highlights=clampf(log2f(ref[2]/raw[2])-ev,-1.5f,1.5f),
        .shoulder=0
    };
    if(!film_exposure_curve_valid(&shot))return FILM_INVALID;
    /* The middle anchor is a true identity residual: EV is applied exactly
     * once by the existing linear-scene pipeline, then the three offsets. */
    matrix->neutral=(FilmExposureCurve){0};
    matrix->shot=shot;
    matrix->fitted=true;
    return FILM_OK;
}
