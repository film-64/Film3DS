#ifndef FILM_GPU_LAYOUT_H
#define FILM_GPU_LAYOUT_H
/* RGBA8 PICA texture layout, CPU image coordinates with top-left origin.
 * Callers supply valid coordinates and power-of-two dimensions >= 8. */
static inline unsigned film_gpu_texel_offset(unsigned x,unsigned y,unsigned w,unsigned h)
{
    unsigned row=h-1-y,tile=(x>>3)+(row>>3)*(w/8);
    unsigned morton=(x&1)|((row&1)<<1)|((x&2)<<1)|((row&2)<<2)|((x&4)<<2)|((row&4)<<3);
    return (tile*64+morton)*4;
}
#endif
