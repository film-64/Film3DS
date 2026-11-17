#include "raw_darkroom.h"
#include "film_runtime_contract.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool admitted(const char *source,const char *expected)
{ return source && !strcmp(source,expected) && film_runtime_source_valid(source); }
static bool shape(unsigned w,unsigned h)
{ return w>=4 && h>=4 && !(w&1) && !(h&1) && (size_t)w<=FILM_RAW_MAX_PIXELS/h; }
static bool range(float x,float lo,float hi) { return isfinite(x) && x>=lo && x<=hi; }
static bool overlap(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;if(an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y)return true;return an&&bn&&x<y+bn&&y<x+an;}
static unsigned color(FilmCfa cfa,unsigned x,unsigned y)
{
    static const unsigned colors[4][4]={{0,1,1,2},{2,1,1,0},{1,0,2,1},{1,2,0,1}};
    return colors[cfa][(y&1)*2+(x&1)];
}
static unsigned reflect(int x,unsigned n)
{ return x<0?(unsigned)-x:(unsigned)x>=n?2*n-2-(unsigned)x:(unsigned)x; }

FilmRawIsp film_raw_isp_default(void)
{ return (FilmRawIsp){.cfa=FILM_RGGB,.white=1023,.wb={1,1,1},.ccm={1,0,0,0,1,0,0,0,1}}; }

FilmStatus film_raw10_unpack(const char *source,const uint8_t *wire,size_t bytes,
    unsigned w,unsigned h,uint16_t *samples,size_t capacity)
{
    if (!admitted(source,"film-raw10-unpack/v1")) return FILM_STATE;
    if (!wire || !samples || !shape(w,h)) return FILM_INVALID;
    size_t n=(size_t)w*h;
    if (bytes!=2*n || capacity<n) return FILM_INVALID;
    uintptr_t a=(uintptr_t)wire,b=(uintptr_t)samples;
    if (a>UINTPTR_MAX-bytes || b>UINTPTR_MAX-2*n || (a<b+2*n && b<a+bytes)) return FILM_INVALID;
    for (size_t i=0;i<n;++i) if (wire[2*i+1]>3) return FILM_CORRUPT;
    for (size_t i=0;i<n;++i) samples[i]=(uint16_t)(((uint16_t)wire[2*i]<<2)|wire[2*i+1]);
    return FILM_OK;
}

static float median(float *v,unsigned n)
{
    for (unsigned i=1;i<n;++i) { float t=v[i];unsigned j=i;while(j && v[j-1]>t) {v[j]=v[j-1];--j;}v[j]=t; }
    return n&1?v[n/2]:(v[n/2-1]+v[n/2])*.5f;
}

static float interpolate(const float *raw,unsigned w,unsigned h,FilmCfa cfa,unsigned x,unsigned y,unsigned c)
{
    if (color(cfa,x,y)==c) return raw[(size_t)y*w+x];
    float value=0,weight=0;
    for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
        unsigned xx=reflect((int)x+dx,w),yy=reflect((int)y+dy,h);
        if (color(cfa,xx,yy)!=c) continue;
        float k=(dx?1.f:2.f)*(dy?1.f:2.f);
        value+=raw[(size_t)yy*w+xx]*k;weight+=k;
    }
    return value/weight;
}

static float encode(float v)
{ v=fmaxf(v,0);return v<=.0031308f?12.92f*v:1.055f*powf(v,1/2.4f)-.055f; }

