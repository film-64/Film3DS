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
{ return (FilmRawIsp){.cfa=FILM_RGGB,.white=1023,.local_limit_ev=2,.wb={1,1,1},.ccm={1,0,0,0,1,0,0,0,1}}; }

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

FilmStatus film_raw_bad_pixel_clean(const char *source,uint16_t *samples,unsigned w,unsigned h)
{
    if(!admitted(source,"film-raw-bad-pixel/v1"))return FILM_STATE;
    if(!samples||!shape(w,h))return FILM_INVALID;
    size_t count=(size_t)w*h;
    for(size_t i=0;i<count;++i)if(samples[i]>1023)return FILM_CORRUPT;
    uint16_t *original=malloc(count*sizeof *original);if(!original)return FILM_NOMEM;
    memcpy(original,samples,count*sizeof *original);
    for(unsigned y=2;y+2<h;++y)for(unsigned x=2;x+2<w;++x){
        float neighbors[8];unsigned n=0;uint16_t lo=1023,hi=0;
        for(int dy=-2;dy<=2;dy+=2)for(int dx=-2;dx<=2;dx+=2){
            if(!dx&&!dy)continue;
            uint16_t v=original[(size_t)((int)y+dy)*w+(unsigned)((int)x+dx)];
            neighbors[n++]=v;if(v<lo)lo=v;if(v>hi)hi=v;
        }
        if((unsigned)(hi-lo)>FILM_RAW_BAD_PIXEL_RANGE)continue; /* Protect edges, texture and uncertain clusters. */
        float center=median(neighbors,8);
        if(fabsf(original[(size_t)y*w+x]-center)>=FILM_RAW_BAD_PIXEL_DIFFERENCE)
            samples[(size_t)y*w+x]=(uint16_t)lroundf(center);
    }
    free(original);return FILM_OK;
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

/* Spatial base is computed on a small log-luminance grid. Bilateral range
 * weights avoid spreading exposure across strong scene edges. A shared gain
 * preserves RGB ratios; the EV bound prevents unbounded shadow amplification.
 * This adjusts rendering only: it cannot recover saturated sensor samples. */
static FilmStatus local_exposure(FilmRgb *rgb,unsigned w,unsigned h,const FilmRawIsp *p)
{
    if (!p->local_strength || !p->local_limit_ev) return FILM_OK;
    unsigned gw=(w+31)/32,gh=(h+31)/32;
    float *grid=calloc((size_t)gw*gh,sizeof *grid),*gains=malloc((size_t)gw*gh*sizeof *gains);
    if (!grid || !gains) {free(grid);free(gains);return FILM_NOMEM;}
    for(unsigned by=0;by<gh;++by)for(unsigned bx=0;bx<gw;++bx){
        float sum=0;unsigned count=0;
        for(unsigned y=by*32;y<h && y<(by+1)*32;++y)for(unsigned x=bx*32;x<w && x<(bx+1)*32;++x){
            FilmRgb a=rgb[(size_t)y*w+x];
            sum+=log2f(fmaxf(.2126f*a.r+.7152f*a.g+.0722f*a.b,1e-6f));++count;
        }
        grid[by*gw+bx]=sum/count;
    }
    for(unsigned y=0;y<gh;++y)for(unsigned x=0;x<gw;++x){
        float sum=0,weight=0,center=grid[y*gw+x];
        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){
            int xx=(int)x+dx,yy=(int)y+dy;if(xx<0||yy<0||xx>=(int)gw||yy>=(int)gh)continue;
            float a=grid[yy*gw+xx],d=a-center;
            float k=expf(-.25f*(dx*dx+dy*dy)-.5f*d*d);
            sum+=a*k;weight+=k;
        }
        float ev=p->local_strength*(log2f(.18f)-sum/weight);
        gains[y*gw+x]=fminf(fmaxf(ev,-p->local_limit_ev),p->local_limit_ev);
    }
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        float fx=fminf(fmaxf(((float)x-15.5f)/32,0),gw-1),fy=fminf(fmaxf(((float)y-15.5f)/32,0),gh-1);
        unsigned x0=(unsigned)fx,y0=(unsigned)fy,x1=x0+1<gw?x0+1:x0,y1=y0+1<gh?y0+1:y0;
        fx-=x0;fy-=y0;
        float ev=(1-fy)*((1-fx)*gains[y0*gw+x0]+fx*gains[y0*gw+x1])+fy*((1-fx)*gains[y1*gw+x0]+fx*gains[y1*gw+x1]);
        float gain=exp2f(ev);FilmRgb *a=&rgb[(size_t)y*w+x];a->r*=gain;a->g*=gain;a->b*=gain;
    }
    free(grid);free(gains);return FILM_OK;
}

