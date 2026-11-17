#include "raw_darkroom.h"
#include "layered_grain.h"
#include "film_runtime_contract.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static float encoded(float x) {return x<=.0031308f?12.92f*x:1.055f*powf(x,1/2.4f)-.055f;}
static unsigned cfa_color(unsigned cfa,unsigned phase)
{ static const unsigned table[4][4]={{0,1,1,2},{2,1,1,0},{1,0,2,1},{1,2,0,1}};return table[cfa][phase]; }
static void unpack(void)
{
    uint8_t wire[2048];uint16_t samples[1024];
    for(unsigned i=0;i<1024;++i){wire[2*i]=i>>2;wire[2*i+1]=i&3;samples[i]=999;}
    assert(film_raw10_unpack("film-raw10-unpack/v1",wire,sizeof wire,32,32,samples,1024)==FILM_OK);
    for(unsigned i=0;i<1024;++i)assert(samples[i]==i);
    for(unsigned i=0;i<1024;++i)samples[i]=999;
    wire[2047]=4;
    assert(film_raw10_unpack("film-raw10-unpack/v1",wire,sizeof wire,32,32,samples,1024)==FILM_CORRUPT);
    for(unsigned i=0;i<1024;++i)assert(samples[i]==999);
    assert(film_raw10_unpack("scheduler_decision:admitted",wire,sizeof wire,32,32,samples,1024)==FILM_STATE);
    assert(film_raw10_unpack("film-raw10-unpack/v1",wire,sizeof wire-1,32,32,samples,1024)==FILM_INVALID);
    assert(film_raw10_unpack("film-raw10-unpack/v1",wire,sizeof wire,31,32,samples,1024)==FILM_INVALID);
    assert(film_raw10_unpack("film-raw10-unpack/v1",wire,sizeof wire,32,32,(uint16_t *)wire,1024)==FILM_INVALID);
}
static void isp(void)
{
    uint16_t raw[192],before[192];FilmRaw10View view={16,12,raw};
    for(unsigned cfa=0;cfa<4;++cfa){
        FilmRawIsp p=film_raw_isp_default();p.cfa=(FilmCfa)cfa;p.white=1000;
        for(unsigned y=0;y<12;++y)for(unsigned x=0;x<16;++x)raw[y*16+x]=100*(1+cfa_color(cfa,(y&1)*2+(x&1)));
        memcpy(before,raw,sizeof raw);FilmRawPrepared image={0};
        assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&image)==FILM_OK);
        for(unsigned i=0;i<192;++i){assert(fabsf(image.pixels[i].r-encoded(.1f))<1e-6f);assert(fabsf(image.pixels[i].g-encoded(.2f))<1e-6f);assert(fabsf(image.pixels[i].b-encoded(.3f))<1e-6f);}
        assert(!memcmp(raw,before,sizeof raw));film_raw_prepared_free(&image);
        p.exposure_ev=1;p.wb[0]=2;
        assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&image)==FILM_OK);
        assert(fabsf(image.pixels[30].r-encoded(.4f))<1e-6f);assert(fabsf(image.pixels[30].b-encoded(.6f))<1e-6f);
        film_raw_prepared_free(&image);
    }
    FilmRawIsp p=film_raw_isp_default();FilmRawPrepared image={0};
    for(unsigned i=0;i<192;++i)raw[i]=(i*37)%1024;
    assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&image)==FILM_OK);
    for(unsigned y=0;y<12;++y)for(unsigned x=0;x<16;++x){
        FilmRgb q=image.pixels[y*16+x];float v[3]={q.r,q.g,q.b};
        assert(fabsf(v[cfa_color(0,(y&1)*2+(x&1))]-encoded(raw[y*16+x]/1023.f))<1e-6f);
    }
    film_raw_prepared_free(&image);
    for(unsigned i=0;i<192;++i)raw[i]=200;raw[34]=0;
    FilmRawBadPixel bad[2]={{2,2},{2,2}};p.bad_pixels=bad;p.bad_pixel_count=1;
    assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&image)==FILM_OK);
    assert(fabsf(image.pixels[34].r-encoded(200/1023.f))<1e-6f && raw[34]==0);film_raw_prepared_free(&image);
    p.bad_pixel_count=2;assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&image)==FILM_INVALID);
    p=film_raw_isp_default();p.white=0;assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&image)==FILM_INVALID);
    p=film_raw_isp_default();p.wb[0]=NAN;assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&image)==FILM_INVALID);
    assert(film_raw_isp_prepare("startup_warmup",&view,&p,&image)==FILM_STATE);
    p=film_raw_isp_default();
    memcpy(before,raw,sizeof raw);
    assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,(FilmRawPrepared *)raw)==FILM_INVALID);
    assert(!memcmp(before,raw,sizeof raw));
    raw[0]=1024;assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&image)==FILM_CORRUPT);
}
static float linear(float x){return x<=.04045f?x/12.92f:powf((x+.055f)/1.055f,2.4f);}
static void local_and_offset(void)
{
    uint16_t raw[128*64];
    /* Two flat regions straddling the base grid, with stable RGB ratios. */
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<128;++x){
        unsigned c=cfa_color(0,(y&1)*2+(x&1));
        raw[y*128+x]=(x<64?32:256)*(c==0?1:c==1?2:3);
    }
    FilmRaw10View view={128,64,raw};FilmRawIsp p=film_raw_isp_default();
    FilmRawPrepared base={0},local={0};
    assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&base)==FILM_OK);
    p.local_strength=1;p.local_limit_ev=.5f;
    assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&local)==FILM_OK);
    for(unsigned i=0;i<128*64;++i){
        FilmRgb a=base.pixels[i],b=local.pixels[i];
        float gain=linear(b.g)/linear(a.g);
        assert(gain>=exp2f(-.5f)-1e-5f && gain<=exp2f(.5f)+1e-5f);
        assert(fabsf(linear(b.r)/linear(a.r)-gain)<1e-5f);
        assert(fabsf(linear(b.b)/linear(a.b)-gain)<1e-5f);
    }
    assert(local.pixels[16].g>base.pixels[16].g);
    assert(local.pixels[112].g<base.pixels[112].g);
    film_raw_prepared_free(&local);film_raw_prepared_free(&base);
    p.local_strength=0;p.ccm_offset[0]=-.01f;
    assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&local)==FILM_OK);
    assert(fabsf(linear(local.pixels[16].r)-(32/1023.f-.01f))<1e-6f);
    film_raw_prepared_free(&local);
    p.ccm_offset[0]=0;p.lens_shading[0][3]=1;
    assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&local)==FILM_OK);
    /* x=-1 edge is one stop brighter in red; green remains unchanged. */
    assert(fabsf(linear(local.pixels[0].r)-2*32/1023.f)<1e-6f);
    assert(fabsf(linear(local.pixels[0].g)-64/1023.f)<1e-6f);
    film_raw_prepared_free(&local);
    p.lens_shading[0][3]=NAN;
    assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&local)==FILM_INVALID);
    p.lens_shading[0][3]=0;
    p.local_strength=NAN;assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&local)==FILM_INVALID);
    p.local_strength=1;p.local_limit_ev=4.1f;assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&local)==FILM_INVALID);
    p.local_limit_ev=1;p.ccm_offset[0]=INFINITY;assert(film_raw_isp_prepare("film-raw-isp/v1",&view,&p,&local)==FILM_INVALID);
    assert(film_raw_isp_prepare("scheduler_decision:admitted",&view,&p,&local)==FILM_STATE);
}
static void calibration_contract(void)
{
    FILE *f=fopen("romfs/calibration/display-color.f3isp","rb");assert(f);
    FilmRawIsp p=film_raw_isp_default();
    assert(film_raw_profile_load("scheduler_decision:admitted",f,&p)==FILM_STATE);
    assert(film_raw_profile_load("film-raw-profile-load/v1",f,&p)==FILM_OK);fclose(f);
    FilmRawIsp before=p;
    f=tmpfile();assert(f);fputs("F3ISP1 black nan 0 0 0",f);rewind(f);
    assert(film_raw_profile_load("film-raw-profile-load/v1",f,&p)==FILM_CORRUPT);
    assert(!memcmp(&p,&before,sizeof p));fclose(f);
}
static void scene_headroom(void)
{
    uint16_t raw[16*12];for(unsigned y=0;y<12;++y)for(unsigned x=0;x<16;++x)raw[y*16+x]=x<8?800:900;
    FilmRaw10View view={16,12,raw};FilmRawIsp p=film_raw_isp_default();p.ccm[0]=p.ccm[4]=p.ccm[8]=4;
    FilmRawScene scene={0};
    assert(film_raw_scene_prepare("scheduler_decision:admitted",&view,&p,&scene)==FILM_STATE);
    assert(film_raw_scene_prepare("film-raw-scene/v1",&view,&p,&scene)==FILM_OK);
    assert(scene.pixels[2].r>3 && scene.pixels[12].r>3.5f);
    FilmRgb before[192];memcpy(before,scene.pixels,sizeof before);
    FilmRecipe recipe=film_default_recipe();recipe.grain_amount=0;recipe.input_tone.value[FILM_TONE_EV]=-2;
    FilmDevelopCanvas a={0},b={0};FilmDevelopOptions o={0};
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,NULL,&o,&a)==FILM_OK);
    assert(a.pixels[12*3]>a.pixels[2*3]+2000); /* high values remain distinguishable after lowering EV */
    for(unsigned i=0;i<3;++i)assert(abs((int)a.pixels[2*3+i]-(int)lroundf(encoded(800/1023.f)*65535))<=2);
    o.raw_shoulder=.75f;
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,NULL,&o,&b)==FILM_OK);
    assert(!memcmp(before,scene.pixels,sizeof before));
    film_develop_canvas_free(&a);film_develop_canvas_free(&b);
    o.raw_midtones=NAN;assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,NULL,&o,&a)==FILM_INVALID);
    assert(film_raw_scene_grade("startup_warmup",&scene,&recipe,NULL,&o,&a)==FILM_STATE);
    film_raw_scene_free(&scene);
    p.ccm_offset[0]=-1;p.ccm[0]=1;
    assert(film_raw_scene_prepare("film-raw-scene/v1",&view,&p,&scene)==FILM_OK);
    assert(scene.pixels[2].r<0);film_raw_scene_free(&scene);
    /* Dense log ramp, all extreme scene-curve combinations remain monotone. */
    FilmRgb ramp[512];for(unsigned i=0;i<512;++i){float v=.18f*exp2f(-12+24*i/511.f);ramp[i]=(FilmRgb){v,v,v};}
    FilmRawScene synthetic={32,16,ramp};recipe.input_tone.value[FILM_TONE_EV]=0;
    for(int sh=-1;sh<=1;++sh)for(int mid=-1;mid<=1;++mid)for(int hi=-1;hi<=1;++hi){
        o=(FilmDevelopOptions){.raw_shadows=2.5f*sh,.raw_midtones=2.5f*mid,.raw_highlights=2.5f*hi,.raw_shoulder=.75f};
        assert(film_raw_scene_grade("film-raw-scene-grade/v1",&synthetic,&recipe,NULL,&o,&a)==FILM_OK);
        for(unsigned i=1;i<512;++i)assert(a.pixels[3*i]>=a.pixels[3*(i-1)]);
        film_develop_canvas_free(&a);
    }
}
static void grade(void)
{
    FilmRawPrepared prepared={16,12,calloc(192,sizeof(FilmRgb))};assert(prepared.pixels);
    for(unsigned i=0;i<192;++i)prepared.pixels[i]=(FilmRgb){.2f,.4f,.6f};
    FilmRgb before[192];memcpy(before,prepared.pixels,sizeof before);
    FilmRecipe p=film_default_recipe();FilmDevelopCanvas plain={0},look={0};
    assert(film_raw_grade_canvas("film-raw-grade/v1",&prepared,&p,NULL,(FilmDevelopCanvas *)prepared.pixels)==FILM_INVALID);
    assert(!memcmp(before,prepared.pixels,sizeof before));
    assert(film_raw_grade_canvas("film-raw-grade/v1",&prepared,&p,NULL,&plain)==FILM_OK);
    FilmLut lut={.size=17,.max={1,1,1},.samples=calloc(17*17*17,sizeof(FilmRgb))};assert(lut.samples);
    for(unsigned b=0;b<17;++b)for(unsigned g=0;g<17;++g)for(unsigned r=0;r<17;++r)lut.samples[r+17*g+289*b]=(FilmRgb){r/16.f,g/16.f,b/16.f};
    strcpy(p.lut_id,"identity");
    assert(film_raw_grade_canvas("film-raw-grade/v1",&prepared,&p,&lut,&look)==FILM_OK);
    for(unsigned i=0;i<192*3;++i)assert(abs((int)plain.pixels[i]-(int)look.pixels[i])<=1);
    assert(!memcmp(before,prepared.pixels,sizeof before));film_develop_canvas_free(&look);
    for(unsigned i=0;i<17*17*17;++i)lut.samples[i]=(FilmRgb){.1f,.2f,.3f};
    p.output_tone.value[FILM_TONE_MID]=.5f;
    assert(film_raw_grade_canvas("film-raw-grade/v1",&prepared,&p,&lut,&look)==FILM_OK);
    assert(memcmp(plain.pixels,look.pixels,192*3*sizeof(uint16_t)));
    FilmImage rendered={0};p.sharpness=1;
    assert(film_develop_canvas_render(&look,&p,NULL,true,&rendered)==FILM_OK);
    assert(rendered.width==16&&rendered.height==12);film_image_free(&rendered);
    film_develop_canvas_free(&plain);film_develop_canvas_free(&look);film_lut_free(&lut);
    assert(film_raw_grade_canvas("film-raw-grade/v1",&prepared,&p,NULL,&plain)==FILM_STATE);
    assert(film_raw_grade_canvas("scheduler_decision:admitted",&prepared,&p,NULL,&plain)==FILM_STATE);
    film_raw_prepared_free(&prepared);
}
static void grain(void)
{
    FilmImage a={0},b={0};assert(film_image_alloc(&a,384,256,FILM_RGB24)==FILM_OK);assert(film_image_alloc(&b,384,256,FILM_RGB24)==FILM_OK);
    size_t bytes=384*256*3;memset(a.pixels,128,bytes);memset(b.pixels,128,bytes);
    FilmGrainProfile profile;FILE *f=fopen("romfs/grain/medium.f3g","rb");assert(f);
    assert(film_grain_profile_load(f,&profile)==FILM_OK);fclose(f);
    FilmLayeredGrain p={.amount=0,.size=1,.coarse=.5f,.shadow=1,.mid=1,.highlight=1,.reference_width=384,.seed=1616};
    assert(film_grain_layered("film-grain-layered/v1",&a,&p,&profile)==FILM_OK);assert(!memcmp(a.pixels,b.pixels,bytes));
    p.amount=.5f;assert(film_grain_layered("film-grain-layered/v1",&a,&p,&profile)==FILM_OK);
    assert(film_grain_layered("film-grain-layered/v1",&b,&p,&profile)==FILM_OK);assert(!memcmp(a.pixels,b.pixels,bytes));
    double mean=0,square=0;for(unsigned i=0;i<384*256;++i){int d=(int)a.pixels[3*i]-128;mean+=d;square+=d*d;assert(a.pixels[3*i]==a.pixels[3*i+1]&&a.pixels[3*i]==a.pixels[3*i+2]);}
    mean/=384*256;square=sqrt(square/(384*256));assert(fabs(mean)<1.5 && square>4 && square<9);
    memset(b.pixels,128,bytes);p.seed++;
    assert(film_grain_layered("film-grain-layered/v1",&b,&p,&profile)==FILM_OK);assert(memcmp(a.pixels,b.pixels,bytes));
    assert(film_grain_layered("unknown",&b,&p,&profile)==FILM_STATE);
    memset(a.pixels,0,bytes);assert(film_grain_layered("film-grain-layered/v1",&a,&p,&profile)==FILM_OK);
    for(size_t i=0;i<bytes;++i)assert(!a.pixels[i]);
    memset(a.pixels,255,bytes);assert(film_grain_layered("film-grain-layered/v1",&a,&p,&profile)==FILM_OK);
    for(size_t i=0;i<bytes;++i)assert(a.pixels[i]==255);
    printf("PASS layered grain deterministic density response: mean delta %.3f RMS %.3f\n",mean,square);
    film_image_free(&a);film_image_free(&b);
}
int main(void){unpack();isp();local_and_offset();calibration_contract();scene_headroom();grade();grain();puts("PASS RAW10 exact unpack, immutable ISP, four CFA patterns, exposure/WB, explicit repair, shared LUT/neutral canvas");return 0;}