FilmStatus film_raw_isp_prepare(const char *source,const FilmRaw10View *raw,
    const FilmRawIsp *p,FilmRawPrepared *out)
{
    if (!admitted(source,"film-raw-isp/v1")) return FILM_STATE;
    if (!raw || !raw->samples || !p || !out || !shape(raw->width,raw->height) ||
        (unsigned)p->cfa>FILM_GBRG || !range(p->white,1,1023) || !range(p->exposure_ev,-8,8) ||
        !range(p->shoulder,0,.99f) ||
        p->bad_pixel_count>FILM_RAW_MAX_BAD_PIXELS || (p->bad_pixel_count && !p->bad_pixels)) return FILM_INVALID;
    for (unsigned i=0;i<4;++i) if (!range(p->black[i],0,p->white-1)) return FILM_INVALID;
    for (unsigned i=0;i<3;++i) if (!range(p->wb[i],.125f,8)) return FILM_INVALID;
    for (unsigned i=0;i<9;++i) if (!range(p->ccm[i],-8,8)) return FILM_INVALID;
    unsigned w=raw->width,h=raw->height;size_t n=(size_t)w*h;
    if(overlap(out,sizeof *out,raw->samples,n*sizeof *raw->samples)||overlap(out,sizeof *out,p,sizeof *p)||
       overlap(out,sizeof *out,raw,sizeof *raw)||overlap(out,sizeof *out,p->bad_pixels,p->bad_pixel_count*sizeof *p->bad_pixels))return FILM_INVALID;
    if(out->pixels)return FILM_INVALID;
    for (size_t i=0;i<n;++i) if (raw->samples[i]>1023) return FILM_CORRUPT;
    uint8_t *mask=p->bad_pixel_count?calloc(n,1):NULL;
    if (p->bad_pixel_count && !mask) return FILM_NOMEM;
    for (unsigned i=0;i<p->bad_pixel_count;++i) {
        unsigned x=p->bad_pixels[i].x,y=p->bad_pixels[i].y;
        if (x>=w || y>=h || mask[(size_t)y*w+x]) {free(mask);return FILM_INVALID;}
        mask[(size_t)y*w+x]=1;
    }
    float *work=malloc(n*sizeof *work);
    FilmRgb *rgb=malloc(n*sizeof *rgb);
    if (!work || !rgb) {free(mask);free(work);free(rgb);return FILM_NOMEM;}
    for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x) {
        unsigned phase=(y&1)*2+(x&1);size_t i=(size_t)y*w+x;
        float v=raw->samples[i];
        if (mask && mask[i]) {
            float values[8];unsigned count=0;
            for (int dy=-2;dy<=2;dy+=2) for (int dx=-2;dx<=2;dx+=2) {
                int xx=(int)x+dx,yy=(int)y+dy;
                if ((!dx && !dy) || xx<0 || yy<0 || xx>=(int)w || yy>=(int)h) continue;
                size_t j=(size_t)yy*w+(unsigned)xx;
                if (!mask[j]) values[count++]=raw->samples[j];
            }
            if (!count) {free(mask);free(work);free(rgb);return FILM_INVALID;}
            v=median(values,count);
        }
        work[i]=fmaxf(v-p->black[phase],0)/(p->white-p->black[phase]);
    }
    free(mask);
    float exposure=exp2f(p->exposure_ev);
    for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x) {
        float v[3];
        for (unsigned c=0;c<3;++c) v[c]=interpolate(work,w,h,p->cfa,x,y,c)*p->wb[c]*exposure;
        float a[3];
        for (unsigned c=0;c<3;++c) a[c]=fmaxf(0,p->ccm[3*c]*v[0]+p->ccm[3*c+1]*v[1]+p->ccm[3*c+2]*v[2]);
        float peak=fmaxf(a[0],fmaxf(a[1],a[2])),gain=1;
        if (p->shoulder && peak>p->shoulder) {
            float k=p->shoulder;
            gain=(k+(1-k)*(1-expf(-(peak-k)/(1-k))))/peak;
        }
        rgb[(size_t)y*w+x]=(FilmRgb){encode(a[0]*gain),encode(a[1]*gain),encode(a[2]*gain)};
    }
    free(work);
    *out=(FilmRawPrepared){w,h,rgb};return FILM_OK;
}

void film_raw_prepared_free(FilmRawPrepared *p)
{ if(p) {free(p->pixels);*p=(FilmRawPrepared){0};} }

FilmStatus film_raw_grade_canvas(const char *source,const FilmRawPrepared *p,
    const FilmRecipe *recipe,const FilmLut *lut,FilmDevelopCanvas *out)
{
    if (!admitted(source,"film-raw-grade/v1")) return FILM_STATE;
    if (!p || !p->pixels || !shape(p->width,p->height) || !out || film_recipe_validate(recipe)!=FILM_OK) return FILM_INVALID;
    if (recipe->lut_id[0] && recipe->profile.strength && !lut) return FILM_STATE;
    if (lut && (!lut->samples || (lut->size!=17 && lut->size!=33 && lut->size!=65))) return FILM_INVALID;
    FilmDevelopGrade grade=film_develop_grade_prepare(lut,recipe);
    size_t n=(size_t)p->width*p->height;
    if(overlap(out,sizeof *out,p->pixels,n*sizeof *p->pixels)||overlap(out,sizeof *out,recipe,sizeof *recipe)||
       overlap(out,sizeof *out,p,sizeof *p))return FILM_INVALID;
    if(out->pixels)return FILM_INVALID;
    uint16_t *pixels=malloc(n*3*sizeof *pixels);
    if (!pixels) return FILM_NOMEM;
    for (size_t i=0;i<n;++i) {
        FilmRgb q=p->pixels[i];
        if (!isfinite(q.r) || !isfinite(q.g) || !isfinite(q.b)) {free(pixels);return FILM_INVALID;}
        q=film_develop_grade_pixel(q,&grade);
        float v[3]={q.r,q.g,q.b};
        for (unsigned c=0;c<3;++c) {
            if (!isfinite(v[c])) {free(pixels);return FILM_INVALID;}
            pixels[3*i+c]=(uint16_t)lroundf(fminf(fmaxf(v[c],0),1)*65535);
        }
    }
    *out=(FilmDevelopCanvas){p->width,p->height,pixels};return FILM_OK;
}
