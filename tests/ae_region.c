#include "ae_region.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    FilmAeRegion r=film_ae_region_default();
    assert(film_ae_region_valid(r)&&film_ae_region_is_full(r));
    for(unsigned i=1;i<FILM_AE_LEVELS;i++){
        r=film_ae_region_resize(r,1);
        assert(film_ae_region_valid(r));
    }
    assert(r.width==80&&r.height==60);
    for(unsigned j=0;j<100;j++){
        r=film_ae_region_move(r,1,1);
        assert(film_ae_region_valid(r));
    }
    assert(r.x==560&&r.y==420);
    for(unsigned j=0;j<100;j++){
        r=film_ae_region_move(r,-1,-1);
        assert(film_ae_region_valid(r));
    }
    assert(r.x==0&&r.y==0);
    for(unsigned i=0;i<6;i++)r=film_ae_region_resize(r,-1);
    assert(film_ae_region_is_full(r));
    assert(!film_ae_region_valid((FilmAeRegion){0,0,319,240}));
    puts("ae region: resize, full-frame, clamp and 4:3 sensor bounds OK");
    return 0;
}
