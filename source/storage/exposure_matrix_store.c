#include "exposure_matrix.h"
#include "storage_internal.h"
#include "film_runtime_contract.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

enum { MATRIX_BYTES=88, MATRIX_VERSION=1 };
_Static_assert(sizeof(float)==4,"F3EM requires IEEE binary32");
static void put(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t get(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put_float(uint8_t *p,float v){uint32_t bits;memcpy(&bits,&v,4);put(p,bits);}
static float get_float(const uint8_t *p){uint32_t bits=get(p);float v;memcpy(&v,&bits,4);return v;}
static uint32_t crc32(const uint8_t *p,size_t n)
{
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((c&1)?UINT32_C(0xedb88320):0);}
    return ~c;
}
static FilmStatus matrix_path(char *path,size_t size,const char *dir,unsigned id)
{
    if(!path||!size||!dir||!*dir||!id||id>9999)return FILM_INVALID;
    size_t n=strlen(dir);while(n&&dir[n-1]=='/')--n;
    if(n>400)return FILM_INVALID;
    int written=snprintf(path,size,"%.*s/ROLL%04u.F3E",(int)n,dir,id);
    return written>0&&(size_t)written<size?FILM_OK:FILM_INVALID;
}
static bool valid(const FilmExposureMatrix *m)
{
    if(!m||!m->id||m->id>9999||m->exposure_mode>1||m->native_level< -5||m->native_level>5||
       !film_exposure_curve_valid(&m->neutral)||!film_exposure_curve_valid(&m->shot)||
       memcmp(&m->neutral,&(FilmExposureCurve){0},sizeof m->neutral)!=0||
       (m->fitted&&!m->preview_valid))return false;
    if(m->preview_valid){
        for(unsigned i=0;i<3;++i)if(!isfinite(m->preview_linear[i])||
            m->preview_linear[i]<.001f||m->preview_linear[i]>1||
            (i&&m->preview_linear[i]<m->preview_linear[i-1]))return false;
    } else if(m->fitted)return false;
    return true;
}
static void put_curve(uint8_t *p,FilmExposureCurve c)
{put_float(p,c.ev);put_float(p+4,c.shadows);put_float(p+8,c.midtones);
 put_float(p+12,c.highlights);put_float(p+16,c.shoulder);}
static FilmExposureCurve get_curve(const uint8_t *p)
{return (FilmExposureCurve){get_float(p),get_float(p+4),get_float(p+8),
                            get_float(p+12),get_float(p+16)};}
FilmStatus film_exposure_matrix_write(const char *source,const char *directory,const FilmExposureMatrix *m)
{
    if(!source||strcmp(source,"film-exposure-matrix-write/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    char path[512];FilmStatus s=matrix_path(path,sizeof path,directory,m?m->id:0);
    if(s!=FILM_OK||!valid(m))return FILM_INVALID;
    uint8_t b[MATRIX_BYTES]={'F','3','E','M'};
    put(b+4,MATRIX_VERSION);put(b+8,m->id);put(b+12,m->source_crc);
    put(b+16,m->exposure_mode);put(b+20,(uint32_t)(int32_t)m->native_level);
    put(b+24,m->preview_valid?1:0);
    for(unsigned i=0;i<3;++i)put_float(b+28+4*i,m->preview_linear[i]);
    put_curve(b+40,m->neutral);put_curve(b+60,m->shot);
    put(b+80,m->fitted?1:0);put(b+84,crc32(b,84));
    FilmWrite w={0};s=film_write_begin(&w,path);
    if(s!=FILM_OK)return s;
    if(fwrite(b,1,sizeof b,w.file)!=sizeof b)s=FILM_IO;
    return film_write_end(&w,s);
}
FilmStatus film_exposure_matrix_read(const char *source,const char *directory,unsigned id,
                                    uint32_t source_crc,FilmExposureMatrix *out)
{
    if(!source||strcmp(source,"film-exposure-matrix-read/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    char path[512];FilmStatus s=matrix_path(path,sizeof path,directory,id);
    if(s!=FILM_OK||!out)return FILM_INVALID;
    FILE *f=fopen(path,"rb");if(!f)return errno==ENOENT?FILM_IO:FILM_IO;
    uint8_t b[MATRIX_BYTES];
    bool complete=fread(b,1,sizeof b,f)==sizeof b&&fgetc(f)==EOF&&!ferror(f);
    if(fclose(f))complete=false;
    if(!complete||memcmp(b,"F3EM",4)||get(b+4)!=MATRIX_VERSION||
       get(b+8)!=id||get(b+12)!=source_crc||get(b+84)!=crc32(b,84))return FILM_CORRUPT;
    FilmExposureMatrix m={0};
    m.id=id;m.source_crc=source_crc;m.exposure_mode=get(b+16);
    m.native_level=(int32_t)get(b+20);
    if(get(b+24)>1||get(b+80)>1)return FILM_CORRUPT;
    m.preview_valid=get(b+24)!=0;m.fitted=get(b+80)!=0;
    for(unsigned i=0;i<3;++i)m.preview_linear[i]=get_float(b+28+4*i);
    m.neutral=get_curve(b+40);m.shot=get_curve(b+60);
    if(!valid(&m))return FILM_CORRUPT;
    *out=m;return FILM_OK;
}
