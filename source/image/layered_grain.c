#include "layered_grain.h"
#include "film_runtime_contract.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t crc(const uint8_t *p,size_t n)
{uint32_t v=UINT32_MAX;for(size_t i=0;i<n;++i){v^=p[i];for(unsigned b=0;b<8;++b)v=(v>>1)^((v&1)?UINT32_C(0xedb88320):0);}return v^UINT32_MAX;}
FilmStatus film_grain_texture_load(FILE *file,FilmGrainTexture *out)
{
    enum {SIDE=128,COUNT=9*SIDE*SIDE,BYTES=56+2*COUNT+4};
    if(!file||!out||out->pixels)return FILM_INVALID;
    uint8_t *data=malloc(BYTES);if(!data)return FILM_NOMEM;
    if(fread(data,1,BYTES,file)!=BYTES||fgetc(file)!=EOF||ferror(file)||memcmp(data,"F3GT",4)||
       read32(data+4)!=1||read32(data+8)!=SIDE||read32(data+12)!=SIDE||read32(data+16)!=9||
       read32(data+BYTES-4)!=crc(data,BYTES-4)){free(data);return FILM_CORRUPT;}
    FilmGrainTexture result={.side=SIDE};
    for(unsigned i=0;i<9;++i){uint32_t v=read32(data+20+4*i);memcpy(&result.code[i],&v,4);
        if(!isfinite(result.code[i])||result.code[i]<0||result.code[i]>255||(i&&result.code[i]<=result.code[i-1])){free(data);return FILM_CORRUPT;}}
    result.pixels=malloc(COUNT*sizeof *result.pixels);if(!result.pixels){free(data);return FILM_NOMEM;}
    for(unsigned i=0;i<COUNT;++i){unsigned v=(unsigned)data[56+2*i]|((unsigned)data[57+2*i]<<8);result.pixels[i]=(int16_t)(v<32768?(int)v:(int)v-65536);}
    free(data);
    for(unsigned band=0;band<9;++band){double sum=0,square=0;
        for(unsigned i=0;i<SIDE*SIDE;++i){double v=result.pixels[band*SIDE*SIDE+i]/4096.;sum+=v;square+=v*v;}
        if(fabs(sum/(SIDE*SIDE))>.02||square/(SIDE*SIDE)<.95||square/(SIDE*SIDE)>1.05){free(result.pixels);return FILM_CORRUPT;}}
    *out=result;return FILM_OK;
}
void film_grain_texture_free(FilmGrainTexture *t){if(t){free(t->pixels);*t=(FilmGrainTexture){0};}}

static uint32_t hash(uint32_t seed,int x,int y)
{
    uint32_t v=seed^(uint32_t)x*UINT32_C(0x9e3779b9)^(uint32_t)y*UINT32_C(0x85ebca6b);
    v=(v^(v>>16))*UINT32_C(0x7feb352d);v=(v^(v>>15))*UINT32_C(0x846ca68b);return v^(v>>16);
}
static float random_value(uint32_t v)
{ return ((float)(v&65535)-(float)(v>>16))*(2.449489743f/65535); }
static float field(uint32_t seed,float x,float y)
{
    int ix=(int)floorf(x),iy=(int)floorf(y);float fx=x-ix,fy=y-iy;
    fx=fx*fx*(3-2*fx);fy=fy*fy*(3-2*fy);
    float a=(1-fx)*(1-fy),b=fx*(1-fy),c=(1-fx)*fy,d=fx*fy;
    return (a*random_value(hash(seed,ix,iy))+b*random_value(hash(seed,ix+1,iy))+
            c*random_value(hash(seed,ix,iy+1))+d*random_value(hash(seed,ix+1,iy+1)))/sqrtf(a*a+b*b+c*c+d*d);
}
static float decode(float x) {return x<=.04045f?x/12.92f:powf((x+.055f)/1.055f,2.4f);}
static float encode(float x) {return x<=.0031308f?12.92f*x:1.055f*powf(x,1/2.4f)-.055f;}
static float clamp(float x,float lo,float hi) {return fminf(fmaxf(x,lo),hi);}
static bool valid(float x,float lo,float hi) {return isfinite(x)&&x>=lo&&x<=hi;}

