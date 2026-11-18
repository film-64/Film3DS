#include "exposure_matrix.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void exercise(void)
{
    FilmExposureMatrix m=film_exposure_matrix_default();
    m.id=12;m.source_crc=UINT32_C(0x1234abcd);
    m.exposure_mode=0;m.native_level=2;
    m.preview_valid=true;
    m.preview_linear[0]=.04f;m.preview_linear[1]=.35f;m.preview_linear[2]=.8f;
    enum {SIDE=32};
    FilmRgb *pixels=calloc(SIDE*SIDE,sizeof *pixels);assert(pixels);
    for(unsigned y=0;y<SIDE;++y)for(unsigned x=0;x<SIDE;++x){
        float v=.02f+.75f*(float)x/(SIDE-1);
        pixels[y*SIDE+x]=(FilmRgb){v,v,v};
    }
    FilmRawScene scene={SIDE,SIDE,pixels};
    assert(film_exposure_matrix_fit(&m,&scene)==FILM_OK);
    assert(m.fitted);
    assert(fabsf(m.neutral.ev)<1e-7f&&fabsf(m.neutral.shadows)<1e-7f);
    assert(m.shot.ev>-.99f&&m.shot.ev<1.5f);
    assert(m.shot.shadows>=-1.5f&&m.shot.shadows<=1.5f);
    assert(m.shot.highlights>=-1.5f&&m.shot.highlights<=1.5f);
    assert(film_exposure_matrix_fit(&m,&scene)==FILM_INVALID);
    FilmRecipe recipe=film_default_recipe();
    FilmDevelopOptions options={.raw_shadows=1,.raw_midtones=-.5f,.raw_local=.5f};
    recipe.output_tone.value[FILM_TONE_MID]=.2f;
    assert(film_exposure_curve_apply(&m.shot,&recipe,&options)==FILM_OK);
    assert(fabsf(options.raw_shadows-m.shot.shadows)<1e-7f);
    assert(options.raw_local==0);
    assert(recipe.output_tone.value[FILM_TONE_MID]==.2f);
    assert(film_exposure_curve_apply(&m.neutral,&recipe,&options)==FILM_OK);
    assert(recipe.input_tone.value[FILM_TONE_EV]==0);
    assert(options.raw_shadows==0&&options.raw_midtones==0&&options.raw_highlights==0&&options.raw_shoulder==0);
    char dir[]="/tmp/film-exposure-XXXXXX";assert(mkdtemp(dir));
    assert(film_exposure_matrix_write(dir,&m)==FILM_OK);
    FilmExposureMatrix out={0};
    assert(film_exposure_matrix_read(dir,12,UINT32_C(0x1234abcd),&out)==FILM_OK);
    assert(out.fitted&&out.id==12&&out.native_level==2&&out.source_crc==m.source_crc);
    assert(fabsf(out.shot.ev-m.shot.ev)<1e-7f);
    assert(film_exposure_matrix_read(dir,12,UINT32_C(0x99999999),&out)==FILM_CORRUPT);
    assert(film_exposure_matrix_read(dir,13,m.source_crc,&out)==FILM_IO);
    char name[256];snprintf(name,sizeof name,"%s/ROLL0012.F3E",dir);
    FILE *file=fopen(name,"r+b");assert(file);
    assert(fseek(file,65,SEEK_SET)==0);
    assert(fputc(0xff,file)!=EOF);
    assert(fclose(file)==0);
    assert(film_exposure_matrix_read(dir,12,m.source_crc,&out)==FILM_CORRUPT);
    assert(unlink(name)==0&&rmdir(dir)==0);
    m=film_exposure_matrix_default();m.preview_valid=true;
    m.preview_linear[0]=m.preview_linear[1]=m.preview_linear[2]=.2f;
    for(unsigned i=0;i<SIDE*SIDE;++i)pixels[i]=(FilmRgb){0,0,0};
    assert(film_exposure_matrix_fit(&m,&scene)==FILM_STATE);
    assert(!m.fitted);
    pixels[0].r=NAN;
    /* Sample grid uses the center of each cell; force that actual sample. */
    pixels[SIDE+1].r=NAN;
    assert(film_exposure_matrix_fit(&m,&scene)==FILM_CORRUPT);
    free(pixels);
}
int main(void){exercise();puts("exposure matrix: OK");return 0;}