static FilmStatus prepare(const char *source,const FilmRaw10View *raw,
    const FilmRawIsp *p,FilmRawPrepared *out,bool scene)
{
    if (!admitted(source,scene?"film-raw-scene/v1":"film-raw-isp/v1")) return FILM_STATE;
    if (!raw || !raw->samples || !p || !out || !shape(raw->width,raw->height) ||
        (unsigned)p->cfa>FILM_GBRG || !range(p->white,1,1023) || !range(p->exposure_ev,-8,8) ||
        !range(p->shoulder,0,.99f) || !range(p->local_strength,0,1) || !range(p->local_limit_ev,0,4) ||
        p->bad_pixel_count>FILM_RAW_MAX_BAD_PIXELS || (p->bad_pixel_count && !p->bad_pixels)) return FILM_INVALID;
    for (unsigned i=0;i<4;++i) if (!range(p->black[i],0,p->white-1)) return FILM_INVALID;
    for (unsigned i=0;i<3;++i) if (!range(p->wb[i],.125f,8) || !range(p->ccm_offset[i],-1,1)) return FILM_INVALID;
    for (unsigned i=0;i<9;++i) if (!range(p->ccm[i],-8,8)) return FILM_INVALID;
    bool shading=false;
    for(unsigned c=0;c<3;++c)for(unsigned i=0;i<6;++i){
        if(!range(p->lens_shading[c][i],-4,4))return FILM_INVALID;
        if(p->lens_shading[c][i])shading=true;
    }
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
        if(shading){
            float xx=2.f*x/(w-1)-1,yy=2.f*y/(h-1)-1;
            float basis[6]={1,xx,yy,xx*xx,xx*yy,yy*yy};
            for(unsigned c=0;c<3;++c){float ev=0;for(unsigned j=0;j<6;++j)ev+=p->lens_shading[c][j]*basis[j];
                v[c]*=exp2f(fminf(fmaxf(ev,-2),2));}
        }
        float a[3];
        for (unsigned c=0;c<3;++c) a[c]=p->ccm[3*c]*v[0]+p->ccm[3*c+1]*v[1]+p->ccm[3*c+2]*v[2]+p->ccm_offset[c];
        if(!scene)for(unsigned c=0;c<3;++c)a[c]=fmaxf(0,a[c]);
        rgb[(size_t)y*w+x]=(FilmRgb){a[0],a[1],a[2]};
    }
    free(work);
    if(scene){*out=(FilmRawPrepared){w,h,rgb};return FILM_OK;}
    FilmStatus status=local_exposure(rgb,w,h,p);
    if(status!=FILM_OK){free(rgb);return status;}
    for(size_t i=0;i<n;++i){
        float a[3]={rgb[i].r,rgb[i].g,rgb[i].b};
        float peak=fmaxf(a[0],fmaxf(a[1],a[2])),gain=1;
        if (p->shoulder && peak>p->shoulder) {
            float k=p->shoulder;
            gain=(k+(1-k)*(1-expf(-(peak-k)/(1-k))))/peak;
        }
        rgb[i]=(FilmRgb){encode(a[0]*gain),encode(a[1]*gain),encode(a[2]*gain)};
    }
    *out=(FilmRawPrepared){w,h,rgb};return FILM_OK;
}

