#include "gpu_layout.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
enum {S=256};
static void put(uint8_t *data,unsigned x,unsigned y,FilmGpuReadbackAxes axes,
    uint8_t r,uint8_t g,uint8_t b)
{
    uint8_t *p=(uint8_t *)film_gpu_readback_pixel(data,S,x,y,axes);
    p[0]=255;p[1]=b;p[2]=g;p[3]=r;
}
int main(void)
{
    uint8_t *data=calloc(S*S,4);assert(data);
    for(unsigned fx=0;fx<2;++fx)for(unsigned fy=0;fy<2;++fy){
        memset(data,0,S*S*4);
        FilmGpuReadbackAxes sent={!!fx,!!fy},found={0};
        put(data,S-1,0,sent,255,0,0);
        put(data,0,S-1,sent,0,255,0);
        put(data,S-1,S-1,sent,0,0,255);
        /* Red at upper right must never be mistaken for upper left. */
        put(data,8,12,sent,143,51,22);
        put(data,130,77,sent,25,88,208);
        assert(film_gpu_readback_axes(data,S,&found));
        assert(found.flip_x==sent.flip_x&&found.flip_y==sent.flip_y);
        const uint8_t *a=film_gpu_readback_pixel(data,S,8,12,found);
        const uint8_t *b=film_gpu_readback_pixel(data,S,130,77,found);
        assert(a[3]==143&&a[2]==51&&a[1]==22);
        assert(b[3]==25&&b[2]==88&&b[1]==208);
        /* A missing sentinel must fail closed, not silently mirror a photo. */
        put(data,S-1,S-1,sent,0,0,0);
        assert(!film_gpu_readback_axes(data,S,&found));
    }
    free(data);puts("GPU grain readback: all 4 axes and asymmetric pixels OK");return 0;
}
