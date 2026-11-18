#include "roll.h"
#include "darkroom.h"
#include "layered_grain.h"
#include "film_runtime_contract.h"
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
    FilmFrame raw={.recipe=film_default_recipe()},native={.recipe=film_default_recipe()};
    assert(film_image_alloc(&raw.source,16,12,FILM_RAW10_RGGB)==FILM_OK);
    for(unsigned i=0;i<192;++i){raw.source.pixels[2*i]=50+i%30;raw.source.pixels[2*i+1]=i%4;}
    uint8_t before[384];memcpy(before,raw.source.pixels,sizeof before);
    assert(film_image_alloc(&native.source,16,12,FILM_YUYV)==FILM_OK);memset(native.source.pixels,128,384);
    FilmImage image={0};FilmRollPair pair={0};FilmRollSaveResult save;
    char original[512],jpeg[512],index[512];
    assert(film_roll_original_path(original,sizeof original,directory,1)==FILM_OK);
    snprintf(jpeg,sizeof jpeg,"%s/ROLL0001.JPG",directory);snprintf(index,sizeof index,"%s/ROLL0001.F3I",directory);
    assert(film_roll_capture_save("scheduler_decision:admitted",&raw,original,directory,1,&save)==FILM_STATE&&!save.written);
    assert(access(original,F_OK)!=0);
    assert(film_roll_capture_save("film-roll-capture-save/v1",&raw,original,directory,1,&save)==FILM_OK);
    assert(save.written&&save.verification_status==FILM_OK&&save.preview_status==FILM_STATE);
    assert(access(original,F_OK)==0&&access(index,F_OK)!=0&&access(jpeg,F_OK)!=0);
    assert(film_roll_jpeg_preview("film-roll-jpeg-preview/v1",directory,1,&pair,&image)==FILM_IO);
    assert(film_roll_decode("scheduler_decision:admitted",directory,directory,1,NULL,NULL)==FILM_STATE);
    assert(access(index,F_OK)!=0);
    FilmRawScene scene={0},direct={0},resident={0};FilmRecipe decoded_recipe;
    assert(film_roll_decode("film-roll-decode/v1",directory,directory,1,&resident,&decoded_recipe)==FILM_OK);
    assert(resident.pixels&&resident.width==16&&resident.height==12);
    assert(film_recipe_validate(&decoded_recipe)==FILM_OK);
    assert(film_roll_decode_load("unknown",directory,directory,1,&scene)==FILM_STATE&&!scene.pixels);
    assert(film_roll_decode_load("film-roll-decode-load/v1",directory,directory,1,&scene)==FILM_OK);
    assert(film_raw_frame_scene(&raw,NULL,&direct)==FILM_OK);
    assert(scene.width==16&&scene.height==12&&!memcmp(scene.pixels,direct.pixels,192*sizeof(FilmRgb)));
    assert(!memcmp(resident.pixels,direct.pixels,192*sizeof(FilmRgb)));
    assert(film_roll_decode("film-roll-decode/v1",directory,directory,1,&resident,&decoded_recipe)==FILM_INVALID);
    film_raw_scene_free(&resident);
    FilmDarkroom cached={0},fresh={0};
    raw.recipe.white_balance=(FilmWhiteBalance){4500,2,-1};raw.recipe.input_tone.value[FILM_TONE_EV]=.5f;
    FilmDevelopOptions options={.raw_shadows=.5f,.raw_shoulder=.75f};
    assert(film_darkroom_scene_preview(&cached,&scene,&raw.recipe,NULL,&options)==FILM_OK);
    assert(film_darkroom_preview(&fresh,&raw,NULL,&options)==FILM_OK);
    assert(cached.preview.width==fresh.preview.width&&cached.preview.height==fresh.preview.height);
    assert(!memcmp(cached.preview.pixels,fresh.preview.pixels,film_image_bytes(fresh.preview.width,fresh.preview.height,FILM_RGB24)));
    assert(!memcmp(cached.composition_view.pixels,fresh.composition_view.pixels,576));
    FilmRawIsp calibration=film_raw_isp_default();options.raw_isp=&calibration;
    assert(film_darkroom_scene_preview(&cached,&scene,&raw.recipe,NULL,&options)==FILM_STATE);
    film_darkroom_discard(&cached);film_darkroom_discard(&fresh);
    film_raw_scene_free(&scene);film_raw_scene_free(&direct);raw.recipe=film_default_recipe();
    FilmImage plain={0},edited={0};
    assert(film_roll_jpeg_preview("film-roll-jpeg-preview/v1",directory,1,NULL,&plain)==FILM_OK);
    raw.recipe.grain_amount=1;raw.recipe.sharpness=5;raw.recipe.white_balance=(FilmWhiteBalance){1000,9,-9};
    raw.recipe.input_tone.value[FILM_TONE_EV]=2;strcpy(raw.recipe.lut_id,"must-not-load.cube");
    assert(film_roll_pair_publish("film-roll-pair-publish/v1",&raw,NULL,directory,1)==FILM_OK);
    assert(film_roll_jpeg_preview("film-roll-jpeg-preview/v1",directory,1,NULL,&edited)==FILM_OK);
    assert(!memcmp(plain.pixels,edited.pixels,320*240*3));
    film_image_free(&plain);film_image_free(&edited);raw.recipe=film_default_recipe();
    char cache[512];snprintf(cache,sizeof cache,"%s/ROLL0001.F3C",directory);
    FILE *cache_file=fopen(cache,"r+b");assert(cache_file&&fseek(cache_file,80,SEEK_SET)==0);
    int byte=fgetc(cache_file);assert(byte!=EOF&&fseek(cache_file,80,SEEK_SET)==0);fputc(byte^1,cache_file);fclose(cache_file);
    assert(film_roll_decode_load("film-roll-decode-load/v1",directory,directory,1,&scene)==FILM_CORRUPT&&!scene.pixels);
    assert(film_roll_decode("film-roll-decode/v1",directory,directory,1,NULL,NULL)==FILM_OK);
    assert(film_roll_decode_load("film-roll-decode-load/v1",directory,directory,1,&scene)==FILM_OK);film_raw_scene_free(&scene);
    struct stat original_info;assert(stat(original,&original_info)==0);
    cache_file=fopen(original,"ab");assert(cache_file);fputc(0,cache_file);fclose(cache_file);
    assert(film_roll_decode_load("film-roll-decode-load/v1",directory,directory,1,&scene)==FILM_CORRUPT&&!scene.pixels);
    assert(truncate(original,original_info.st_size)==0);
    assert(film_roll_capture_save("film-roll-capture-save/v1",&raw,original,directory,1,&save)==FILM_STATE&&!save.written);
    assert(film_roll_pair_publish("scheduler_decision:admitted",&raw,NULL,directory,1)==FILM_STATE);
    assert(!film_runtime_source_valid("film-roll-native-raw-pair/v1"));
    char refused[512];snprintf(refused,sizeof refused,"%s/ROLL0002.DNG",directory);
    assert(film_roll_capture_save("film-roll-capture-save/v1",&native,refused,directory,2,&save)==FILM_UNSUPPORTED&&!save.written);
    assert(access(refused,F_OK)!=0);
    memcpy(refused+strlen(refused)-3,"F3S",3);
    assert(film_roll_capture_save("film-roll-capture-save/v1",&raw,refused,directory,2,&save)==FILM_INVALID&&!save.written);
    assert(access(refused,F_OK)!=0);
    uint16_t ids[]={1};notified=0;
    assert(film_roll_prepare_catalog("film-preview-catalog-prepare/v1",directory,directory,ids,1,cancel)==FILM_STATE);
    assert(film_roll_prepare_catalog("film-preview-catalog-prepare/v1",directory,directory,ids,1,note)==FILM_OK&&notified==1);
    assert(film_roll_jpeg_preview("film-roll-jpeg-preview/v1",directory,1,&pair,&image)==FILM_OK);
    assert(image.width==320&&image.height==240);film_image_free(&image);
    assert(film_roll_pair_verify("film-roll-original-open/v1",&raw,&pair)==FILM_OK);
    assert(film_roll_pair_verify("scheduler_decision:admitted",&raw,&pair)==FILM_STATE);
    raw.source.pixels[0]^=1;
    assert(film_roll_pair_verify("film-roll-original-open/v1",&raw,&pair)==FILM_CORRUPT);
    /* An existing valid JPG/index for an older source is rebuilt. */
    assert(film_dng_write("film-dng-write/v1",original,&raw)==FILM_OK);
    assert(film_roll_prepare_catalog("film-preview-catalog-prepare/v1",directory,directory,ids,1,note)==FILM_OK);
    assert(film_roll_pair_index("film-roll-pair-index/v1",directory,1,&pair)==FILM_OK);
    assert(film_roll_pair_verify("film-roll-original-open/v1",&raw,&pair)==FILM_OK);
    raw.source.pixels[0]^=1;assert(!memcmp(before,raw.source.pixels,sizeof before));
    assert(film_dng_write("film-dng-write/v1",original,&raw)==FILM_OK);
    FILE *f=fopen(jpeg,"wb");assert(f);fputs("bad",f);fclose(f);
    assert(film_roll_jpeg_preview("film-roll-jpeg-preview/v1",directory,1,&pair,&image)==FILM_CORRUPT&&!image.pixels);
    assert(film_roll_prepare_catalog("film-preview-catalog-prepare/v1",directory,directory,ids,1,note)==FILM_OK);
    assert(film_roll_jpeg_preview("film-roll-jpeg-preview/v1",directory,1,&pair,&image)==FILM_OK);film_image_free(&image);
    assert(unlink(jpeg)==0); /* Existing index alone must never count as ready. */
    assert(film_roll_prepare_catalog("film-preview-catalog-prepare/v1",directory,directory,ids,1,note)==FILM_OK);
    f=fopen(index,"wb");assert(f);fputs("bad",f);fclose(f);
    assert(film_roll_prepare_catalog("film-preview-catalog-prepare/v1",directory,directory,ids,1,note)==FILM_OK);
    assert(film_roll_pair_index("unknown",directory,1,&pair)==FILM_STATE);
    raw.source.pixels[1]=255;
    assert(film_roll_pair_publish("film-roll-pair-publish/v1",&raw,NULL,directory,1)==FILM_CORRUPT);
    raw.source.pixels[1]=before[1];
    assert(unlink(original)==0);
    assert(film_roll_capture_save("film-roll-capture-save/v1",&raw,original,"build/no-such-preview-directory",1,&save)==FILM_OK);
    assert(save.written&&save.verification_status==FILM_OK&&save.preview_status==FILM_STATE);
    FilmFrame saved={0};assert(film_frame_read(original,&saved)==FILM_OK&&!memcmp(before,saved.source.pixels,sizeof before));film_frame_free(&saved);
    assert(film_roll_capture_save("film-roll-capture-save/v1",&raw,"build/no-such-roll-directory/file.DNG",directory,1,&save)==FILM_IO&&!save.written);
    /* Browsing consumes only JPG/index, even when original is unavailable. */
    unlink(original);
    assert(film_roll_jpeg_preview("film-roll-jpeg-preview/v1",directory,1,&pair,&image)==FILM_OK);film_image_free(&image);
    assert(film_roll_prepare_catalog("film-preview-catalog-prepare/v1",directory,directory,ids,1,note)==FILM_IO);
    film_frame_free(&raw);film_frame_free(&native);unlink(jpeg);unlink(index);unlink(cache);assert(rmdir(directory)==0);
    puts("PASS RAW-only commit, YUYV/F3S rejection, no overwrite, deferred preview, explicit decode/cache parity, corrupt cache rejection, write failure, RAW repair, JPG/index CRC/binding validation, cancellation and unknown-source rejection");
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
