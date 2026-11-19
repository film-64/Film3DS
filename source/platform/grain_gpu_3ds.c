#include "platform_develop.h"
#include "film_runtime_contract.h"
#include "gpu_layout.h"
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {SIDE=FILM_GPU_COMPOSE_SIDE,CORE=FILM_GPU_COMPOSE_CORE,MAPS=6};
/* Main-thread owned; reuse across tiles and A previews. Nothing is retained
 * from the caller after a render. Release while Citro3D is still active. */
static struct {
    C3D_Tex maps[MAPS],gain,photo;
    C3D_RenderTarget *gain_target,*output_target;
    uint8_t *readback;
    float *planes,*sigma,*luma;
    uint8_t *zero_mask,*response_code;
    size_t luma_capacity;
    size_t capacity;
    unsigned allocated;
    bool gain_ready,ready,photo_ready;
} gpu_workspace;

static unsigned offset(unsigned x,unsigned y)
{return film_gpu_texel_offset(x,y,SIDE,SIDE);}
void film_3ds_develop_release(void)
{
    if(gpu_workspace.gain_target)C3D_RenderTargetDelete(gpu_workspace.gain_target);
    if(gpu_workspace.output_target)C3D_RenderTargetDelete(gpu_workspace.output_target);
    if(gpu_workspace.gain_ready)C3D_TexDelete(&gpu_workspace.gain);
    for(unsigned i=0;i<gpu_workspace.allocated;++i)C3D_TexDelete(gpu_workspace.maps+i);
    if(gpu_workspace.readback)linearFree(gpu_workspace.readback);
    if(gpu_workspace.photo_ready)C3D_TexDelete(&gpu_workspace.photo);
    free(gpu_workspace.luma);free(gpu_workspace.zero_mask);free(gpu_workspace.response_code);
    free(gpu_workspace.planes);free(gpu_workspace.sigma);
    memset(&gpu_workspace,0,sizeof gpu_workspace);
}
static FilmStatus reserve(size_t count)
{
    if(!gpu_workspace.ready){
        for(;gpu_workspace.allocated<MAPS;++gpu_workspace.allocated){
            C3D_Tex *t=gpu_workspace.maps+gpu_workspace.allocated;
            GPU_TEXCOLOR format=gpu_workspace.allocated<3||gpu_workspace.allocated==5?GPU_L8:
                gpu_workspace.allocated==4?GPU_RGB8:GPU_RGBA8;
            if(!C3D_TexInit(t,SIDE,SIDE,format))goto fail;
            C3D_TexSetFilter(t,GPU_NEAREST,GPU_NEAREST);
            C3D_TexSetWrap(t,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
        }
        gpu_workspace.gain_ready=C3D_TexInitVRAM(&gpu_workspace.gain,SIDE,SIDE,GPU_RGBA8);
        if(!gpu_workspace.gain_ready)goto fail;
        C3D_TexSetFilter(&gpu_workspace.gain,GPU_NEAREST,GPU_NEAREST);
        C3D_TexSetWrap(&gpu_workspace.gain,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
        gpu_workspace.gain_target=C3D_RenderTargetCreateFromTex(&gpu_workspace.gain,GPU_TEXFACE_2D,0,-1);
        gpu_workspace.output_target=C3D_RenderTargetCreate(SIDE,SIDE,GPU_RB_RGBA8,-1);
        gpu_workspace.readback=linearMemAlign(SIDE*SIDE*4,0x80);
        if(!gpu_workspace.gain_target||!gpu_workspace.output_target||!gpu_workspace.readback)goto fail;
        gpu_workspace.ready=true;
    }
    if(count>gpu_workspace.capacity){
        float *planes=malloc(count*3*sizeof *planes),*sigma=malloc(count*sizeof *sigma);
        uint8_t *mask=malloc(count),*tone=malloc(count);
        if(!planes||!sigma||!mask||!tone){free(planes);free(sigma);free(mask);free(tone);return FILM_NOMEM;}
        free(gpu_workspace.planes);free(gpu_workspace.sigma);
        free(gpu_workspace.zero_mask);free(gpu_workspace.response_code);gpu_workspace.zero_mask=mask;gpu_workspace.response_code=tone;
        gpu_workspace.planes=planes;gpu_workspace.sigma=sigma;gpu_workspace.capacity=count;
    }
    return FILM_OK;
fail:
    film_3ds_develop_release();return FILM_NOMEM;
}
static uint8_t code(float v){return (uint8_t)lroundf(fminf(fmaxf(v,0),1)*255);}
static void rgba(unsigned map,unsigned x,unsigned y,uint8_t r,uint8_t g,uint8_t b)
{
    unsigned index=offset(x,y)/4;uint8_t *p=gpu_workspace.maps[map].data;
    if(map<3||map==5)p[index]=r;
    else if(map==4){p+=index*3;p[0]=b;p[1]=g;p[2]=r;}
    else{p+=index*4;p[0]=255;p[1]=b;p[2]=g;p[3]=r;}
}
static FilmStatus upload(const FilmImage *im,unsigned x0,unsigned y0,unsigned tw,unsigned th,bool resident_photo)
{
    for(unsigned y=0;y<SIDE;++y)for(unsigned x=0;x<SIDE;++x){
        bool valid=x<tw&&y<th;size_t i=(size_t)(y0+y)*im->width+x0+x;
        const float zero[3]={0};uint8_t masks[5];
        FilmStatus status=film_grain_gpu_masks("film-grain-gpu-compose/v1",valid?gpu_workspace.planes+3*i:zero,
            valid?gpu_workspace.sigma[i]:0,valid?gpu_workspace.response_code[i]:0,masks);
        if(status!=FILM_OK)return status;
        for(unsigned layer=0;layer<3;++layer)rgba(layer,x,y,masks[layer],masks[layer],masks[layer]);
        const uint8_t *pixel=valid?im->pixels+3*i:NULL;
        if(!resident_photo)rgba(3,x,y,pixel?pixel[0]:0,pixel?pixel[1]:0,pixel?pixel[2]:0);
        /* sRGB power segment: (C + .055) * gain - .055.
         * PICA handles final RGB multiply/add/subtract. The exact CPU density
         * backend remains available; the toe and RGBA8 rounding are approximate. */
        uint8_t positive=masks[3],negative=masks[4];
        unsigned mask=valid?(resident_photo?gpu_workspace.zero_mask[i]:
            (unsigned)(!pixel[0])|((unsigned)(!pixel[1])<<1)|((unsigned)(!pixel[2])<<2)):7;
        rgba(4,x,y,mask&1?0:positive,mask&2?0:positive,mask&4?0:positive);
        rgba(5,x,y,negative,negative,negative);
    }
    for(unsigned i=0;i<MAPS;++i)if(!resident_photo||i!=3)C3D_TexFlush(gpu_workspace.maps+i);
    return FILM_OK;
}
static bool draw_texture(C3D_Tex *texture)
{
    const Tex3DS_SubTexture sub={SIDE,SIDE,0,1,1,0};
    C3D_TexBind(0,texture);
    bool ok=C2D_DrawImageAt((C2D_Image){texture,&sub},0,0,0,NULL,1,1);
    C2D_Flush();return ok;
}
static void blend(GPU_BLENDEQUATION equation,GPU_BLENDFACTOR src,GPU_BLENDFACTOR dst)
{C3D_AlphaBlend(equation,GPU_BLEND_ADD,src,dst,GPU_ONE,GPU_ZERO);}
static FilmStatus render_tile(FilmImage *out,unsigned x0,unsigned y0,unsigned tw,unsigned th,const FilmRect *source,bool grain)
{
    if(R_FAILED(GSPGPU_FlushDataCache(gpu_workspace.readback,SIDE*SIDE*4))||!C3D_FrameBegin(0))return FILM_DEVICE;
    C2D_Prepare();bool ok=true;
    if(grain){
    C2D_TargetClear(gpu_workspace.gain_target,C2D_Color32(128,128,128,255));
    C2D_SceneBegin(gpu_workspace.gain_target);
    for(unsigned i=0;i<3;++i){
        blend(GPU_BLEND_ADD,GPU_DST_COLOR,GPU_SRC_COLOR);
        ok=draw_texture(gpu_workspace.maps+i)&&ok;
    }
    /* Submit producer commands and flush its color buffer before sampling the
     * resident gain texture. No CPU readback between the two targets. */
    C3D_FrameSplit(0);
    }
    C2D_TargetClear(gpu_workspace.output_target,0);C2D_SceneBegin(gpu_workspace.output_target);
    blend(GPU_BLEND_ADD,GPU_ONE,GPU_ZERO);
    if(source){
        Tex3DS_SubTexture sub={source->width,source->height,0,1,
            source->width/(float)gpu_workspace.photo.width,1-source->height/(float)gpu_workspace.photo.height};
        C3D_TexBind(0,&gpu_workspace.photo);
        ok=C2D_DrawImageAt((C2D_Image){&gpu_workspace.photo,&sub},-(float)x0,-(float)y0,0,NULL,
            out->width/(float)source->width,out->height/(float)source->height)&&ok;C2D_Flush();
    }else ok=draw_texture(gpu_workspace.maps+3)&&ok;
    if(grain){
    blend(GPU_BLEND_ADD,GPU_DST_COLOR,GPU_SRC_COLOR);ok=draw_texture(&gpu_workspace.gain)&&ok;
    blend(GPU_BLEND_ADD,GPU_ONE,GPU_ONE);ok=draw_texture(gpu_workspace.maps+4)&&ok;
    blend(GPU_BLEND_REVERSE_SUBTRACT,GPU_ONE,GPU_ONE);ok=draw_texture(gpu_workspace.maps+5)&&ok;
    }
    blend(GPU_BLEND_ADD,GPU_ONE,GPU_ZERO);
    /* Three non-collinear markers outside the 254x254 output core test BOTH
     * directions. Never infer image axes from the user-selected LUT/grain. */
    ok=C2D_DrawRectSolid(255,0,0,1,1,C2D_Color32(255,0,0,255))&&ok;
    ok=C2D_DrawRectSolid(0,255,0,1,1,C2D_Color32(0,255,0,255))&&ok;
    ok=C2D_DrawRectSolid(255,255,0,1,1,C2D_Color32(0,0,255,255))&&ok;C2D_Flush();
    C3D_SyncDisplayTransfer(gpu_workspace.output_target->frameBuf.colorBuf,GX_BUFFER_DIM(SIDE,SIDE),
        (u32 *)gpu_workspace.readback,GX_BUFFER_DIM(SIDE,SIDE),
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8));
    blend(GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA);C3D_FrameEnd(0);
    if(!C3D_FrameBegin(0))return FILM_DEVICE;
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    if(!ok||R_FAILED(GSPGPU_InvalidateDataCache(gpu_workspace.readback,SIDE*SIDE*4)))return FILM_DEVICE;
    FilmGpuReadbackAxes axes={0};
    if(!film_gpu_readback_axes(gpu_workspace.readback,SIDE,&axes))return FILM_DEVICE;
    for(unsigned y=0;y<th;++y)for(unsigned x=0;x<tw;++x){
        const uint8_t *src=film_gpu_readback_pixel(gpu_workspace.readback,SIDE,x,y,axes);
        uint8_t *dst=out->pixels+((size_t)(y0+y)*out->width+x0+x)*3;
        dst[0]=src[3];dst[1]=src[2];dst[2]=src[1]; /* Layout copy only; no CPU blend. */
    }
    return FILM_OK;
}
FilmStatus film_3ds_grain_layered(const char *source,FilmImage *im,const FilmLayeredGrain *p,
    const FilmGrainProfile *profile,const FilmGrainTexture *texture)
{
    if(!source||strcmp(source,"film-grain-layered/v1")||!film_runtime_source_valid(source)||
       !film_runtime_source_valid("film-grain-gpu-compose/v1"))return FILM_STATE;
    if(!im||!im->pixels||im->format!=FILM_RGB24||!film_image_bytes(im->width,im->height,im->format)||!p)return FILM_INVALID;
    if(!p->amount)return film_grain_layered_mix("film-grain-layered/v1",im,p,profile,texture,NULL);
    FilmStatus s=reserve((size_t)im->width*im->height);if(s!=FILM_OK)return s;
    s=film_grain_response_prepare("film-grain-gpu-compose/v1",im,p,profile,texture,gpu_workspace.planes,gpu_workspace.sigma,gpu_workspace.response_code);
    if(s!=FILM_OK||!p->amount)return s;
    FilmImage result={0};s=film_image_alloc(&result,im->width,im->height,FILM_RGB24);
    for(unsigned y=0;s==FILM_OK&&y<im->height;y+=CORE)for(unsigned x=0;s==FILM_OK&&x<im->width;x+=CORE){
        unsigned w=im->width-x<CORE?im->width-x:CORE,h=im->height-y<CORE?im->height-y:CORE;
        s=upload(im,x,y,w,h,false);if(s==FILM_OK)s=render_tile(&result,x,y,w,h,NULL,true);
    }
    C3D_TexBind(0,NULL);
    if(s==FILM_OK){FilmImage previous=*im;*im=result;result=previous;}
    film_image_free(&result);return s;
}

static unsigned power2(unsigned n){unsigned v=8;while(v<n)v*=2;return v;}
static unsigned photo_offset(unsigned x,unsigned y,unsigned w,unsigned h)
{return film_gpu_texel_offset(x,y,w,h);}
static float srgb(float x){return x<=.0031308f?12.92f*x:1.055f*powf(x,1/2.4f)-.055f;}
static FilmStatus prepare_photo(const FilmDevelopCanvas *canvas,FilmRect crop,FilmImage *response,bool grain)
{
    unsigned w=power2(crop.width),h=power2(crop.height);
    if(w>1024||h>1024)return FILM_UNSUPPORTED;
    if(!gpu_workspace.photo_ready||gpu_workspace.photo.width!=w||gpu_workspace.photo.height!=h){
        if(gpu_workspace.photo_ready)C3D_TexDelete(&gpu_workspace.photo);
        gpu_workspace.photo_ready=C3D_TexInit(&gpu_workspace.photo,w,h,GPU_RGBA8);
        if(!gpu_workspace.photo_ready)return FILM_NOMEM;
        C3D_TexSetFilter(&gpu_workspace.photo,GPU_LINEAR,GPU_LINEAR);
        C3D_TexSetWrap(&gpu_workspace.photo,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    }
    size_t count=(size_t)crop.width*crop.height;
    if(grain&&count>gpu_workspace.luma_capacity){
        float *luma=malloc(count*sizeof *luma);if(!luma)return FILM_NOMEM;
        free(gpu_workspace.luma);gpu_workspace.luma=luma;gpu_workspace.luma_capacity=count;
    }
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        unsigned sx=x<crop.width?x:crop.width-1,sy=y<crop.height?y:crop.height-1;
        const uint16_t *pixel=canvas->pixels+((size_t)(crop.y+sy)*canvas->width+crop.x+sx)*3;
        uint8_t *dst=(uint8_t *)gpu_workspace.photo.data+photo_offset(x,y,w,h);
        dst[0]=255;dst[1]=(pixel[2]+128u)/257u;dst[2]=(pixel[1]+128u)/257u;dst[3]=(pixel[0]+128u)/257u;
        if(grain&&x<crop.width&&y<crop.height){
            const uint8_t rgb[3]={dst[3],dst[2],dst[1]};
            gpu_workspace.luma[(size_t)y*crop.width+x]=film_grain_linear_luma(rgb);
        }
    }
    C3D_TexFlush(&gpu_workspace.photo);
    if(!grain)return FILM_OK;
    /* Only scalar response is resized on CPU. The RGB photograph is sampled
     * and resized on GPU without an intermediate image readback. */
    for(unsigned y=0;y<response->height;++y)for(unsigned x=0;x<response->width;++x){
        float fx=fmaxf((x+.5f)*crop.width/response->width-.5f,0),fy=fmaxf((y+.5f)*crop.height/response->height-.5f,0);
        unsigned x0=(unsigned)fx,y0=(unsigned)fy,x1=x0+1<crop.width?x0+1:x0,y1=y0+1<crop.height?y0+1:y0;
        fx-=x0;fy-=y0;
        float *l=gpu_workspace.luma;
        float value=(1-fy)*((1-fx)*l[(size_t)y0*crop.width+x0]+fx*l[(size_t)y0*crop.width+x1])+
            fy*((1-fx)*l[(size_t)y1*crop.width+x0]+fx*l[(size_t)y1*crop.width+x1]);
        size_t i=(size_t)y*response->width+x;uint8_t c=code(srgb(value));
        response->pixels[3*i]=response->pixels[3*i+1]=response->pixels[3*i+2]=c;
        unsigned mask=0;
        for(unsigned channel=0;channel<3;++channel){
            bool zero=true;
            for(unsigned yy=y0;yy<=y1;++yy)for(unsigned xx=x0;xx<=x1;++xx)
                if(canvas->pixels[((size_t)(crop.y+yy)*canvas->width+crop.x+xx)*3+channel])zero=false;
            if(zero)mask|=1u<<channel;
        }
        gpu_workspace.zero_mask[i]=(uint8_t)mask;
    }
    return FILM_OK;
}
FilmStatus film_3ds_canvas_output(const char *source,const FilmDevelopCanvas *canvas,const FilmRecipe *recipe,
    const FilmDevelopOptions *options,FilmImage *out)
{
    if(!source||strcmp(source,"film-darkroom-gpu-output/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!canvas||!canvas->pixels||!recipe||!options||!out||out->pixels||film_recipe_validate(recipe)!=FILM_OK)return FILM_INVALID;
    /* Explicit CPU stages for controls that fixed-function sampling cannot
     * reproduce. GPU errors never cause a hidden CPU retry. */
    if(recipe->filter!=FILM_BILINEAR||recipe->sharpness||!options->layered_grain){
        FilmDevelopOptions partition=*options;partition.output_renderer=NULL;
        return film_develop_canvas_render(canvas,recipe,&partition,false,out);
    }
    FilmRect crop,target;unsigned w,h;
    FilmStatus s=film_composition_rect(canvas->width,canvas->height,recipe,&crop);
    if(s==FILM_OK)s=film_scaled_dims(canvas->width,canvas->height,recipe->scale,&w,&h);
    if(s==FILM_OK)s=film_crop_rect(w,h,recipe->aspect,&target);
    if(s!=FILM_OK)return s;
    if(crop.width>1024||crop.height>1024)return FILM_UNSUPPORTED;
    s=reserve(recipe->grain_amount?(size_t)target.width*target.height:1);if(s!=FILM_OK)return s;
    FilmImage result={0};s=film_image_alloc(&result,target.width,target.height,FILM_RGB24);
    bool grain=recipe->grain_amount!=0;
    if(s==FILM_OK)s=prepare_photo(canvas,crop,&result,grain);
    FilmLayeredGrain p={.amount=recipe->grain_amount,.size=options->grain_size,
        .coarse=options->grain==FILM_GRAIN_FINE?.2f:options->grain==FILM_GRAIN_COARSE?.8f:.5f,
        .shadow=options->grain_shadow,.mid=options->grain_mid,.highlight=options->grain_highlight,
        .reference_width=crop.width,.seed=recipe->grain_seed};
    if(s==FILM_OK&&grain)s=film_grain_response_prepare("film-grain-gpu-compose/v1",&result,&p,options->grain_profile,
        options->grain_texture,gpu_workspace.planes,gpu_workspace.sigma,gpu_workspace.response_code);
    for(unsigned y=0;s==FILM_OK&&y<result.height;y+=CORE)for(unsigned x=0;s==FILM_OK&&x<result.width;x+=CORE){
        unsigned tw=result.width-x<CORE?result.width-x:CORE,th=result.height-y<CORE?result.height-y:CORE;
        if(grain)s=upload(&result,x,y,tw,th,true);
        if(s==FILM_OK)s=render_tile(&result,x,y,tw,th,&crop,grain);
    }
    C3D_TexBind(0,NULL);
    if(s==FILM_OK)*out=result;else film_image_free(&result);
    return s;
}
