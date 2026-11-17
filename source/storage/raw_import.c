#include "raw_darkroom.h"
#include "film_runtime_contract.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

FilmStatus film_raw_import(const char *source,const char *input,const char *output)
{
    if(!source||strcmp(source,"film-app-raw-import/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!input||!output)return FILM_INVALID;
    struct stat st;if(!stat(output,&st)||errno!=ENOENT)return FILM_STATE;
    FilmFrame frame={.recipe=film_default_recipe(),.camera=1};
    FilmStatus s=film_image_alloc(&frame.source,640,480,FILM_RAW10_RGGB);
    if(s!=FILM_OK)return s;
    FILE *f=fopen(input,"rb");
    if(!f){film_frame_free(&frame);return FILM_IO;}
    bool ok=fread(frame.source.pixels,1,614400,f)==614400&&fgetc(f)==EOF&&!ferror(f);
    if(fclose(f))ok=false;
    if(!ok)s=FILM_CORRUPT;
    if(s==FILM_OK)for(size_t i=1;i<614400;i+=2)if(frame.source.pixels[i]>3){s=FILM_CORRUPT;break;}
    frame.recipe.grain_amount=.35f;
    if(s==FILM_OK)s=film_dng_write("film-dng-write/v1",output,&frame);
    film_frame_free(&frame);return s;
}
