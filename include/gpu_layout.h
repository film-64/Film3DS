#ifndef FILM_GPU_LAYOUT_H
#define FILM_GPU_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Readback orientation is a property of the 3DS render target, NOT the photo
 * or LUT. Three distinct corner sentinels lie outside the 254x254 output
 * core. Match both axes; the previous two right-edge markers only detected
 * vertical reversal and could silently mirror an entire GPU grain export. */
typedef struct {bool flip_x,flip_y;} FilmGpuReadbackAxes;
static inline const uint8_t *film_gpu_readback_pixel(const uint8_t *data,unsigned side,
    unsigned x,unsigned y,FilmGpuReadbackAxes axes)
{
    return data+4*((size_t)(axes.flip_y?side-1-y:y)*side+
                   (axes.flip_x?side-1-x:x));
}
static inline bool film_gpu_marker(const uint8_t *p,unsigned channel)
{
    /* GPU_RGBA8 transfer readback: alpha, B, G, R. */
    return p[0]>240 && p[3-channel]>240 &&
           p[3-((channel+1)%3)]<16 && p[3-((channel+2)%3)]<16;
}
static inline bool film_gpu_readback_axes(const uint8_t *data,unsigned side,
    FilmGpuReadbackAxes *axes)
{
    if(!data||!axes||side<8)return false;
    unsigned found=0;
    FilmGpuReadbackAxes selected={0};
    for(unsigned fx=0;fx<2;++fx)for(unsigned fy=0;fy<2;++fy){
        FilmGpuReadbackAxes test={fx!=0,fy!=0};
        const uint8_t *red=film_gpu_readback_pixel(data,side,side-1,0,test);
        const uint8_t *green=film_gpu_readback_pixel(data,side,0,side-1,test);
        const uint8_t *blue=film_gpu_readback_pixel(data,side,side-1,side-1,test);
        if(film_gpu_marker(red,0)&&film_gpu_marker(green,1)&&film_gpu_marker(blue,2)){
            ++found;selected=test;
        }
    }
    if(found!=1)return false;
    *axes=selected;
    return true;
}
/* RGBA8 PICA texture layout, CPU image coordinates with top-left origin.
 * Callers supply valid coordinates and power-of-two dimensions >= 8. */
static inline unsigned film_gpu_texel_offset(unsigned x,unsigned y,unsigned w,unsigned h)
{
    unsigned row=h-1-y,tile=(x>>3)+(row>>3)*(w/8);
    unsigned morton=(x&1)|((row&1)<<1)|((x&2)<<1)|((row&2)<<2)|((x&4)<<2)|((row&4)<<3);
    return (tile*64+morton)*4;
}
#endif
