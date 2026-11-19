#include "gray_capture.h"
#include "film_runtime_contract.h"
#include "storage_internal.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum { HEADER=80, LEGACY_HEADER=64, WIDTH=640, HEIGHT=480, FRAME=WIDTH*HEIGHT*2, CHUNK=4096 };
static bool admitted(const char *source,const char *name)
{return source&&!strcmp(source,name)&&film_runtime_source_valid(source);}
static void put(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t get(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t update(uint32_t c,const uint8_t *p,size_t bytes)
{for(size_t i=0;i<bytes;++i){c^=p[i];for(unsigned bit=0;bit<8;++bit)c=(c>>1)^((c&1)?UINT32_C(0xedb88320):0);}return c;}
static FilmStatus name(char out[512],const char *directory,unsigned id)
{
    if(!directory||!*directory||id<1||id>9999)return FILM_INVALID;
    size_t n=strlen(directory);while(n&&directory[n-1]=='/')--n;
    if(!n||n>400)return FILM_INVALID;
    int length=snprintf(out,512,"%.*s/ROLL%04u.F3Y",(int)n,directory,id);
    return length>0&&length<512?FILM_OK:FILM_INVALID;
}
FilmStatus film_gray_pair_write(const char *source,const char *directory,const FilmGrayPair *p)
{
    if(!admitted(source,"film-gray-pair-write/v1"))return FILM_STATE;
    char path[512];
    if(!p||name(path,directory,p->id)!=FILM_OK||!p->preview.pixels||
       p->preview.width!=WIDTH||p->preview.height!=HEIGHT||p->preview.format!=FILM_YUYV||
       (unsigned)p->yuv>FILM_709_LIMITED||p->exposure_mode>1||
       p->native_level< -5||p->native_level>5||!p->preview_time_ms||
       !film_ae_region_valid(p->meter_region))return FILM_INVALID;
    struct stat old;
    if(!stat(path,&old))return FILM_STATE; /* Never replace acquired evidence. */
    if(errno!=ENOENT)return FILM_IO;
    uint8_t b[HEADER]={'F','3','Y','G'};
    uint32_t data_crc=~update(UINT32_MAX,p->preview.pixels,FRAME);
    put(b+4,2);put(b+8,p->id);put(b+12,p->raw_source_crc);
    put(b+16,WIDTH);put(b+20,HEIGHT);put(b+24,(uint32_t)p->yuv);
    put(b+28,(uint32_t)(int32_t)p->native_level);put(b+32,p->exposure_mode);
    put(b+36,p->native_ae?1:0);put(b+40,p->native_awb?1:0);
    put(b+44,(uint32_t)p->preview_time_ms);put(b+48,(uint32_t)(p->preview_time_ms>>32));
    put(b+52,FRAME);put(b+56,data_crc);
    put(b+60,p->meter_region.x);put(b+64,p->meter_region.y);
    put(b+68,p->meter_region.width);put(b+72,p->meter_region.height);
    put(b+76,~update(UINT32_MAX,b,76));
    FilmWrite w={0};FilmStatus s=film_write_begin(&w,path);
    if(s!=FILM_OK)return s;
    if(fwrite(b,1,HEADER,w.file)!=HEADER||fwrite(p->preview.pixels,1,FRAME,w.file)!=FRAME)
        s=FILM_IO;
    return film_write_end(&w,s);
}
FilmStatus film_gray_pair_verify(const char *source,const char *directory,unsigned id,uint32_t raw_crc)
{
    if(!admitted(source,"film-gray-pair-verify/v1"))return FILM_STATE;
    char path[512];FilmStatus s=name(path,directory,id);if(s!=FILM_OK)return s;
    FILE *f=fopen(path,"rb");if(!f)return FILM_IO;
    uint8_t b[HEADER]={0},block[CHUNK];
    bool valid=fread(b,1,LEGACY_HEADER,f)==LEGACY_HEADER;
    unsigned version=valid?get(b+4):0;
    if(valid&&version==2)valid=fread(b+LEGACY_HEADER,1,HEADER-LEGACY_HEADER,f)==HEADER-LEGACY_HEADER;
    if(valid)valid=!memcmp(b,"F3YG",4)&&(version==1||version==2)&&get(b+8)==id&&
        get(b+12)==raw_crc&&get(b+16)==WIDTH&&get(b+20)==HEIGHT&&
        get(b+24)<=FILM_709_LIMITED&&(int32_t)get(b+28)>=-5&&(int32_t)get(b+28)<=5&&
        get(b+32)<=1&&get(b+36)<=1&&get(b+40)<=1&&
        (get(b+44)||get(b+48))&&get(b+52)==FRAME;
    if(valid&&version==2)
        valid=film_ae_region_valid((FilmAeRegion){get(b+60),get(b+64),get(b+68),get(b+72)})&&
            get(b+76)==~update(UINT32_MAX,b,76);
    if(valid&&version==1)valid=get(b+60)==~update(UINT32_MAX,b,60);
    uint32_t digest=UINT32_MAX;
    if(valid)for(unsigned pos=0;pos<FRAME;){
        unsigned n=FRAME-pos;if(n>CHUNK)n=CHUNK;
        if(fread(block,1,n,f)!=n){valid=false;break;}
        digest=update(digest,block,n);pos+=n;
    }
    if(valid)valid=~digest==get(b+56)&&fgetc(f)==EOF&&!ferror(f);
    if(fclose(f))valid=false;
    return valid?FILM_OK:FILM_CORRUPT;
}
