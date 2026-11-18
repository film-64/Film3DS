#include "roll.h"
#include "film_runtime_contract.h"
#include "storage_internal.h"
#include "exposure_matrix.h"
#include "gray_capture.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Local, versioned, IEEE float32 little-endian scene cache. No recipe,
 * grading, clipping or display transfer is baked into the cached pixels. */
enum {CACHE_HEADER=64, CACHE_BLOCK=4092};
_Static_assert(sizeof(float)==4&&FLT_RADIX==2&&FLT_MANT_DIG==24,"scene cache requires binary32");
static bool source_is(const char *source,const char *expected)
{return source&&!strcmp(source,expected)&&film_runtime_source_valid(source);}
static uint32_t crc(uint32_t c,const uint8_t *p,size_t n)
{for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((c&1)?UINT32_C(0xedb88320):0);}return c;}
static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t get(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put64(uint8_t *p,uint64_t v){put(p,(uint32_t)v);put(p+4,(uint32_t)(v>>32));}
static uint64_t get64(const uint8_t *p){return get(p)|(uint64_t)get(p+4)<<32;}
static FilmStatus cache_path(char *name,size_t size,const char *directory,unsigned id)
{
    if(!directory||!*directory||!id||id>9999)return FILM_INVALID;
    size_t n=strlen(directory);while(n&&directory[n-1]=='/')--n;
    if(n>400)return FILM_INVALID;
    int length=snprintf(name,size,"%.*s/ROLL%04u.F3C",(int)n,directory,id);
    return length>=0&&(size_t)length<size?FILM_OK:FILM_INVALID;
}
static FilmStatus original_stat(const char *directory,unsigned id,struct stat *st,char name[512])
{
    FilmStatus s=film_roll_original_path(name,512,directory,id);
    if(s!=FILM_OK)return s;
    return !stat(name,st)&&S_ISREG(st->st_mode)?FILM_OK:FILM_IO;
}
static bool same_original(const struct stat *a,const struct stat *b)
{return a->st_size==b->st_size&&a->st_mtime==b->st_mtime;}

static FilmStatus write_cache(const FilmFrame *frame,const FilmRawScene *scene,
    const struct stat *original,const char *directory,unsigned id)
{
    char name[512];FilmStatus s=cache_path(name,sizeof name,directory,id);
    if(s!=FILM_OK)return s;
    FilmWrite write;s=film_write_begin(&write,name);if(s!=FILM_OK)return s;
    uint8_t header[CACHE_HEADER]={'F','3','S','C'},block[CACHE_BLOCK];
    put(header+4,FILM_RAW_SCENE_CACHE_VERSION);put(header+8,id);put(header+12,scene->width);put(header+16,scene->height);
    put(header+20,film_roll_source_crc(&frame->source));
    put64(header+24,(uint64_t)original->st_size);put64(header+32,(uint64_t)original->st_mtime);
    if(fwrite(header,1,sizeof header,write.file)!=sizeof header)s=FILM_IO;
    uint32_t sum=UINT32_MAX;size_t count=(size_t)scene->width*scene->height;
    for(size_t first=0;s==FILM_OK&&first<count;){
        size_t n=count-first;if(n>CACHE_BLOCK/12)n=CACHE_BLOCK/12;
        for(size_t i=0;i<n;++i){
            FilmRgb a=scene->pixels[first+i];float values[3]={a.r,a.g,a.b};
            for(unsigned c=0;c<3;++c){uint32_t bits;memcpy(&bits,values+c,4);put(block+i*12+c*4,bits);}
        }
        sum=crc(sum,block,n*12);
        if(fwrite(block,12,n,write.file)!=n)s=FILM_IO;
        first+=n;
    }
    put(header+40,~sum);put(header+60,~crc(UINT32_MAX,header,60));
    if(s==FILM_OK&&(fseek(write.file,0,SEEK_SET)||fwrite(header,1,sizeof header,write.file)!=sizeof header))s=FILM_IO;
    return film_write_end(&write,s);
}

FilmStatus film_roll_decode(const char *source,const char *originals,const char *previews,unsigned id,FilmRawScene *out,FilmRecipe *recipe)
{
    if(!source_is(source,"film-roll-decode/v1"))return FILM_STATE;
    if((out&&out->pixels)||(recipe&&!out))return FILM_INVALID;
    char name[512];struct stat before,after;
    FilmStatus s=original_stat(originals,id,&before,name);
    FilmFrame frame={0};FilmRawScene scene={0};
    if(s==FILM_OK)s=film_frame_read(name,&frame); /* DNG digest / legacy CRC before decode. */
    if(s==FILM_OK&&frame.source.format==FILM_RAW10_RGGB){
        s=film_raw_frame_scene(&frame,NULL,&scene);
        if(s==FILM_OK)s=original_stat(originals,id,&after,name);
        if(s==FILM_OK&&!same_original(&before,&after))s=FILM_CORRUPT;
        /* Fit only at explicit Roll decode, never in the shutter worker.
         * F3EM is optional derived metadata; older RAWs stay decodable.
         * No image data or original DNG is written by this operation. */
        if(s==FILM_OK){
            FilmExposureMatrix matrix={0};
            FilmStatus metadata=film_exposure_matrix_read("film-exposure-matrix-read/v1",previews,id,
                film_roll_source_crc(&frame.source),&matrix);
            /* A gray diagnostic is deliberately unfit until its Display-gray
             * calibration and paired YUV have been measured together.
             * Never fill its F3E with an uncalibrated neutral-scene guess. */
            FilmStatus gray=film_gray_pair_verify("film-gray-pair-verify/v1",
                previews,id,film_roll_source_crc(&frame.source));
            if(metadata==FILM_OK && gray==FILM_IO &&
               matrix.preview_valid && !matrix.fitted &&
               film_exposure_matrix_fit("film-exposure-matrix-fit/v1",&matrix,&scene)==FILM_OK)
                (void)film_exposure_matrix_write("film-exposure-matrix-write/v1",previews,&matrix);
        }
        if(s==FILM_OK)s=write_cache(&frame,&scene,&before,previews,id);
        if(s==FILM_OK)s=film_roll_pair_publish(source,&frame,&scene,previews,id);
    }else if(s==FILM_OK){
        /* Legacy native originals retain their established decode path. */
        s=film_roll_pair_publish("film-roll-decode/v1",&frame,NULL,previews,id);
    }
    if(s==FILM_OK&&out){*out=scene;scene=(FilmRawScene){0};if(recipe)*recipe=frame.recipe;}
    film_raw_scene_free(&scene);film_frame_free(&frame);return s;
}

FilmStatus film_roll_decode_load(const char *source,const char *originals,const char *previews,unsigned id,FilmRawScene *out)
{
    if(!source_is(source,"film-roll-decode-load/v1"))return FILM_STATE;
    if(!out||out->pixels)return FILM_INVALID;
    char name[512];struct stat st;FilmRollPair pair={0};
    FilmStatus s=original_stat(originals,id,&st,name);
    if(s==FILM_OK)s=film_roll_pair_index("film-roll-pair-index/v1",previews,id,&pair);
    if(s!=FILM_OK)return s;
    if(pair.format!=FILM_RAW10_RGGB)return FILM_UNSUPPORTED;
    s=cache_path(name,sizeof name,previews,id);if(s!=FILM_OK)return s;
    FILE *f=fopen(name,"rb");if(!f)return FILM_IO;
    uint8_t header[CACHE_HEADER],block[CACHE_BLOCK];
    if(fread(header,1,sizeof header,f)!=sizeof header)s=FILM_CORRUPT;
    if(s==FILM_OK&&(memcmp(header,"F3SC",4)||get(header+4)!=FILM_RAW_SCENE_CACHE_VERSION||get(header+8)!=id||
       get(header+12)!=pair.width||get(header+16)!=pair.height||get(header+20)!=pair.source_crc||
       get64(header+24)!=(uint64_t)st.st_size||get64(header+32)!=(uint64_t)st.st_mtime||
       get(header+60)!=~crc(UINT32_MAX,header,60)))s=FILM_CORRUPT;
    size_t count=(size_t)pair.width*pair.height;FilmRgb *pixels=NULL;
    if(s==FILM_OK){pixels=malloc(count*sizeof *pixels);if(!pixels)s=FILM_NOMEM;}
    uint32_t sum=UINT32_MAX;
    for(size_t first=0;s==FILM_OK&&first<count;){
        size_t n=count-first;if(n>CACHE_BLOCK/12)n=CACHE_BLOCK/12;
        if(fread(block,12,n,f)!=n){s=FILM_CORRUPT;break;}
        sum=crc(sum,block,n*12);
        for(size_t i=0;i<n;++i){float values[3];
            for(unsigned c=0;c<3;++c){uint32_t bits=get(block+i*12+c*4);memcpy(values+c,&bits,4);if(!isfinite(values[c]))s=FILM_CORRUPT;}
            pixels[first+i]=(FilmRgb){values[0],values[1],values[2]};
        }
        first+=n;
    }
    if(s==FILM_OK&&(get(header+40)!=~sum||fgetc(f)!=EOF||ferror(f)))s=FILM_CORRUPT;
    if(fclose(f)&&s==FILM_OK)s=FILM_IO;
    if(s==FILM_OK){
        struct stat after;
        s=original_stat(originals,id,&after,name);
        if(s==FILM_OK&&!same_original(&st,&after))s=FILM_CORRUPT;
    }
    if(s==FILM_OK)*out=(FilmRawScene){pair.width,pair.height,pixels};else free(pixels);
    return s;
}
