#include "film.h"
#include "darkroom.h"
#include "film_runtime_contract.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void white_balance(void)
{
    float matrix[3][3],xyz[3];
    assert(film_white_balance_matrix((FilmWhiteBalance){0},matrix)==FILM_OK);
    for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j) assert(matrix[i][j]==(i==j));
    FilmRecipe r=film_default_recipe();
    FilmDevelopGrade g=film_develop_grade_prepare(NULL,&r);
    for (unsigned i=0;i<256;++i) {
        float x=i/255.f;FilmRgb out=film_develop_grade_pixel((FilmRgb){x,x,x},&g);
        assert(fabsf(out.r-x)<1e-6f);
    }
    assert(film_blackbody_white(2856,xyz)==FILM_OK);
    float sum=xyz[0]+xyz[1]+xyz[2];
    assert(fabsf(xyz[0]/sum-.44757f)<.0002f && fabsf(xyz[1]/sum-.40745f)<.0002f);
    for (unsigned k=1000;k<=10000;k+=100) {
        assert(film_blackbody_white(k,xyz)==FILM_OK);
        assert(film_white_balance_matrix((FilmWhiteBalance){k,0,0},matrix)==FILM_OK);
        /* A modeled source illuminant must map to D65 neutral, even when
         * the source white is outside sRGB (1000 K). Test in linear light. */
        float source[]={3.2404542f*xyz[0]-1.5371385f*xyz[1]-.4985314f*xyz[2],
                       -.969266f*xyz[0]+1.8760108f*xyz[1]+.041556f*xyz[2],
                       .0556434f*xyz[0]-.2040259f*xyz[1]+1.0572252f*xyz[2]};
        for (unsigned i=0;i<3;++i) {
            float neutral=0;
            for (unsigned j=0;j<3;++j) { assert(isfinite(matrix[i][j]));neutral+=matrix[i][j]*source[j]; }
            assert(fabsf(neutral-1)<.001f);
        }
        r.white_balance=(FilmWhiteBalance){k,9,-9};g=film_develop_grade_prepare(NULL,&r);
        FilmRgb out=film_develop_grade_pixel((FilmRgb){.5,.3,.1},&g);
        assert(isfinite(out.r)&&isfinite(out.g)&&isfinite(out.b));
    }
    for (int red=-9;red<=9;++red) for (int blue=-9;blue<=9;++blue) {
        FilmRgb out=film_white_balance_swatch(red,blue);
        assert(out.r>=0 && out.r<=1 && out.g>=0 && out.g<=1 && out.b>=0 && out.b<=1);
        assert(red>0 ? out.r>out.g : red<0 ? out.r<out.g : fabsf(out.r-out.g)<1e-6f);
        assert(blue>0 ? out.b>out.g : blue<0 ? out.b<out.g : fabsf(out.b-out.g)<1e-6f);
    }
    assert(film_white_balance_matrix((FilmWhiteBalance){999,0,0},matrix)==FILM_INVALID);
    assert(film_white_balance_matrix((FilmWhiteBalance){10001,0,0},matrix)==FILM_INVALID);
    assert(film_white_balance_matrix((FilmWhiteBalance){6500,10,0},matrix)==FILM_INVALID);
    puts("PASS 91 Kelvin settings, Planck/CIE illuminant A, Bradford neutralization, 361 R/B swatches, identity and invalid ranges");
}

