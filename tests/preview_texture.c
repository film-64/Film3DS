#include "preview_texture.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    unsigned w=0,h=0;
    film_preview_texture_dims(640,480,&w,&h);assert(w==256&&h==192);
    film_preview_texture_dims(480,480,&w,&h);assert(w==192&&h==192);
    film_preview_texture_dims(640,360,&w,&h);assert(w==256&&h==144);
    film_preview_texture_dims(600,400,&w,&h);assert(w==256&&h==171);
    film_preview_texture_dims(300,400,&w,&h);assert(w==144&&h==192);
    film_preview_texture_dims(0,0,&w,&h);assert(w==256&&h==192);
    film_preview_texture_dims(4096,1,&w,&h);assert(w==256&&h>=1);
    puts("Darkroom image aperture: native ratio for 4:3, 1:1, 16:9, 3:2, 3:4 OK");
    return 0;
}
