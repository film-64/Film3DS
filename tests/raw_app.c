#include "raw_capture.h"
#include "film_runtime_contract.h"
#include "raw_darkroom.h"
#include "layered_grain.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {uint16_t reg[65536],seq;unsigned writes,frames,retained,locks,unlocks;int fail_frame;bool fail_restore,wrong_hold,native_channels,fail_post_hold,fail_idle;uint16_t expected_coarse,expected_gain,lock_coarse,failed_address,failed_expected,failed_observed;} Sensor;
static FilmStatus rd(void *ctx,bool mcu,uint16_t a,uint16_t *v)
{Sensor *s=ctx;*v=mcu?s->seq:s->reg[a];if((s->wrong_hold&&s->writes&&a==0x3056)||(s->fail_post_hold&&s->retained&&a==0x3056))*v^=1;return FILM_OK;}
static FilmStatus wr(void *ctx,bool mcu,uint16_t a,uint16_t v)
{Sensor *s=ctx;++s->writes;if(s->fail_restore&&a==0x3006&&v==487)return FILM_DEVICE;if(mcu)s->seq=v;else s->reg[a]=v;return FILM_OK;}
static FilmStatus lock(void *ctx){Sensor *s=ctx;++s->locks;if(s->lock_coarse){s->reg[0x3012]=s->lock_coarse;s->expected_coarse=s->lock_coarse;}return FILM_OK;}
static FilmStatus unlock(void *ctx){++((Sensor *)ctx)->unlocks;return FILM_OK;}
static FilmStatus idle(void *ctx){return ((Sensor *)ctx)->fail_idle?FILM_DEVICE:FILM_OK;}
static FilmStatus frame(void *ctx,bool retained,bool raw)
{
    Sensor *s=ctx;++s->frames;if((int)s->frames==s->fail_frame)return FILM_DEVICE;
    if(raw){assert(s->reg[0x3006]==479&&s->reg[0x3008]==639);assert((s->reg[0x3330]&0x40)&&s->reg[0x3012]==s->expected_coarse);}
    else assert(s->reg[0x3006]==487&&s->reg[0x3008]==647);
    if(raw)assert(s->reg[0x3056]==s->expected_gain&&s->reg[0x3058]==s->expected_gain+(s->native_channels?7:0)&&s->reg[0x305a]==s->expected_gain+(s->native_channels?14:0)&&s->reg[0x305c]==s->expected_gain+(s->native_channels?25:0));
    if(retained){assert(raw);++s->retained;}return FILM_OK;
}
static void setup(Sensor *s)
{memset(s,0,sizeof *s);s->reg[0]=8832;s->reg[0x3006]=487;s->reg[0x3008]=647;s->reg[0x3040]=0x25;s->reg[0x300a]=704;s->seq=13;s->reg[0x3012]=s->expected_coarse=128;
    s->expected_gain=528;s->reg[0x3056]=s->reg[0x3058]=s->reg[0x305a]=s->reg[0x305c]=528;}
