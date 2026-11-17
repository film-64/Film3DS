#include "raw_darkroom.h"
#include "film_runtime_contract.h"
#include <math.h>
#include <string.h>
/* Calibration is explicit and separate from portable recipes. Fixed ordered
 * keys, no optional hidden fields, no fallback on malformed calibration. */
FilmStatus film_raw_profile_load(const char *source,FILE *file,FilmRawIsp *out)
{
    if(!source||strcmp(source,"film-raw-profile-load/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!file||!out)return FILM_INVALID;
    long start=ftell(file);if(start<0||fseek(file,0,SEEK_END))return FILM_CORRUPT;
    long end=ftell(file);if(end<start||end-start>4096||fseek(file,start,SEEK_SET))return FILM_CORRUPT;
    FilmRawIsp p=film_raw_isp_default();char word[32];
    if(fscanf(file,"%31s",word)!=1||strcmp(word,"F3ISP1"))return FILM_CORRUPT;
    const char *keys[]={"black","white","wb","ccm","ccm_offset","shading_r","shading_g","shading_b"};
    float *values[]={p.black,&p.white,p.wb,p.ccm,p.ccm_offset,p.lens_shading[0],p.lens_shading[1],p.lens_shading[2]};
    const unsigned counts[]={4,1,3,9,3,6,6,6};
    for(unsigned k=0;k<8;++k){
        if(fscanf(file,"%31s",word)!=1||strcmp(word,keys[k]))return FILM_CORRUPT;
        for(unsigned i=0;i<counts[k];++i)if(fscanf(file,"%f",&values[k][i])!=1||!isfinite(values[k][i]))return FILM_CORRUPT;
    }
    if(fscanf(file,"%31s",word)!=EOF||ferror(file))return FILM_CORRUPT;
    if(p.white<1||p.white>1023)return FILM_CORRUPT;
    for(unsigned i=0;i<4;++i)if(p.black[i]<0||p.black[i]>p.white-1)return FILM_CORRUPT;
    for(unsigned i=0;i<3;++i)if(p.wb[i]<.125f||p.wb[i]>8||fabsf(p.ccm_offset[i])>1)return FILM_CORRUPT;
    for(unsigned i=0;i<9;++i)if(fabsf(p.ccm[i])>8)return FILM_CORRUPT;
    for(unsigned c=0;c<3;++c)for(unsigned i=0;i<6;++i)if(fabsf(p.lens_shading[c][i])>4)return FILM_CORRUPT;
    *out=p;return FILM_OK;
}