FilmStatus film_grain_gpu_encoding(const char *source,const float *planes,size_t pixels,float *scale)
{
    if(!source||strcmp(source,"film-grain-gpu-mix/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!planes||!scale||!pixels||pixels>4096u*4096u)return FILM_INVALID;
    float peak=0;
    for(size_t i=0;i<pixels*3;++i){if(!isfinite(planes[i]))return FILM_INVALID;peak=fmaxf(peak,fabsf(planes[i]));}
    *scale=peak?fminf(8,40/peak):8;
    return *scale>=3.75f?FILM_OK:FILM_UNSUPPORTED;
}

static float texture_at(const FilmGrainTexture *t,unsigned band,float x,float y)
{
    unsigned ix=(unsigned)floorf(x),iy=(unsigned)floorf(y);float fx=x-ix,fy=y-iy;
    const int16_t *p=t->pixels+band*128*128+iy*128+ix;
    float a=(1-fx)*(1-fy),b=fx*(1-fy),c=(1-fx)*fy,d=fx*fy;
    return (a*p[0]+b*p[1]+c*p[128]+d*p[129])/(4096*sqrtf(a*a+b*b+c*c+d*d));
}
static float texture_field(const FilmGrainTexture *t,uint32_t seed,float code,float u,float v)
{
    unsigned lo=0;while(lo<7&&code>t->code[lo+1])++lo;
    float tone=clamp((code-t->code[lo])/(t->code[lo+1]-t->code[lo]),0,1);
    int ix=(int)floorf(u/32),iy=(int)floorf(v/32);float fx=u/32-ix,fy=v/32-iy;
    fx=fx*fx*(3-2*fx);fy=fy*fy*(3-2*fy);float result=0,squares=0;
    for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){
        uint32_t h=hash(seed,ix+dx,iy+dy);
        float x=u-(ix+dx)*32,y=v-(iy+dy)*32;
        if(h&4)x=-x;
        for(unsigned r=0;r<(h&3);++r){float z=x;x=-y;y=z;}
        x+=32+(h>>8)%63;y+=32+(h>>16)%63;
        float a=texture_at(t,lo,x,y),b=texture_at(t,lo+1,x,y);
        float sample=(a*(1-tone)+b*tone)/sqrtf((1-tone)*(1-tone)+tone*tone);
        float w=(dx?fx:1-fx)*(dy?fy:1-fy);
        w*=1+.2f*field(h,u/9,v/9);result+=w*sample;squares+=w*w;
    }
    return result/sqrtf(squares);
}

FilmStatus film_grain_layered(const char *source,FilmImage *im,const FilmLayeredGrain *p,const FilmGrainProfile *profile)
{return film_grain_layered_texture(source,im,p,profile,NULL);}

static void density_pixel(uint8_t *pixel,const FilmLayeredGrain *p,const float *rms,float n)
{
    float rgb[3]={decode(pixel[0]/255.f),decode(pixel[1]/255.f),decode(pixel[2]/255.f)};
    float l=.2126f*rgb[0]+.7152f*rgb[1]+.0722f*rgb[2],code=clamp(encode(l),0,1);
    if(l<=0||code>=1)return;
    unsigned idx=(unsigned)lroundf(code*255);
    float response=p->shadow*(1-code)*(1-code)+p->mid*2*code*(1-code)+p->highlight*code*code;
    float target=fminf(p->amount*rms[idx]/255*response,fminf(code,1-code)*.5f);
    float derivative=l<=.0031308f?12.92f:1.055f/2.4f*powf(l,1/2.4f-1);
    float sigma=fminf(target/fmaxf(derivative*l,.000001f),.7f);
    float gain=expf(sigma*n-.5f*sigma*sigma);
    for(unsigned k=0;k<3;++k)pixel[k]=(uint8_t)lroundf(clamp(encode(fmaxf(rgb[k]*gain,0)),0,1)*255);
}

FilmStatus film_grain_layered_texture(const char *source,FilmImage *im,const FilmLayeredGrain *p,const FilmGrainProfile *profile,const FilmGrainTexture *texture)
{return film_grain_layered_mix(source,im,p,profile,texture,NULL);}

FilmStatus film_grain_layered_mix(const char *source,FilmImage *im,const FilmLayeredGrain *p,const FilmGrainProfile *profile,const FilmGrainTexture *texture,FilmGrainMixer mix)
{
    if (!source || strcmp(source,"film-grain-layered/v1") || !film_runtime_source_valid(source)) return FILM_STATE;
    if (!im || !im->pixels || im->format!=FILM_RGB24 || !film_image_bytes(im->width,im->height,im->format) ||
        !p || !profile || !valid(p->amount,0,1) || !valid(p->size,.25f,8) || !valid(p->coarse,0,1) ||
        !valid(p->shadow,0,2) || !valid(p->mid,0,2) || !valid(p->highlight,0,2) ||
        !p->reference_width || p->reference_width>4096 || im->width>4096 || im->height>4096) return FILM_INVALID;
    for (unsigned i=0;i<9;++i)
        if (!valid(profile->code[i],0,255) || (i && profile->code[i]<=profile->code[i-1]) ||
            !valid(profile->rms[i],0,32) || !valid(profile->correlation[i],-.95f,.95f)) return FILM_INVALID;
    if(texture){if(texture->side!=128||!texture->pixels)return FILM_INVALID;
        for(unsigned i=0;i<9;++i)if(!valid(texture->code[i],0,255)||(i&&texture->code[i]<=texture->code[i-1]))return FILM_INVALID;}
    if (!p->amount) return FILM_OK;
    float rms[256],corr[256];unsigned lower=0;
    for (unsigned i=0;i<256;++i) {
        while(lower<7 && i>profile->code[lower+1]) ++lower;
        float t=clamp((i-profile->code[lower])/(profile->code[lower+1]-profile->code[lower]),0,1);
        rms[i]=profile->rms[lower]*(1-t)+profile->rms[lower+1]*t;
        corr[i]=profile->correlation[lower]*(1-t)+profile->correlation[lower+1]*t;
    }
    size_t count=(size_t)im->width*im->height;
    float *planes=mix?calloc(count*3,sizeof(float)):NULL,*noise=mix?calloc(count,sizeof(float)):NULL;
    if(mix&&(!planes||!noise)){free(planes);free(noise);return FILM_NOMEM;}
    float scale=(float)im->width/p->reference_width*p->size;
    for (unsigned y=0;y<im->height;++y) for (unsigned x=0;x<im->width;++x) {
        uint8_t *pixel=im->pixels+((size_t)y*im->width+x)*3;
        float rgb[3]={decode(pixel[0]/255.f),decode(pixel[1]/255.f),decode(pixel[2]/255.f)};
        float l=.2126f*rgb[0]+.7152f*rgb[1]+.0722f*rgb[2];
        float code=clamp(encode(l),0,1);
        if (l<=0 || code>=1) continue;
        unsigned idx=(unsigned)lroundf(code*255);
        float u=(x+.5f)/scale,v=(y+.5f)/scale;
        float warp=field(p->seed^UINT32_C(0x561748ac),u/7.1f,v/7.1f);
        float a=field(p->seed,u*.8660254f-v*.5f+.6f*warp,u*.5f+v*.8660254f-.4f*warp);
        if(texture)a=texture_field(texture,p->seed,code*255,u+.6f*warp,v-.4f*warp);
        float b=field(p->seed^UINT32_C(0xb4761da1),u/2.4f+.3f*warp,v/2.4f-.6f*warp);
        float c=field(p->seed^UINT32_C(0xd1b54a35),u/7.1f+.17f,v/7.1f+.91f);
        float wb=.25f+.7f*p->coarse+.2f*corr[idx],wc=.15f*p->coarse;
        float norm=sqrtf(1+wb*wb+wc*wc);
        if(mix){size_t i=((size_t)y*im->width+x)*3;planes[i]=a/norm;planes[i+1]=wb*b/norm;planes[i+2]=wc*c/norm;}
        else density_pixel(pixel,p,rms,(a+wb*b+wc*c)/norm);
    }
    if(mix){FilmStatus status=mix(im->width,im->height,planes,noise);free(planes);
        if(status!=FILM_OK){free(noise);return status;}
        for(size_t i=0;i<count;++i)if(!isfinite(noise[i])){free(noise);return FILM_CORRUPT;}
        for(size_t i=0;i<count;++i)density_pixel(im->pixels+3*i,p,rms,noise[i]);
        free(noise);
    }
    return FILM_OK;
}
