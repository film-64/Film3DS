#include "raw_darkroom.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void verify(FilmRgb color)
{
    FilmRgb pixels[64],before[64];
    /* Vary brightness over two stops without changing chromaticity. */
    for(unsigned i=0;i<64;++i){float f=.25f+.75f*i/63;pixels[i]=(FilmRgb){color.r*f,color.g*f,color.b*f};}
    memcpy(before,pixels,sizeof pixels);
    FilmRawScene scene={8,8,pixels};FilmNeutralPatch patch={0,0,8,8,.001f,.99f};
    FilmNeutralBalance result={0};assert(film_raw_scene_neutral(&scene,&patch,&result)==FILM_OK);
    assert(result.accepted==64&&result.rejected==0&&result.spread_stops<1e-5);
    float matrix[3][3],v[3]={color.r,color.g,color.b},out[3]={0};
    assert(film_white_balance_matrix(result.balance,matrix)==FILM_OK);
    for(unsigned c=0;c<3;++c)for(unsigned j=0;j<3;++j)out[c]+=matrix[c][j]*v[j];
    assert(fabsf(log2f(out[0]/out[1]))<=.08001f&&fabsf(log2f(out[2]/out[1]))<=.08001f);
    assert(!memcmp(before,pixels,sizeof pixels));
    FilmRecipe recipe=film_default_recipe();recipe.white_balance=result.balance;recipe.grain_amount=0;
    recipe.input_tone.value[FILM_TONE_EV]=0;recipe.base_saturation=0;recipe.profile.strength=0;
    FilmDevelopCanvas canvas={0};
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,NULL,NULL,&canvas)==FILM_OK);
    assert(canvas.pixels);
    for(unsigned i=0;i<64;++i){
        assert(abs((int)canvas.pixels[i*3]-(int)canvas.pixels[i*3+1])<2000);
        assert(abs((int)canvas.pixels[i*3+2]-(int)canvas.pixels[i*3+1])<2000);
    }
    film_develop_canvas_free(&canvas);
}
int main(void)
{
    verify((FilmRgb){.2f,.2f,.2f});verify((FilmRgb){.24f,.2f,.17f});verify((FilmRgb){.12f,.2f,.3f});
    FilmRgb pixels[16];for(unsigned i=0;i<16;++i)pixels[i]=(FilmRgb){.2f,.2f,.2f};
    FilmRawScene scene={4,4,pixels};FilmNeutralPatch patch={0,0,4,4,.01f,.95f};
    FilmNeutralBalance result={0};assert(film_raw_scene_neutral(&scene,&patch,&result)==FILM_OK);
    assert(result.balance.kelvin==0&&result.balance.red==0&&result.balance.blue==0);
    pixels[0].r=NAN;pixels[1].g=0;pixels[2].b=1;pixels[3].r=-1;
    assert(film_raw_scene_neutral(&scene,&patch,&result)==FILM_OK&&result.accepted==12&&result.rejected==4);
    FilmNeutralBalance saved=result;
    pixels[4].r=INFINITY;assert(film_raw_scene_neutral(&scene,&patch,&result)==FILM_INVALID);
    assert(!memcmp(&saved,&result,sizeof result));
    patch.x=~0u;assert(film_raw_scene_neutral(&scene,&patch,&result)==FILM_INVALID);patch.x=0;
    patch.width=2;patch.height=2;assert(film_raw_scene_neutral(&scene,&patch,&result)==FILM_INVALID);
    patch.width=patch.height=4;
    for(unsigned i=0;i<16;++i)pixels[i]=(FilmRgb){i%2?.4f:.1f,.2f,.2f};
    assert(film_raw_scene_neutral(&scene,&patch,&result)==FILM_INVALID);
    for(unsigned i=0;i<16;++i)pixels[i]=(FilmRgb){.01f,.9f,.01f};
    assert(film_raw_scene_neutral(&scene,&patch,&result)==FILM_UNSUPPORTED);
    assert(!memcmp(&saved,&result,sizeof result));
    assert(film_raw_scene_neutral(&scene,&patch,(FilmNeutralBalance *)pixels)==FILM_INVALID);
    patch.minimum=NAN;assert(film_raw_scene_neutral(&scene,&patch,&result)==FILM_INVALID);
    puts("neutral balance: OK");return 0;
}