FilmStatus film_raw_isp_prepare(const char *source,const FilmRaw10View *raw,const FilmRawIsp *p,FilmRawPrepared *out)
{ return prepare(source,raw,p,out,false); }
FilmStatus film_raw_scene_prepare(const char *source,const FilmRaw10View *raw,const FilmRawIsp *p,FilmRawScene *out)
{
    if(!admitted(source,"film-raw-scene/v1"))return FILM_STATE;
    if(!out)return FILM_INVALID;
    if(out->pixels)return FILM_INVALID;
    /* Separate public types avoid accidentally treating linear data as sRGB. */
    FilmRawPrepared result={0};
    if(overlap(out,sizeof *out,raw?raw->samples:NULL,raw?(size_t)raw->width*raw->height*sizeof(uint16_t):0)||
       overlap(out,sizeof *out,p,p?sizeof *p:0)||overlap(out,sizeof *out,raw,raw?sizeof *raw:0))return FILM_INVALID;
    FilmStatus s=prepare(source,raw,p,&result,true);
    if(s==FILM_OK)*out=(FilmRawScene){result.width,result.height,result.pixels};
    return s;
}
void film_raw_scene_free(FilmRawScene *p)
{if(p){free(p->pixels);*p=(FilmRawScene){0};}}
FilmStatus film_raw_scene_preview_half(const FilmRawScene *src,FilmRawScene *out)
{
    if(!src||!out||src==(const FilmRawScene *)out||out->pixels||!src->pixels||
       !src->width||!src->height||
       (size_t)src->width>FILM_RAW_MAX_PIXELS/src->height)return FILM_INVALID;
    unsigned w=(src->width+1)/2,h=(src->height+1)/2;
    FilmRgb *pixels=malloc((size_t)w*h*sizeof *pixels);
    if(!pixels)return FILM_NOMEM;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        FilmRgb sum={0};unsigned count=0;
        for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){
            unsigned xx=x*2+dx,yy=y*2+dy;
            if(xx>=src->width||yy>=src->height)continue;
            FilmRgb v=src->pixels[(size_t)yy*src->width+xx];
            sum.r+=v.r;sum.g+=v.g;sum.b+=v.b;++count;
        }
        pixels[(size_t)y*w+x]=(FilmRgb){sum.r/count,sum.g/count,sum.b/count};
    }
    *out=(FilmRawScene){w,h,pixels};
    return FILM_OK;
}

/* Scene curve in stops relative to 18% gray. Positive segment slopes for all
 * legal controls: knots six stops apart, offsets at most +/-2.5 EV. */
static float scene_gain(float y,const FilmDevelopOptions *o)
{
    if(!o || (!o->raw_shadows&&!o->raw_midtones&&!o->raw_highlights))return 1;
    float x=log2f(fmaxf(y,1e-10f)/.18f),offset;
    if(x<=-6)offset=o->raw_shadows;
    else if(x<0)offset=o->raw_shadows+(o->raw_midtones-o->raw_shadows)*(x+6)/6;
    else if(x<6)offset=o->raw_midtones+(o->raw_highlights-o->raw_midtones)*x/6;
    else offset=o->raw_highlights;
    return exp2f(offset);
}

