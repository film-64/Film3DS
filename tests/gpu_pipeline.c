#include "film.h"
#include "darkroom.h"
#include "raw_darkroom.h"
#include "layered_grain.h"
#include "gpu_layout.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void bad_pixels(void)
{
    uint16_t raw[12*12],before[12*12];
    for(unsigned y=0;y<12;++y)for(unsigned x=0;x<12;++x)raw[y*12+x]=100+100*((y&1)*2+(x&1));
    raw[4*12+4]=1023;raw[7*12+7]=0;raw[0]=1023;
    memcpy(before,raw,sizeof raw);
    assert(film_raw_bad_pixel_clean("scheduler_decision:admitted",raw,12,12)==FILM_STATE&&!memcmp(raw,before,sizeof raw));
    assert(film_raw_bad_pixel_clean("film-raw-bad-pixel/v1",raw,12,12)==FILM_OK);
    assert(raw[4*12+4]==100&&raw[7*12+7]==400&&raw[0]==1023);
    for(unsigned y=0;y<12;++y)for(unsigned x=0;x<12;++x)raw[y*12+x]=x<6?100:800;
    memcpy(before,raw,sizeof raw);assert(film_raw_bad_pixel_clean("film-raw-bad-pixel/v1",raw,12,12)==FILM_OK);
    assert(!memcmp(before,raw,sizeof raw)); /* Coherent edge is not a defect. */
    for(unsigned i=0;i<144;++i)raw[i]=100;
    raw[4*12+4]=raw[4*12+6]=900;memcpy(before,raw,sizeof raw);
    assert(film_raw_bad_pixel_clean("film-raw-bad-pixel/v1",raw,12,12)==FILM_OK&&!memcmp(before,raw,sizeof raw));
    raw[3]=1024;memcpy(before,raw,sizeof raw);
    assert(film_raw_bad_pixel_clean("film-raw-bad-pixel/v1",raw,12,12)==FILM_CORRUPT&&!memcmp(before,raw,sizeof raw));
    puts("PASS same-CFA hot/dead correction, immutable rejection, borders, edges and clusters");
}
static void layout(void)
{
    unsigned sizes[][2]={{8,8},{256,256},{1024,512}};
    for(unsigned n=0;n<3;++n){unsigned w=sizes[n][0],h=sizes[n][1];
        uint8_t *seen=calloc((size_t)w*h,1);assert(seen);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
            unsigned offset=film_gpu_texel_offset(x,y,w,h);assert(!(offset&3)&&offset<(size_t)w*h*4);
            assert(!seen[offset/4]);seen[offset/4]=1;
        }
        for(unsigned i=0;i<w*h;++i)assert(seen[i]);free(seen);
        assert(film_gpu_texel_offset(0,h-1,w,h)==0&&film_gpu_texel_offset(1,h-1,w,h)==4);
        assert(film_gpu_texel_offset(0,h-2,w,h)==8);
    }
    puts("PASS rectangular texture layout bijection, corners and 8px tile boundaries");
}
static unsigned quant(float v){return (unsigned)lroundf(fminf(fmaxf(v,0),255));}
static unsigned compose(unsigned photo,const uint8_t m[5])
{
    unsigned gain=128;
    for(unsigned layer=0;layer<3;++layer)gain=quant(2.f*gain*m[layer]/255);
    unsigned v=quant(2.f*photo*gain/255);v=quant(v+(photo?m[3]:0));return v>m[4]?v-m[4]:0;
}
static unsigned grain_calls;
static FilmStatus counted(const char *source,FilmImage *image,const FilmLayeredGrain *p,
    const FilmGrainProfile *profile,const FilmGrainTexture *texture)
{++grain_calls;return film_grain_layered_texture(source,image,p,profile,texture);}
static void grain_and_pair(void)
{
    FilmGrainProfile profile;FILE *file=fopen("romfs/grain/medium.f3g","rb");
    assert(file&&film_grain_profile_load(file,&profile)==FILM_OK);fclose(file);
    FilmFrame frame={.recipe=film_default_recipe()};assert(film_image_alloc(&frame.source,510,258,FILM_RGB24)==FILM_OK);
    size_t count=(size_t)frame.source.width*frame.source.height;
    for(unsigned y=0;y<258;++y)for(unsigned x=0;x<510;++x){unsigned i=(y*510+x)*3;
        frame.source.pixels[i]=frame.source.pixels[i+1]=frame.source.pixels[i+2]=(uint8_t)(x%256);}
    float *planes=malloc(count*3*sizeof(float)),*again=malloc(count*3*sizeof(float)),*sigma=malloc(count*sizeof(float)),*second=malloc(count*sizeof(float));
    uint8_t *tone=malloc(count),*tone2=malloc(count);assert(planes&&again&&sigma&&second&&tone&&tone2);
    FilmLayeredGrain p={.amount=.5f,.size=1,.coarse=.5f,.shadow=.8f,.mid=1.1f,.highlight=.7f,.reference_width=510,.seed=1234};
    uint8_t m[5]={9,9,9,9,9},before[5];memcpy(before,m,5);float zero[3]={0};
    assert(film_grain_gpu_masks("unknown",zero,0,0,m)==FILM_STATE&&!memcmp(m,before,5));
    float huge[3]={1000,0,0};assert(film_grain_gpu_masks("film-grain-gpu-compose/v1",huge,.7f,0,m)==FILM_UNSUPPORTED&&!memcmp(m,before,5));
    unsigned maximum=0,peak_input=0,peak_actual=0,peak_expected=0;
    for(unsigned kind=0;kind<3;++kind){p.coarse=kind==0?.2f:kind==1?.5f:.8f;
        assert(film_grain_response_prepare("film-grain-gpu-compose/v1",&frame.source,&p,&profile,NULL,planes,sigma,tone)==FILM_OK);
        assert(film_grain_response_prepare("film-grain-gpu-compose/v1",&frame.source,&p,&profile,NULL,again,second,tone2)==FILM_OK);
        assert(!memcmp(planes,again,count*3*sizeof(float))&&!memcmp(sigma,second,count*sizeof(float)));
        FilmImage reference={0};assert(film_image_alloc(&reference,510,258,FILM_RGB24)==FILM_OK);memcpy(reference.pixels,frame.source.pixels,count*3);
        assert(film_grain_layered_texture("film-grain-layered/v1",&reference,&p,&profile,NULL)==FILM_OK);
        for(unsigned y0=0;y0<258;y0+=254)for(unsigned x0=0;x0<510;x0+=254)
            for(unsigned y=y0;y<258&&y<y0+254;++y)for(unsigned x=x0;x<510&&x<x0+254;++x){
                size_t i=(size_t)y*510+x;uint8_t a[5],b[5];
                assert(film_grain_gpu_masks("film-grain-gpu-compose/v1",planes+3*i,sigma[i],tone[i],a)==FILM_OK);
                assert(film_grain_gpu_masks("film-grain-gpu-compose/v1",again+3*i,second[i],tone2[i],b)==FILM_OK&&!memcmp(a,b,5));
                unsigned actual=compose(frame.source.pixels[3*i],a),expected=reference.pixels[3*i];
                unsigned error=actual>expected?actual-expected:expected-actual;if(error>maximum){maximum=error;peak_input=frame.source.pixels[3*i];peak_actual=actual;peak_expected=expected;}
            }
        film_image_free(&reference);
    }
    fprintf(stderr,"affine max=%u input=%u actual=%u CPU=%u\n",maximum,peak_input,peak_actual,peak_expected);
    assert(maximum<=8); /* Explicit RGBA8 affine approximation, not CPU bit identity. */
    for(unsigned y=0;y<258;++y)for(unsigned x=0;x<510;++x){size_t i=((size_t)y*510+x)*3;
        frame.source.pixels[i]=x%256;frame.source.pixels[i+1]=255-x%256;frame.source.pixels[i+2]=y%256;}
    p.coarse=.5f;
    assert(film_grain_response_prepare("film-grain-gpu-compose/v1",&frame.source,&p,&profile,NULL,planes,sigma,tone)==FILM_OK);
    FilmImage color={0};assert(film_image_alloc(&color,510,258,FILM_RGB24)==FILM_OK);memcpy(color.pixels,frame.source.pixels,count*3);
    assert(film_grain_layered_texture("film-grain-layered/v1",&color,&p,&profile,NULL)==FILM_OK);
    unsigned color_max=0;
    for(size_t i=0;i<count;++i){assert(film_grain_gpu_masks("film-grain-gpu-compose/v1",planes+3*i,sigma[i],tone[i],m)==FILM_OK);
        for(unsigned c=0;c<3;++c){unsigned actual=compose(frame.source.pixels[3*i+c],m),expected=color.pixels[3*i+c];
            unsigned error=actual>expected?actual-expected:expected-actual;if(error>color_max)color_max=error;}}
    fprintf(stderr,"color affine max=%u\n",color_max);assert(color_max<=8);film_image_free(&color);
    assert(film_grain_response_prepare("scheduler_decision:admitted",&frame.source,&p,&profile,NULL,again,second,tone2)==FILM_STATE);
    p.seed++;assert(film_grain_response_prepare("film-grain-gpu-compose/v1",&frame.source,&p,&profile,NULL,again,second,tone2)==FILM_OK);
    assert(memcmp(planes,again,count*3*sizeof(float)));
    p.size=2;assert(film_grain_response_prepare("film-grain-gpu-compose/v1",&frame.source,&p,&profile,NULL,again,second,tone2)==FILM_OK);
    assert(memcmp(planes,again,count*3*sizeof(float)));
    free(planes);free(again);free(sigma);free(second);free(tone);free(tone2);
    frame.recipe.grain_amount=.5f;frame.recipe.crop_percent=50;frame.recipe.scale=FILM_150;
    FilmDevelopOptions options={.layered_grain=true,.grain=FILM_GRAIN_MEDIUM,.grain_size=1,.grain_shadow=.8f,.grain_mid=1.1f,.grain_highlight=.7f,
        .grain_profile=&profile,.grain_renderer=counted};
    FilmDarkroom room={0};grain_calls=0;
    assert(film_darkroom_preview(&room,&frame,NULL,&options)==FILM_OK&&grain_calls==1);
    FilmRect crop;assert(film_composition_rect(510,258,&frame.recipe,&crop)==FILM_OK);
    FilmImage inset={0};assert(film_resize(&room.preview,crop.width,crop.height,FILM_BILINEAR,&inset)==FILM_OK);
    for(unsigned y=0;y<crop.height;++y)assert(!memcmp(inset.pixels+(size_t)y*crop.width*3,
        room.composition_view.pixels+((size_t)(crop.y+y)*510+crop.x)*3,(size_t)crop.width*3));
    const char *expected="build/gpu-pair-expected.jpg",*saved="build/gpu-pair-saved.jpg";unlink(expected);unlink(saved);
    assert(film_jpeg_write(expected,&room.preview,room.quality)==FILM_OK);
    assert(film_darkroom_finish(&room,true,saved)==FILM_OK&&grain_calls==1);
    FILE *a=fopen(expected,"rb"),*b=fopen(saved,"rb");assert(a&&b);int x,y;
    do{x=fgetc(a);y=fgetc(b);assert(x==y);}while(x!=EOF);fclose(a);fclose(b);unlink(expected);unlink(saved);
    film_image_free(&inset);film_frame_free(&frame);
    printf("PASS deterministic 3-scale masks, 254px boundaries, seed/size changes, single grain pass, composition/output and JPEG identity; software affine max error=%u/255\n",maximum);
}
int main(void){bad_pixels();layout();grain_and_pair();return 0;}