static void geometry(void)
{
    FilmRecipe r=film_default_recipe();FilmRect rect;
    for (unsigned p=100;p>50;--p) assert(film_composition_percent(&r,640,480,-1)==FILM_OK);
    assert(r.crop_percent==50 && film_composition_rect(640,480,&r,&rect)==FILM_OK);
    assert(rect.width==320 && rect.height==240 && rect.x==160 && rect.y==120);
    for (unsigned p=50;p<70;++p) assert(film_composition_percent(&r,640,480,1)==FILM_OK);
    assert(film_composition_rect(640,480,&r,&rect)==FILM_OK && rect.width==448 && rect.height==336);
    unsigned x=rect.x,y=rect.y;
    assert(film_darkroom_pan(&r,640,480,1,-1,false)==FILM_OK && r.crop_x==x+1 && r.crop_y==y-1);
    assert(film_darkroom_pan(&r,640,480,1,-1,true)==FILM_OK && r.crop_x==x+11 && r.crop_y==y-11);
    r.crop_x=640-448-3;
    assert(film_darkroom_pan(&r,640,480,1,0,true)==FILM_OK && r.crop_x==192);
    assert(film_darkroom_pan(&r,640,480,-1000,0,true)==FILM_OK && r.crop_x==182); /* fixed step */
    for (unsigned aspect=0;aspect<4;++aspect) {
        r.aspect=aspect;film_composition_reset(&r);
        assert(film_composition_rect(640,480,&r,&rect)==FILM_OK);
        assert(rect.x==(640-rect.width)/2 && rect.y==(480-rect.height)/2);
        for (unsigned i=0;i<120;++i) assert(film_composition_percent(&r,640,480,-1)==FILM_OK);
        assert(r.crop_percent==1);
        assert(film_composition_pan(&r,640,480,10000,10000)==FILM_OK);
        assert(film_composition_rect(640,480,&r,&rect)==FILM_OK && rect.x+rect.width==640 && rect.y+rect.height==480);
        assert(film_composition_pan(&r,640,480,-10000,-10000)==FILM_OK && !r.crop_x && !r.crop_y);
        assert(film_darkroom_adjust(&r,DARKROOM_ASPECT,1) && !r.crop_percent && !r.crop_positioned);
    }
    r=film_default_recipe();r.crop_percent=50;
    assert(film_composition_rect(320,240,&r,&rect)==FILM_OK && rect.width==160 && rect.height==120);
    r.crop_positioned=true;r.crop_x=161;
    assert(film_composition_rect(320,240,&r,&rect)==FILM_INVALID);
    puts("PASS 1% percentage, 50/70% exact frames, source-pixel movement, fixed 1/10 steps, edge clamp, aspect reset and minimum bounds");
}

