#include "roll.h"
#include "darkroom.h"
#include "layered_grain.h"
#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned notified;
static bool note(unsigned i,unsigned total,unsigned id){assert(i&&total&&id);++notified;return true;}
static bool cancel(unsigned i,unsigned total,unsigned id){(void)i;(void)total;(void)id;return false;}

static void preview(void)
{
    char directory[]="build/roll-pair.XXXXXX";int fd=mkstemp(directory);assert(fd>=0);close(fd);unlink(directory);assert(mkdir(directory,0700)==0);
    FilmFrame frame={.recipe=film_default_recipe()};assert(film_image_alloc(&frame.source,640,480,FILM_RAW10_RGGB)==FILM_OK);
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<640;++x){unsigned v=10+x+y/2;size_t i=((size_t)y*640+x)*2;frame.source.pixels[i]=v>>2;frame.source.pixels[i+1]=v&3;}
    uint8_t *before=malloc(614400);assert(before);memcpy(before,frame.source.pixels,614400);
    FilmImage image={0};FilmRollPair pair={0};
    assert(film_roll_pair_publish("scheduler_decision:admitted",&frame,directory,1)==FILM_STATE);
    assert(film_roll_jpeg_preview("film-roll-jpeg-preview/v1",directory,1,&pair,&image)==FILM_IO&&!image.pixels);
    assert(film_roll_pair_publish("film-roll-pair-publish/v1",&frame,directory,1)==FILM_UNSUPPORTED);
    FilmFrame native={.recipe=film_default_recipe()};assert(film_mock_capture(&native)==FILM_OK);
    native.camera=frame.camera;native.captured_at=frame.captured_at;
    assert(film_roll_native_raw_pair("unknown",&frame,&native,directory,1)==FILM_STATE);
    assert(film_roll_native_raw_pair("film-roll-native-raw-pair/v1",&frame,&frame,directory,1)==FILM_INVALID);
    assert(film_roll_pair_publish("film-roll-pair-publish/v1",&native,directory,2)==FILM_OK);
    FilmRollPair native_pair={0};assert(film_roll_pair_index("film-roll-pair-index/v1",directory,2,&native_pair)==FILM_OK);
    assert(film_roll_native_raw_pair("film-roll-native-raw-pair/v1",&frame,&native,directory,1)==FILM_OK);
    /* Invalid RAW low bytes must not enter RAW unpack/ISP in JPG generation. */
    frame.source.pixels[1]=255;
    assert(film_roll_native_raw_pair("film-roll-native-raw-pair/v1",&frame,&native,directory,3)==FILM_OK);
    FilmRollPair no_decode={0};assert(film_roll_pair_index("film-roll-pair-index/v1",directory,3,&no_decode)==FILM_OK);
    assert(no_decode.jpeg_crc==native_pair.jpeg_crc);
    frame.source.pixels[1]=before[1];
    char extra[512];for(unsigned id=2;id<=3;++id){snprintf(extra,sizeof extra,"%s/ROLL%04u.JPG",directory,id);unlink(extra);snprintf(extra,sizeof extra,"%s/ROLL%04u.F3I",directory,id);unlink(extra);}
    film_frame_free(&native);
    assert(!memcmp(before,frame.source.pixels,614400));
    assert(film_roll_pair_index("film-roll-pair-index/v1",directory,1,&pair)==FILM_OK&&pair.format==FILM_RAW10_RGGB);
    assert(film_roll_pair_verify("film-roll-original-open/v1",&frame,&pair)==FILM_OK);
    frame.source.pixels[0]^=1;assert(film_roll_pair_verify("film-roll-original-open/v1",&frame,&pair)==FILM_CORRUPT);frame.source.pixels[0]^=1;
    assert(film_roll_pair_verify("scheduler_decision:admitted",&frame,&pair)==FILM_STATE);
    uint16_t ids[]={1};
    assert(film_roll_prepare_catalog("film-preview-catalog-prepare/v1","missing-original-directory",directory,ids,1,note)==FILM_OK&&!notified);
    /* ROLL receives no original path or pixel buffer. It must work after the
     * source frame is released, with only the committed JPG/index present. */
    film_frame_free(&frame);
    assert(film_roll_jpeg_preview("film-roll-jpeg-preview/v1",directory,1,&pair,&image)==FILM_OK);
    assert(image.width==320&&image.height==240);film_image_free(&image);
    char jpeg[512],index[512];snprintf(jpeg,sizeof jpeg,"%s/ROLL0001.JPG",directory);snprintf(index,sizeof index,"%s/ROLL0001.F3I",directory);
    FILE *f=fopen(jpeg,"wb");assert(f);fputs("bad",f);fclose(f);
    assert(film_roll_jpeg_preview("film-roll-jpeg-preview/v1",directory,1,&pair,&image)==FILM_CORRUPT&&!image.pixels);
    f=fopen(jpeg,"rb");assert(f&&fgetc(f)=='b');fclose(f);
    assert(unlink(index)==0);
    assert(film_roll_jpeg_preview("film-roll-jpeg-preview/v1",directory,1,&pair,&image)==FILM_IO);
    assert(film_roll_prepare_catalog("film-preview-catalog-prepare/v1",directory,directory,ids,1,cancel)==FILM_STATE);
    assert(film_roll_pair_index("unknown",directory,1,&pair)==FILM_STATE);
    unlink(jpeg);
    frame=(FilmFrame){.recipe=film_default_recipe()};assert(film_image_alloc(&frame.source,16,12,FILM_RAW10_RGGB)==FILM_OK);memset(frame.source.pixels,0,384);
    char original[512];assert(film_roll_original_path(original,sizeof original,directory,1)==FILM_OK);assert(film_frame_write(original,&frame)==FILM_OK);
    assert(film_roll_prepare_catalog("film-preview-catalog-prepare/v1",directory,directory,ids,1,note)==FILM_UNSUPPORTED&&notified==1);
    assert(access(original,F_OK)==0&&access(index,F_OK)!=0);
    native=(FilmFrame){.recipe=film_default_recipe()};assert(film_image_alloc(&native.source,16,12,FILM_YUYV)==FILM_OK);
    memset(native.source.pixels,128,384);
    assert(film_roll_native_raw_pair("film-roll-native-raw-pair/v1",&frame,&native,directory,1)==FILM_OK);
    film_frame_free(&native);
    assert(film_roll_pair_index("film-roll-pair-index/v1",directory,1,&pair)==FILM_OK);
    assert(film_roll_pair_verify("film-roll-original-open/v1",&frame,&pair)==FILM_OK);
    film_frame_free(&frame);unlink(original);unlink(jpeg);unlink(index);
    assert(rmdir(directory)==0);free(before);
    puts("PASS capture-stage pair publication, JPEG-only ROLL without RAW, binding mismatch rejection, corrupt/missing pair fail-closed and no automatic generation");
}
static double range_min=0,range_max=0;
static FilmStatus quantized_mix(unsigned w,unsigned h,const float *planes,float *noise)
{
    size_t count=(size_t)w*h;float scale=0;
    assert(film_grain_gpu_encoding("film-grain-gpu-mix/v1",planes,count,&scale)==FILM_OK);
    for(size_t i=0;i<count;++i){unsigned sum=0;float expected=0;
        for(unsigned c=0;c<3;++c){float v=planes[3*i+c];range_min=fmin(range_min,v);range_max=fmax(range_max,v);
            float code=42+scale*v;assert(code>=1.99f&&code<=82.01f);sum+=(unsigned)lroundf(code);expected+=v;}
        assert(sum<=246);noise[i]=((int)sum-126)/scale;assert(fabsf(noise[i]-expected)<=.4f);
    }
    return FILM_OK;
}
static void yuyv_grain(void)
{
    FilmFrame frame={.recipe=film_default_recipe()};assert(film_mock_capture(&frame)==FILM_OK);assert(frame.source.format==FILM_YUYV);
    FilmGrainTexture texture={0};FILE *f=fopen("build/raw-darkroom-20261116/pan400.f3gt","rb");
    if(f){assert(film_grain_texture_load(f,&texture)==FILM_OK);fclose(f);}
    for(unsigned kind=0;kind<3;++kind){const char *names[]={"fine","medium","coarse"};char path[128];snprintf(path,sizeof path,"romfs/grain/%s.f3g",names[kind]);
        FilmGrainProfile profile;f=fopen(path,"rb");assert(f&&film_grain_profile_load(f,&profile)==FILM_OK);fclose(f);
        FilmDevelopOptions o={.layered_grain=true,.grain=(FilmGrain)kind,.grain_size=1,.grain_shadow=.8f,.grain_mid=1.1f,.grain_highlight=.7f,
            .grain_profile=&profile,.grain_renderer=film_grain_layered_texture,.grain_texture=kind==1&&texture.pixels?&texture:NULL};
        frame.recipe.grain_amount=.5f;FilmDarkroom room={0};
        FilmStatus ps=film_darkroom_preview(&room,&frame,NULL,&o);
        fprintf(stderr,"YUYV grain kind=%u status=%s size=%ux%u recipe=%s\n",kind,film_status_name(ps),frame.source.width,frame.source.height,film_status_name(film_recipe_validate(&frame.recipe)));
        assert(ps==FILM_OK);assert(room.preview.pixels);film_darkroom_discard(&room);
        FilmImage image={0};assert(film_rgb24(&frame.source,frame.recipe.yuv,&image)==FILM_OK);
        FilmLayeredGrain p={.amount=.5f,.size=1,.coarse=kind==0?.2f:kind==2?.8f:.5f,.shadow=.8f,.mid=1.1f,.highlight=.7f,.reference_width=image.width,.seed=UINT32_C(0x9e3779b8)};
        assert(film_grain_layered_mix("film-grain-layered/v1",&image,&p,&profile,o.grain_texture,quantized_mix)==FILM_OK);film_image_free(&image);
        if(kind==1)for(unsigned photo=2;photo<=4;++photo){
            assert(film_rgb24(&frame.source,frame.recipe.yuv,&image)==FILM_OK);
            p.seed=UINT32_C(1)^(UINT32_C(0x9e3779b9)*photo);
            assert(film_grain_layered_mix("film-grain-layered/v1",&image,&p,&profile,o.grain_texture,quantized_mix)==FILM_OK);film_image_free(&image);
        }
    }
    float tail[]={-7,2,1,6,-1,.2f};float scale=0;
    assert(film_grain_gpu_encoding("film-grain-gpu-mix/v1",tail,2,&scale)==FILM_OK&&scale<8);
    assert(film_grain_gpu_encoding("scheduler_decision:admitted",tail,2,&scale)==FILM_STATE);
    film_grain_texture_free(&texture);film_frame_free(&frame);
    printf("PASS YUYV actual darkroom Fine/Medium/Coarse grain and bounded GPU encoding; field range %.3f..%.3f\n",range_min,range_max);
}
int main(void){preview();yuyv_grain();return 0;}
