#include "raw_darkroom.h"
#include "film_runtime_contract.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

FilmStatus film_raw_frame_prepare(const FilmFrame *frame,const FilmDevelopOptions *options,FilmRawPrepared *out)
{
    if(!film_runtime_source_valid("film-app-raw-canvas/v1"))return FILM_STATE;
    if(!frame||frame->source.format!=FILM_RAW10_RGGB||!frame->source.pixels||film_recipe_validate(&frame->recipe)!=FILM_OK)return FILM_INVALID;
    const FilmImage *im=&frame->source;size_t bytes=film_image_bytes(im->width,im->height,im->format);if(!bytes)return FILM_INVALID;
    uint16_t *samples=malloc(bytes);if(!samples)return FILM_NOMEM;
    FilmStatus s=film_raw10_unpack("film-raw10-unpack/v1",im->pixels,bytes,im->width,im->height,samples,bytes/2);
    FilmRawIsp isp=film_raw_isp_default();isp.exposure_ev=frame->recipe.input_tone.value[FILM_TONE_EV];isp.shoulder=.75f;
    FilmRaw10View view={im->width,im->height,samples};
    if(s==FILM_OK)s=film_raw_isp_prepare("film-raw-isp/v1",&view,&isp,out);
    free(samples);
    if(s==FILM_OK&&options&&options->halation)s=film_raw_prepared_halation(out);
    if(s==FILM_OK&&options)s=film_darkroom_denoise("film-darkroom-denoise/v1",out->pixels,out->width,out->height,options->chroma_denoise);
    if(s!=FILM_OK)film_raw_prepared_free(out);
    return s;
}
FilmStatus film_raw_export_pair(const FilmFrame *frame,const FilmLut *lut,const FilmDevelopOptions *options,const char *plain,const char *look)
{
    if(!film_runtime_source_valid("film-app-raw-export-pair/v1"))return FILM_STATE;
    if(!plain||!look||!strcmp(plain,look))return FILM_INVALID;
    const char *paths[]={plain,look};struct stat st;
    for(unsigned i=0;i<2;++i)if(!stat(paths[i],&st)||errno!=ENOENT)return FILM_STATE;
    FilmRawPrepared prepared={0};FilmStatus s=film_raw_frame_prepare(frame,options,&prepared);
    for(unsigned i=0;s==FILM_OK&&i<2;++i){
        FilmRecipe recipe=frame->recipe;recipe.input_tone.value[FILM_TONE_EV]=0;recipe.base_saturation=0;
        if(!i){recipe.profile.strength=0;recipe.lut_id[0]=0;}
        FilmDevelopCanvas canvas={0};FilmImage image={0};
        s=film_raw_grade_canvas("film-raw-grade/v1",&prepared,&recipe,i?lut:NULL,&canvas);
        if(s==FILM_OK)s=film_develop_canvas_render(&canvas,&recipe,options,false,&image);
        if(s==FILM_OK)s=film_jpeg_write(paths[i],&image,recipe.jpeg_quality);
        film_develop_canvas_free(&canvas);film_image_free(&image);
    }
    film_raw_prepared_free(&prepared);return s;
}