static void render_and_storage(void)
{
    FilmFrame frame={.recipe=film_default_recipe()};
    assert(film_image_alloc(&frame.source,64,48,FILM_RGB24)==FILM_OK);
    for (unsigned y=0;y<48;++y) for (unsigned x=0;x<64;++x) {
        unsigned i=(y*64+x)*3;frame.source.pixels[i]=x*4;frame.source.pixels[i+1]=y*5;frame.source.pixels[i+2]=50;
    }
    frame.recipe.crop_percent=50;
    FilmDarkroom room={0};
    assert(film_darkroom_preview(&room,&frame,NULL,NULL)==FILM_OK);
    assert(room.preview.width==64 && room.preview.height==48 && room.canvas.width==64);
    assert(room.preview.pixels[0]>=62 && room.preview.pixels[0]<=66);
    assert(room.composition_view.width==64 && room.composition_view.height==48);
    assert(room.composition_view.pixels[0]==0 && room.composition_view.pixels[3*63]==252);
    uint8_t *full_view=room.composition_view.pixels;
    uint16_t *graded_canvas=room.canvas.pixels;
    FilmImage a={0},b={0};
    assert(film_develop_canvas_render(&room.canvas,&room.recipe,NULL,false,&a)==FILM_OK);
    assert(a.width==64 && a.height==48 && a.pixels[0]>=62 && a.pixels[0]<=66);
    FilmRecipe changed=frame.recipe;changed.crop_positioned=true;changed.crop_x=0;changed.crop_y=0;changed.scale=FILM_200;
    film_darkroom_update_geometry(&room,&changed);
    assert(!room.preview.pixels && room.composition_view.pixels==full_view && room.canvas.pixels==graded_canvas);
    assert(film_darkroom_finish(&room,true,"build/stale-composition-must-not-save.JPG")==FILM_STATE);
    assert(film_develop_canvas_render(&room.canvas,&room.recipe,NULL,false,&b)==FILM_OK);
    assert(b.width==128 && b.height==96 && b.pixels[0]<=2);
    film_image_free(&a);film_image_free(&b);
    changed.aspect=FILM_1_1;film_composition_reset(&changed);film_darkroom_update_geometry(&room,&changed);
    assert(film_develop_canvas_render(&room.canvas,&room.recipe,NULL,true,&a)==FILM_OK && a.width==64 && a.height==48);
    assert(film_develop_canvas_render(&room.canvas,&room.recipe,NULL,false,&b)==FILM_OK && b.width==96 && b.height==96);
    film_image_free(&a);film_image_free(&b);
    const char *path="build/wb-composition-roundtrip.f3s",*saved="build/wb-composition-save.jpg",*expected="build/wb-composition-expected.jpg";
    unlink(path);unlink(saved);unlink(expected);
    frame.recipe.white_balance=(FilmWhiteBalance){4200,4,-3};frame.recipe.crop_positioned=true;frame.recipe.crop_x=7;frame.recipe.crop_y=8;
    assert(film_frame_write(path,&frame)==FILM_OK);
    FilmFrame read={0};assert(film_frame_read(path,&read)==FILM_OK);
    assert(read.recipe.white_balance.kelvin==4200 && read.recipe.white_balance.red==4 && read.recipe.white_balance.blue==-3);
    assert(read.recipe.crop_percent==50 && read.recipe.crop_x==7 && read.recipe.crop_y==8 && read.recipe.crop_positioned);
    FILE *f=fopen(path,"rb");assert(f);unsigned char magic[8];assert(fread(magic,1,8,f)==8 && magic[4]==6);fclose(f);
    film_frame_free(&read);
    frame.recipe.crop_x=100;assert(film_frame_write(path,&frame)==FILM_INVALID);
    frame.recipe.crop_x=7;
    assert(film_develop_with_options(&frame,NULL,NULL,&frame.source)==FILM_INVALID);
    frame.recipe=changed;
    assert(film_darkroom_preview(&room,&frame,NULL,NULL)==FILM_OK);
    assert(room.preview.width==96 && room.preview.height==96);
    assert(room.composition_view.width==64 && room.composition_view.height==48);
    /* Geometry is baked at A; SELECT preserves even a cached preview whose
     * original canvas/recipe can no longer be rendered. */
    assert(film_jpeg_write(expected,&room.preview,room.quality)==FILM_OK);
    film_develop_canvas_free(&room.canvas);
    memset(frame.source.pixels,255,64*48*3);
    assert(film_darkroom_finish(&room,true,saved)==FILM_OK);
    FILE *fa=fopen(saved,"rb"),*fb=fopen(expected,"rb");assert(fa&&fb);int ca,cb;
    do {ca=fgetc(fa);cb=fgetc(fb);assert(ca==cb);}while(ca!=EOF);fclose(fa);fclose(fb);
    assert(film_darkroom_preview(&room,&frame,NULL,NULL)==FILM_OK);
    assert(film_darkroom_finish(&room,false,NULL)==FILM_OK && !room.canvas.pixels);
    film_frame_free(&frame);unlink(path);unlink(saved);unlink(expected);
    puts("PASS full original composition view, edit preserves frame canvas, cached crop save, original coordinates, immutable cache, v6 roundtrip, OOB and alias rejection, cancel cleanup");
}
int main(void)
{
    assert(film_runtime_source_valid("film-darkroom-wb-preview/v1"));
    assert(!film_runtime_source_valid("film-darkroom-wb-preview/unknown"));
    white_balance();geometry();render_and_storage();return 0;
}