static void observe(void *ctx,const char *phase,uint16_t a,uint16_t expected,uint16_t actual,FilmStatus status)
{Sensor *s=ctx;if(!strcmp(phase,"verify_reg")&&status!=FILM_OK){s->failed_address=a;s->failed_expected=expected;s->failed_observed=actual;}}
static void transaction(void)
{
    Sensor *s=calloc(1,sizeof *s);assert(s);setup(s);Sensor before=*s;
    FilmRawCaptureIo io={s,rd,wr,lock,frame,idle,unlock,observe};
    assert(!film_raw_qa_token_valid(NULL)&&!film_raw_qa_token_valid("bad")&&!film_raw_qa_token_valid("0123456789abcdef0123456789abcdeg"));
    assert(film_raw_qa_token_valid("0123456789abcdef0123456789abcdef"));
    assert(film_raw_capture_transaction("scheduler_decision:admitted",io)==FILM_STATE);assert(!s->writes);
    assert(film_raw_capture_transaction("film-camera-raw-capture/v1",io)==FILM_OK);
    assert(s->frames==5&&s->retained==1&&s->locks==1&&s->unlocks==1);
    assert(!memcmp(s->reg,before.reg,sizeof s->reg)&&s->seq==before.seq);
    for(int fail=1;fail<=5;++fail){setup(s);s->fail_frame=fail;
        assert(film_raw_capture_transaction("film-camera-raw-capture/v1",io)==FILM_DEVICE);
        assert(!memcmp(s->reg,before.reg,sizeof s->reg)&&s->seq==before.seq);
    }
    setup(s);s->reg[0]=0;assert(film_raw_capture_transaction("film-camera-raw-capture/v1",io)==FILM_STATE);assert(!s->writes&&!s->frames);assert(s->failed_address==0&&s->failed_expected==8832&&s->failed_observed==0);
    setup(s);s->wrong_hold=true;assert(film_raw_capture_transaction("film-camera-raw-capture/v1",io)==FILM_STATE);assert(!s->retained);
    setup(s);s->fail_restore=true;assert(film_raw_capture_transaction("film-camera-raw-capture/v1",io)==FILM_STATE);
    setup(s);s->reg[0x3330]=0x40;assert(film_raw_capture_transaction("film-camera-raw-capture/v1",io)==FILM_STATE);assert(!s->writes);
    for(int step=FILM_HW_MIN;step<=FILM_HW_MAX;++step){setup(s);unsigned i=(unsigned)(step-FILM_HW_MIN);
        s->reg[0x3012]=s->expected_coarse=film_hw_coarse[i];s->expected_gain=film_hw_core[i];
        s->reg[0x3056]=s->reg[0x3058]=s->reg[0x305a]=s->reg[0x305c]=s->expected_gain;
        assert(film_raw_capture_transaction("film-camera-raw-capture/v1",io)==FILM_OK);
    }
    setup(s);s->lock_coarse=117;s->native_channels=true;s->expected_gain=s->reg[0x3056]=530;
    s->reg[0x3058]=537;s->reg[0x305a]=544;s->reg[0x305c]=555;
    assert(film_raw_capture_transaction("film-camera-raw-capture/v1",io)==FILM_OK&&s->reg[0x3012]==117);
    FilmRawCaptureResult result;
    setup(s);s->fail_restore=true;
    assert(film_raw_capture_transaction_result("film-camera-raw-capture/v1",io,&result)!=FILM_OK);
    assert(result.raw_received&&result.raw_valid&&!result.camera_restored&&result.capture_status==FILM_OK&&result.restore_status!=FILM_OK);
    setup(s);s->fail_frame=4;
    assert(film_raw_capture_transaction_result("film-camera-raw-capture/v1",io,&result)==FILM_DEVICE);
    assert(!result.raw_received&&!result.raw_valid&&result.camera_restored);
    setup(s);s->fail_post_hold=true;
    assert(film_raw_capture_transaction_result("film-camera-raw-capture/v1",io,&result)!=FILM_OK);
    assert(result.raw_received&&!result.raw_valid);
    setup(s);s->fail_idle=true;
    assert(film_raw_capture_transaction_result("film-camera-raw-capture/v1",io,&result)==FILM_DEVICE);
    assert(result.raw_valid&&!result.camera_restored&&result.restore_status==FILM_DEVICE);
    setup(s);
    assert(film_raw_capture_transaction_result("unknown",io,&result)==FILM_STATE);
    assert(!result.raw_received&&!result.raw_valid&&!result.camera_restored&&!s->writes);
    free(s);puts("PASS RAW transaction preconditions, gate, two settle frames, retention, reverse restore, errors and unknown-source rejection");
}
static FilmStatus sum_mix(unsigned w,unsigned h,const float *planes,float *noise)
{for(size_t i=0;i<(size_t)w*h;++i)noise[i]=planes[3*i]+planes[3*i+1]+planes[3*i+2];return FILM_OK;}
static FilmStatus fail_mix(unsigned w,unsigned h,const float *p,float *n){(void)w;(void)h;(void)p;(void)n;return FILM_DEVICE;}
static void denoise(void)
{
    FilmRgb rgb[25],before[25];for(unsigned i=0;i<25;++i)rgb[i]=(FilmRgb){.4f,.4f,.4f};rgb[12]=(FilmRgb){.8f,.25f,.7f};memcpy(before,rgb,sizeof rgb);
    assert(film_darkroom_denoise("film-darkroom-denoise/v1",rgb,5,5,0)==FILM_OK&&!memcmp(before,rgb,sizeof rgb));
    assert(film_darkroom_denoise("unknown",rgb,5,5,1)==FILM_STATE);
    assert(film_darkroom_denoise("film-darkroom-denoise/v1",rgb,5,5,1)==FILM_OK&&memcmp(before,rgb,sizeof rgb));
    for(unsigned i=0;i<25;++i){float a=.2126f*before[i].r+.7152f*before[i].g+.0722f*before[i].b;
        float b=.2126f*rgb[i].r+.7152f*rgb[i].g+.0722f*rgb[i].b;assert(fabsf(a-b)<1e-6f);}
    FilmFrame f={.recipe=film_default_recipe()};assert(film_image_alloc(&f.source,16,12,FILM_RAW10_RGGB)==FILM_OK);
    for(unsigned i=0;i<192;++i){f.source.pixels[2*i]=50;f.source.pixels[2*i+1]=0;}f.source.pixels[2*68]=250;
    uint8_t raw[384];memcpy(raw,f.source.pixels,sizeof raw);
    FilmRawPrepared base={0},zero={0},clean={0};FilmDevelopOptions options={0};
    assert(film_raw_frame_prepare(&f,NULL,&base)==FILM_OK);
    assert(film_raw_frame_prepare(&f,&options,&zero)==FILM_OK&&!memcmp(base.pixels,zero.pixels,192*sizeof(FilmRgb)));
    options.chroma_denoise=1;assert(film_raw_frame_prepare(&f,&options,&clean)==FILM_OK);
    assert(memcmp(base.pixels,clean.pixels,192*sizeof(FilmRgb))&&!memcmp(raw,f.source.pixels,sizeof raw));
    film_raw_prepared_free(&base);film_raw_prepared_free(&zero);film_raw_prepared_free(&clean);film_frame_free(&f);
    puts("PASS darkroom-only denoise, exact zero, luma preservation, explicit effect and immutable RAW source");
}
static void app(void)
{
    FilmFrame f={.recipe=film_default_recipe()};assert(film_image_alloc(&f.source,16,12,FILM_RAW10_RGGB)==FILM_OK);
    for(unsigned i=0;i<192;++i){unsigned v=100+i%100;f.source.pixels[2*i]=v>>2;f.source.pixels[2*i+1]=v&3;}
    f.recipe.grain_amount=.5f;uint8_t before[384];memcpy(before,f.source.pixels,384);
    char file[]="/tmp/filmraw-app-XXXXXX";int fd=mkstemp(file);assert(fd>=0);close(fd);unlink(file);
    assert(film_frame_write(file,&f)==FILM_OK);FilmFrame loaded={0};assert(film_frame_read(file,&loaded)==FILM_OK);
    assert(loaded.source.format==FILM_RAW10_RGGB&&!memcmp(before,loaded.source.pixels,384));unlink(file);
    FilmGrainProfile profile;FILE *pf=fopen("romfs/grain/medium.f3g","rb");assert(pf&&film_grain_profile_load(pf,&profile)==FILM_OK);fclose(pf);
    FilmDevelopOptions o={.layered_grain=true,.grain=FILM_GRAIN_MEDIUM,.grain_size=1,.grain_shadow=.8f,.grain_mid=1.1f,.grain_highlight=.7f,
        .grain_renderer=film_grain_layered_texture,.grain_profile=&profile};
    FilmImage image={0};assert(film_develop_with_options(&loaded,NULL,&o,&image)==FILM_OK);assert(image.width==16&&image.height==12);
    FilmImage reference={0};assert(film_image_alloc(&reference,16,12,FILM_RGB24)==FILM_OK);memcpy(reference.pixels,image.pixels,576);
    FilmLayeredGrain grain={.amount=.5f,.size=1,.coarse=.5f,.shadow=.8f,.mid=1.1f,.highlight=.7f,.reference_width=16,.seed=64};
    assert(film_grain_layered_mix("film-grain-layered/v1",&image,&grain,&profile,NULL,fail_mix)==FILM_DEVICE);
    assert(!memcmp(reference.pixels,image.pixels,576));
    assert(film_grain_layered_mix("film-grain-layered/v1",&image,&grain,&profile,NULL,sum_mix)==FILM_OK);
    assert(film_grain_layered_texture("film-grain-layered/v1",&reference,&grain,&profile,NULL)==FILM_OK);
    assert(!memcmp(reference.pixels,image.pixels,576));assert(!memcmp(before,f.source.pixels,384));
    char plain[128],look[128];snprintf(plain,sizeof plain,"%s-plain.jpg",file);snprintf(look,sizeof look,"%s-look.jpg",file);
    assert(film_raw_export_pair(&loaded,NULL,&o,plain,look)==FILM_OK);
    assert(film_raw_export_pair(&loaded,NULL,&o,plain,look)==FILM_STATE);unlink(plain);unlink(look);
    FilmLut lut={.size=17,.max={1,1,1},.samples=calloc(17*17*17,sizeof(FilmRgb))};assert(lut.samples);
    for(unsigned i=0;i<17*17*17;++i)lut.samples[i]=(FilmRgb){.1f,.2f,.7f};
    strcpy(loaded.recipe.lut_id,"explicit-test-look");loaded.recipe.profile.strength=1;
    assert(film_raw_export_pair(&loaded,&lut,&o,plain,look)==FILM_OK);
    FILE *a=fopen(plain,"rb"),*b=fopen(look,"rb");assert(a&&b);bool different=false;
    for(;;){int x=fgetc(a),y=fgetc(b);if(x!=y){different=true;break;}if(x==EOF)break;}
    assert(different);fclose(a);fclose(b);unlink(plain);unlink(look);film_lut_free(&lut);
    film_image_free(&reference);film_image_free(&image);film_frame_free(&loaded);film_frame_free(&f);
    uint8_t *wire=calloc(614400,1);assert(wire);for(unsigned i=0;i<307200;++i){wire[2*i]=50;wire[2*i+1]=i&3;}
    char bin[128];snprintf(bin,sizeof bin,"%s.bin",file);FILE *input=fopen(bin,"wb");assert(input&&fwrite(wire,1,614400,input)==614400);fclose(input);
    assert(film_raw_import("startup_warmup",bin,file)==FILM_STATE);
    assert(film_raw_import("film-app-raw-import/v1",bin,file)==FILM_OK);
    assert(film_raw_import("film-app-raw-import/v1",bin,file)==FILM_STATE);
    assert(film_frame_read(file,&loaded)==FILM_OK&&!memcmp(loaded.source.pixels,wire,614400));film_frame_free(&loaded);unlink(file);
    wire[9]=4;input=fopen(bin,"wb");assert(input&&fwrite(wire,1,614400,input)==614400);fclose(input);
    assert(film_raw_import("film-app-raw-import/v1",bin,file)==FILM_CORRUPT);assert(access(file,F_OK)!=0);
    unlink(bin);free(wire);
    puts("PASS app RAW DNG roundtrip, actual darkroom path, pair export/no-overwrite, density mixer equivalence and failure atomicity");
}
int main(void){transaction();denoise();app();return 0;}
