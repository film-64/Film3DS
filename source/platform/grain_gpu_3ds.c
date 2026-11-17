#include "platform_develop.h"
#include "film_runtime_contract.h"
#include <3ds.h>
#include <citro2d.h>
#include <math.h>

enum { SIDE=256 };
static unsigned offset(unsigned x,unsigned y)
{
    unsigned row=SIDE-1-y,tile=(x>>3)+(row>>3)*(SIDE/8);
    unsigned morton=(x&1)|((row&1)<<1)|((x&2)<<1)|((row&2)<<2)|((x&4)<<2)|((row&4)<<3);
    return (tile*64+morton)*4;
}
static FilmStatus gpu_mix(unsigned w,unsigned h,const float *planes,float *noise)
{
    if(!film_runtime_source_valid("film-grain-gpu-mix/v1"))return FILM_STATE;
    float scale=0;FilmStatus encoding=film_grain_gpu_encoding("film-grain-gpu-mix/v1",planes,(size_t)w*h,&scale);
    if(encoding!=FILM_OK)return encoding;
    C3D_Tex textures[3];unsigned allocated=0;
    for(;allocated<3;++allocated)if(!C3D_TexInit(textures+allocated,SIDE,SIDE,GPU_RGBA8))break;
    C3D_RenderTarget *target=allocated==3?C3D_RenderTargetCreate(SIDE,SIDE,GPU_RB_RGBA8,-1):NULL;
    uint8_t *readback=target?linearMemAlign(SIDE*SIDE*4,0x80):NULL;
    FilmStatus status=readback?FILM_OK:FILM_NOMEM;
    for(unsigned i=0;i<allocated;++i){C3D_TexSetFilter(textures+i,GPU_NEAREST,GPU_NEAREST);C3D_TexSetWrap(textures+i,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);}
    const Tex3DS_SubTexture sub={SIDE,SIDE,0,1,1,0};
    for(unsigned y0=0;status==FILM_OK&&y0<h;y0+=SIDE)for(unsigned x0=0;status==FILM_OK&&x0<w;x0+=SIDE){
        unsigned tw=w-x0<SIDE?w-x0:SIDE,th=h-y0<SIDE?h-y0:SIDE;
        for(unsigned layer=0;layer<3;++layer){
            uint8_t *tex=textures[layer].data;
            for(unsigned y=0;y<SIDE;++y)for(unsigned x=0;x<SIDE;++x){
                float v=x<tw&&y<th?planes[((size_t)(y0+y)*w+x0+x)*3+layer]:0;
                float code=42+scale*v;
                if(!isfinite(code)||code<0||code>255){status=FILM_INVALID;break;}
                uint8_t c=(uint8_t)lroundf(code);unsigned i=offset(x,y);
                tex[i]=255;tex[i+1]=tex[i+2]=tex[i+3]=c;
            }
            C3D_TexFlush(textures+layer);
        }
        if(status!=FILM_OK)break;
        if(R_FAILED(GSPGPU_FlushDataCache(readback,SIDE*SIDE*4))||!C3D_FrameBegin(0)){status=FILM_DEVICE;break;}
        C2D_TargetClear(target,0);C2D_SceneBegin(target);C2D_Prepare();
        bool drawn=true;
        for(unsigned layer=0;layer<3;++layer){
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ONE,GPU_ONE,GPU_ZERO);
            C3D_TexBind(0,textures+layer);
            drawn=C2D_DrawImageAt((C2D_Image){textures+layer,&sub},0,0,0,NULL,1,1)&&drawn;
            C2D_Flush();
        }
        C3D_SyncDisplayTransfer(target->frameBuf.colorBuf,GX_BUFFER_DIM(SIDE,SIDE),(u32 *)readback,GX_BUFFER_DIM(SIDE,SIDE),
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8));
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
        C3D_FrameEnd(0);
        if(!C3D_FrameBegin(0)){status=FILM_DEVICE;break;}C3D_FrameEnd(GX_CMDLIST_FLUSH);
        if(!drawn||R_FAILED(GSPGPU_InvalidateDataCache(readback,SIDE*SIDE*4))){status=FILM_DEVICE;break;}
        double errors[2]={0};
        for(unsigned flip=0;flip<2;++flip)for(unsigned y=0;y<th;++y)for(unsigned x=0;x<tw;++x){
            size_t i=((size_t)(y0+y)*w+x0+x)*3;
            float expected=planes[i]+planes[i+1]+planes[i+2];
            float actual=((int)readback[((flip?SIDE-1-y:y)*SIDE+x)*4+3]-126)/scale;
            errors[flip]+=fabsf(actual-expected);
        }
        bool flip=errors[1]<errors[0];
        for(unsigned y=0;y<th;++y)for(unsigned x=0;x<tw;++x){
            size_t i=(size_t)(y0+y)*w+x0+x;
            float actual=((int)readback[((flip?SIDE-1-y:y)*SIDE+x)*4+3]-126)/scale;
            if(fabsf(actual-planes[3*i]-planes[3*i+1]-planes[3*i+2])>.4f)status=FILM_DEVICE;
            noise[i]=actual;
        }
    }
    C3D_TexBind(0,NULL);
    if(readback)linearFree(readback);
    if(target)C3D_RenderTargetDelete(target);
    for(unsigned i=0;i<allocated;++i)C3D_TexDelete(textures+i);
    return status;
}
FilmStatus film_3ds_grain_layered(const char *source,FilmImage *image,const FilmLayeredGrain *params,
    const FilmGrainProfile *profile,const FilmGrainTexture *texture)
{return film_grain_layered_mix(source,image,params,profile,texture,gpu_mix);}
