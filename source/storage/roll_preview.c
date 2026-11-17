#include "roll.h"
#include "native_decode.h"
#include "film_runtime_contract.h"
#include "storage_internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum {INDEX_BYTES=44,JPEG_LIMIT=256*1024};
static bool source_is(const char *s,const char *expected)
{return s&&!strcmp(s,expected)&&film_runtime_source_valid(s);}
static uint32_t update(uint32_t crc,const uint8_t *data,size_t n)
{for(size_t i=0;i<n;++i){crc^=data[i];for(unsigned b=0;b<8;++b)crc=(crc>>1)^((crc&1)?UINT32_C(0xedb88320):0);}return crc;}
static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t get(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static FilmStatus path(char *out,size_t capacity,const char *directory,unsigned id,const char *extension)
{
    if(!directory||!*directory||!id||id>9999)return FILM_INVALID;
    size_t n=strlen(directory);while(n&&directory[n-1]=='/')--n;if(n>400)return FILM_INVALID;
    int written=snprintf(out,capacity,"%.*s/ROLL%04u.%s",(int)n,directory,id,extension);
    return written>=0&&(size_t)written<capacity?FILM_OK:FILM_INVALID;
}
static bool shape(unsigned w,unsigned h,FilmPixelFormat format)
{return w&&h&&(size_t)w<=FILM_RAW_MAX_PIXELS/h&&film_image_bytes(w,h,format);}
static uint32_t fingerprint(const FilmImage *image)
{
    uint8_t dimensions[12];put(dimensions,image->width);put(dimensions+4,image->height);put(dimensions+8,image->format);
    return ~update(update(UINT32_MAX,dimensions,sizeof dimensions),image->pixels,film_image_bytes(image->width,image->height,image->format));
}
static FilmStatus encoded(const char *name,uint8_t **data,size_t *bytes)
{
    FILE *f=fopen(name,"rb");if(!f)return FILM_IO;
    uint8_t *p=malloc(JPEG_LIMIT);if(!p){fclose(f);return FILM_NOMEM;}
    size_t n=fread(p,1,JPEG_LIMIT,f);bool ok=!ferror(f)&&fgetc(f)==EOF;if(fclose(f))ok=false;
    if(!ok){free(p);return FILM_CORRUPT;}*data=p;*bytes=n;return FILM_OK;
}
FilmStatus film_roll_pair_index(const char *source,const char *directory,unsigned id,FilmRollPair *out)
{
    if(!source_is(source,"film-roll-pair-index/v1"))return FILM_STATE;
    char name[512];if(!out||path(name,sizeof name,directory,id,"F3I")!=FILM_OK)return FILM_INVALID;
    FILE *f=fopen(name,"rb");if(!f)return FILM_IO;uint8_t b[INDEX_BYTES];
    bool ok=fread(b,1,sizeof b,f)==sizeof b&&fgetc(f)==EOF&&!ferror(f);if(fclose(f))ok=false;
    if(!ok||memcmp(b,"F3RI",4)||get(b+4)!=1||get(b+8)!=id||get(b+32)!=320||get(b+36)!=240||get(b+40)!=~update(UINT32_MAX,b,40))return FILM_CORRUPT;
    FilmRollPair pair={.id=id,.format=(FilmPixelFormat)get(b+12),.width=get(b+16),.height=get(b+20),.source_crc=get(b+24),.jpeg_crc=get(b+28)};
    if(!shape(pair.width,pair.height,pair.format))return FILM_CORRUPT;
    *out=pair;return FILM_OK;
}
FilmStatus film_roll_jpeg_preview(const char *source,const char *directory,unsigned id,FilmRollPair *pair_out,FilmImage *out)
{
    if(!source_is(source,"film-roll-jpeg-preview/v1"))return FILM_STATE;
    if(!out||out->pixels)return FILM_INVALID;
    FilmRollPair pair={0};FilmStatus s=film_roll_pair_index("film-roll-pair-index/v1",directory,id,&pair);
    if(s!=FILM_OK)return s;
    char name[512];s=path(name,sizeof name,directory,id,"JPG");uint8_t *data=NULL;size_t bytes=0;
    if(s==FILM_OK)s=encoded(name,&data,&bytes);
    if(s==FILM_OK&&pair.jpeg_crc!=~update(UINT32_MAX,data,bytes))s=FILM_CORRUPT;
    FilmNativeMedia media={0};
    if(s==FILM_OK)s=film_native_media_inspect("film-native-media-inspect/v1",data,bytes,&media);
    if(s==FILM_OK&&(media.count!=1||media.images[0].width!=320||media.images[0].height!=240))s=FILM_CORRUPT;
    if(s==FILM_OK)s=film_image_alloc(out,320,240,FILM_RGB24);
    unsigned w=0,h=0;
    if(s==FILM_OK)s=film_native_jpeg_decode("film-native-jpeg-decode/v1",data,bytes,0,out->pixels,960,320*240*3,&w,&h);
    free(data);
    if(s==FILM_OK){if(pair_out)*pair_out=pair;}else film_image_free(out);
    return s;
}
static FilmStatus publish(const FilmFrame *frame,const FilmFrame *native,const char *directory,unsigned id)
{
    if(!frame||!frame->source.pixels||!shape(frame->source.width,frame->source.height,frame->source.format))return FILM_INVALID;
    char jpeg[512],index[512];FilmStatus s=path(jpeg,sizeof jpeg,directory,id,"JPG");
    if(s==FILM_OK)s=path(index,sizeof index,directory,id,"F3I");
    if(s!=FILM_OK)return s;
    FilmFrame neutral=*native;neutral.recipe=film_default_recipe();
    neutral.recipe.yuv=native->recipe.yuv;neutral.recipe.base_saturation=native->recipe.base_saturation;
    FilmImage full={0},small={0};s=film_develop_with_options(&neutral,NULL,NULL,&full);
    if(s==FILM_OK)s=film_resize(&full,320,240,FILM_BILINEAR,&small);
    film_image_free(&full);
    if(s==FILM_OK)s=film_jpeg_write(jpeg,&small,90);
    film_image_free(&small);if(s!=FILM_OK)return s;
    uint8_t *data=NULL;size_t bytes=0;s=encoded(jpeg,&data,&bytes);if(s!=FILM_OK)return s;
    uint32_t jpeg_crc=~update(UINT32_MAX,data,bytes);free(data);
    uint8_t b[INDEX_BYTES]={'F','3','R','I'};put(b+4,1);put(b+8,id);put(b+12,frame->source.format);
    put(b+16,frame->source.width);put(b+20,frame->source.height);put(b+24,fingerprint(&frame->source));
    put(b+28,jpeg_crc);put(b+32,320);put(b+36,240);put(b+40,~update(UINT32_MAX,b,40));
    FilmWrite write;s=film_write_begin(&write,index);if(s!=FILM_OK)return s;
    if(fwrite(b,1,sizeof b,write.file)!=sizeof b)s=FILM_IO;
    return film_write_end(&write,s);
}
FilmStatus film_roll_pair_publish(const char *source,const FilmFrame *frame,const char *directory,unsigned id)
{
    if(!source_is(source,"film-roll-pair-publish/v1"))return FILM_STATE;
    if(!frame)return FILM_INVALID;
    if(frame->source.format==FILM_RAW10_RGGB)return FILM_UNSUPPORTED;
    return publish(frame,frame,directory,id);
}
FilmStatus film_roll_native_raw_pair(const char *source,const FilmFrame *raw,const FilmFrame *native,const char *directory,unsigned id)
{
    if(!source_is(source,"film-roll-native-raw-pair/v1"))return FILM_STATE;
    if(!raw||!native||raw==native||!native->source.pixels||raw->source.format!=FILM_RAW10_RGGB||native->source.format!=FILM_YUYV||
        raw->source.width!=native->source.width||raw->source.height!=native->source.height||
        raw->camera!=native->camera||raw->captured_at!=native->captured_at)return FILM_INVALID;
    return publish(raw,native,directory,id);
}
FilmStatus film_roll_pair_verify(const char *source,const FilmFrame *frame,const FilmRollPair *pair)
{
    if(!source_is(source,"film-roll-original-open/v1"))return FILM_STATE;
    if(!frame||!pair||!frame->source.pixels||!shape(frame->source.width,frame->source.height,frame->source.format))return FILM_INVALID;
    return pair->format==frame->source.format&&pair->width==frame->source.width&&pair->height==frame->source.height&&
        pair->source_crc==fingerprint(&frame->source)?FILM_OK:FILM_CORRUPT;
}

FilmStatus film_roll_prepare_catalog(const char *source,const char *originals,const char *previews,const uint16_t *ids,unsigned count,
    bool (*notify)(unsigned,unsigned,unsigned))
{
    if(!source_is(source,"film-preview-catalog-prepare/v1"))return FILM_STATE;
    if(!originals||!previews||(!ids&&count)||count>9999)return FILM_INVALID;
    for(unsigned i=0;i<count;++i){
        if(!ids[i]||ids[i]>9999||(i&&ids[i]<=ids[i-1]))return FILM_INVALID;
    }
    for(unsigned i=0;i<count;++i){
        FilmRollPair pair={0};errno=0;
        FilmStatus status=film_roll_pair_index("film-roll-pair-index/v1",previews,ids[i],&pair);
        if(status==FILM_OK)continue;
        if(status!=FILM_IO||errno!=ENOENT)return status;
        if(notify&&!notify(i+1,count,ids[i]))return FILM_STATE;
        FilmFrame frame={0};char name[512];status=film_roll_original_path(name,sizeof name,originals,ids[i]);
        if(status==FILM_OK)status=film_frame_read(name,&frame);
        if(status==FILM_OK)status=film_roll_pair_publish("film-roll-pair-publish/v1",&frame,previews,ids[i]);
        film_frame_free(&frame);if(status!=FILM_OK)return status;
    }
    return FILM_OK;
}