FilmStatus film_raw_scene_grade(const char *source,const FilmRawScene *scene,const FilmRecipe *recipe,
    const FilmLut *lut,const FilmDevelopOptions *o,FilmDevelopCanvas *out)
{
    if(!admitted(source,"film-raw-scene-grade/v1"))return FILM_STATE;
    if(!scene||!scene->pixels||!shape(scene->width,scene->height)||!out||film_recipe_validate(recipe)!=FILM_OK)return FILM_INVALID;
    if(out->pixels || overlap(out,sizeof *out,scene->pixels,(size_t)scene->width*scene->height*sizeof(FilmRgb)) ||
       overlap(out,sizeof *out,scene,sizeof *scene)||overlap(out,sizeof *out,recipe,sizeof *recipe)||
       overlap(out,sizeof *out,o,o?sizeof *o:0))return FILM_INVALID;
    if(o&&(!range(o->raw_shadows,-2.5f,2.5f)||!range(o->raw_midtones,-2.5f,2.5f)||!range(o->raw_highlights,-2.5f,2.5f)||
       !range(o->raw_shoulder,0,.99f)||!range(o->raw_local,0,1)||!range(o->raw_local_limit,0,4)))return FILM_INVALID;
    size_t n=(size_t)scene->width*scene->height;
    FilmRawPrepared display={scene->width,scene->height,malloc(n*sizeof(FilmRgb))};if(!display.pixels)return FILM_NOMEM;
    float wb[3][3];FilmStatus status=film_white_balance_matrix(recipe->white_balance,wb);
    float exposure=exp2f(recipe->input_tone.value[FILM_TONE_EV]);
    for(size_t i=0;status==FILM_OK&&i<n;++i){
        FilmRgb a=scene->pixels[i];float v[3]={a.r,a.g,a.b},b[3];
        for(unsigned c=0;c<3;++c){
            if(!isfinite(v[c])){status=FILM_INVALID;break;}
            b[c]=(wb[c][0]*v[0]+wb[c][1]*v[1]+wb[c][2]*v[2])*exposure;
        }
        if(status!=FILM_OK)break;
        float y=.2126f*b[0]+.7152f*b[1]+.0722f*b[2],gain=scene_gain(y,o);
        display.pixels[i]=(FilmRgb){b[0]*gain,b[1]*gain,b[2]*gain};
    }
    FilmRawIsp local=film_raw_isp_default();
    if(o){local.local_strength=o->raw_local;local.local_limit_ev=o->raw_local_limit;}
    if(status==FILM_OK)status=local_exposure(display.pixels,scene->width,scene->height,&local);
    float knee=o?o->raw_shoulder:0;
    for(size_t i=0;status==FILM_OK&&i<n;++i){
        FilmRgb a=display.pixels[i];float peak=fmaxf(a.r,fmaxf(a.g,a.b)),gain=1;
        if(knee&&peak>knee)gain=(knee+(1-knee)*(1-expf(-(peak-knee)/(1-knee))))/peak;
        display.pixels[i]=(FilmRgb){encode(a.r*gain),encode(a.g*gain),encode(a.b*gain)};
    }
    if(status==FILM_OK&&o&&o->halation)status=film_raw_prepared_halation(&display);
    if(status==FILM_OK&&o)status=film_darkroom_denoise("film-darkroom-denoise/v1",display.pixels,scene->width,scene->height,o->chroma_denoise);
    FilmRecipe final=*recipe;final.white_balance=(FilmWhiteBalance){0};final.input_tone.value[FILM_TONE_EV]=0;final.base_saturation=0;
    if(status==FILM_OK)status=film_raw_grade_canvas("film-raw-grade/v1",&display,&final,lut,out);
    film_raw_prepared_free(&display);return status;
}

void film_raw_prepared_free(FilmRawPrepared *p)
{ if(p) {free(p->pixels);*p=(FilmRawPrepared){0};} }

FilmStatus film_raw_grade_canvas(const char *source,const FilmRawPrepared *p,
    const FilmRecipe *recipe,const FilmLut *lut,FilmDevelopCanvas *out)
{
    if (!admitted(source,"film-raw-grade/v1")) return FILM_STATE;
    if (!p || !p->pixels || !shape(p->width,p->height) || !out || film_recipe_validate(recipe)!=FILM_OK) return FILM_INVALID;
    if (recipe->lut_id[0] && recipe->profile.strength && !lut) return FILM_STATE;
    if (lut && (!lut->samples || (lut->size<2 || lut->size>256))) return FILM_INVALID;
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
