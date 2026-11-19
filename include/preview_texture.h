#ifndef FILM_PREVIEW_TEXTURE_H
#define FILM_PREVIEW_TEXTURE_H
#include <stdint.h>
/* Fit non-camera output into the 256x192 texture aperture WITHOUT changing
 * its aspect. 3DS camera preview remains the fixed native 4:3 path. */
static inline void film_preview_texture_dims(unsigned width,unsigned height,
                                              unsigned *tw,unsigned *th)
{
    if(!tw||!th)return;
    if(!width||!height){*tw=256;*th=192;return;}
    if((uint64_t)width*192 >= (uint64_t)height*256){
        *tw=256;*th=(unsigned)(((uint64_t)height*256+width/2)/width);
    }else{
        *th=192;*tw=(unsigned)(((uint64_t)width*192+height/2)/height);
    }
    if(!*tw)*tw=1;if(!*th)*th=1;
}
#endif
