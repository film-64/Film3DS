#include "film.h"
#include "raw_darkroom.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char cube[]="TITLE \"adapter\"\r\nLUT_3D_SIZE 2\r\n"
    "0 0 0\r\n1 0 0\r\n0 1 0\r\n1 1 0\r\n0 0 1\r\n1 0 1\r\n0 1 1\r\n1 1 1";
static void write_file(const char *path, const char *text)
{
    FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite(text,1,strlen(text),f)==strlen(text)); assert(!fclose(f));
}
static uint32_t reference_crc(const char *text)
{
    uint32_t crc=UINT32_MAX;
    for (size_t i=0;i<strlen(text);++i) {
        crc^=(unsigned char)text[i];
        for (unsigned b=0;b<8;++b) crc=(crc>>1)^((crc&1)?UINT32_C(0xedb88320):0);
    }
    return ~crc;
}
static void metadata(const char *path, const char *text, const char *input,
                     const char *output, const char *policy, const char *suffix)
{
    char buffer[1024];
    snprintf(buffer,sizeof buffer,"F3LUT1\ncube_bytes %zu\ncube_crc32 %08x\ninput %s\noutput %s\npre_lut_shoulder %s\n%s",
             strlen(text),reference_crc(text),input,output,policy,suffix);
    write_file(path,buffer);
}
static void expect_failure(const char *path, FilmStatus expected)
{
    FilmLut out={.size=123},before=out;
    assert(film_lut_load_path(path,&out,NULL)==expected);
    assert(!memcmp(&out,&before,sizeof out));
}
static void loading(void)
{
    char dir[]="build/lut-adapter.XXXXXX"; assert(mkdtemp(dir));
    char path[256],sidecar[270]; snprintf(path,sizeof path,"%s/look.cube",dir);
    snprintf(sidecar,sizeof sidecar,"%s.f3a",path); write_file(path,cube);
    FilmLut lut={0};
    assert(film_lut_load_path(path,&lut,NULL)==FILM_OK && !lut.has_adapter && !lut.has_transfer);
    assert(lut.source_bytes==strlen(cube) && lut.source_crc32==reference_crc(cube));
    film_lut_free(&lut);
    metadata(sidecar,cube,"FLOG2C","SRGB","bypass","");
    assert(film_lut_load_path(path,&lut,NULL)==FILM_OK && lut.has_adapter && lut.has_transfer);
    assert(lut.input==FILM_FLOG2C && lut.output==FILM_SRGB && lut.bypass_raw_shoulder);
    film_lut_free(&lut);
    metadata(sidecar,cube,"LINEAR_SRGB","LINEAR_SRGB","preserve","");
    assert(film_lut_load_path(path,&lut,NULL)==FILM_OK && !lut.bypass_raw_shoulder); film_lut_free(&lut);
    metadata(sidecar,cube,"LOG","SRGB","preserve",""); expect_failure(path,FILM_UNSUPPORTED);
    metadata(sidecar,cube,"SRGB","UNKNOWN","preserve",""); expect_failure(path,FILM_UNSUPPORTED);
    metadata(sidecar,cube,"SRGB","SRGB","auto",""); expect_failure(path,FILM_UNSUPPORTED);
    metadata(sidecar,cube,"SRGB","SRGB","preserve","unknown 1\n"); expect_failure(path,FILM_CORRUPT);
    metadata(sidecar,cube,"SRGB","SRGB","preserve","input SRGB\n"); expect_failure(path,FILM_CORRUPT);
    write_file(sidecar,"F3LUT2\n"); expect_failure(path,FILM_CORRUPT);
    write_file(sidecar,"F3LUT1\ncube_bytes -1\n"); expect_failure(path,FILM_CORRUPT);
    metadata(sidecar,cube,"SRGB","SRGB","preserve","");
    char changed[sizeof cube]; memcpy(changed,cube,sizeof cube); changed[7]='X';
    write_file(path,changed); expect_failure(path,FILM_CORRUPT); /* same size, different contents */
    write_file(path,cube);
    FILE *f=fopen(sidecar,"wb"); assert(f);
    fputs("F3LUT1\ncube_bytes ",f); fputc(0,f); fputs("99\n",f); fclose(f);
    expect_failure(path,FILM_CORRUPT);
    /* An existing but unreadable-as-file sidecar cannot become an absent one. */
    assert(!unlink(sidecar)); assert(!mkdir(sidecar,0700));
    FilmLut out={0}; assert(film_lut_load_path(path,&out,NULL)!=FILM_OK && !out.samples);
    assert(!rmdir(sidecar));
    char annotated[512]; snprintf(annotated,sizeof annotated,"# FILM3DS_INPUT SRGB\n# FILM3DS_OUTPUT SRGB\n%s",cube);
    write_file(path,annotated); metadata(sidecar,annotated,"FLOG2C","SRGB","preserve","");
    expect_failure(path,FILM_CORRUPT);
    metadata(sidecar,annotated,"SRGB","SRGB","preserve","");
    assert(film_lut_load_path(path,&lut,NULL)==FILM_OK); film_lut_free(&lut);
    assert(!unlink(sidecar) && !unlink(path) && !rmdir(dir));
    puts("PASS exact CUBE binding, CRLF/no final newline, absent/conflicting/stale/corrupt metadata and unchanged failure output");
}

static float encoded(float x)
{ return x<=.0031308f ? x*12.92f : 1.055f*powf(x,1/2.4f)-.055f; }
static void pipelines(void)
{
    FilmRgb samples[8];
    /* Noncommuting mapping: swap R/B, divide by four, add 0.1.
     * DOMAIN is 0..4, so samples are encoded linear outputs at each corner. */
    unsigned n=0;
    for (unsigned b=0;b<2;++b) for (unsigned g=0;g<2;++g) for (unsigned r=0;r<2;++r)
        samples[n++]=(FilmRgb){b+.1f,g+.1f,r+.1f};
    FilmLut lut={.size=2,.max={4,4,4},.samples=samples,.has_transfer=true,
        .input=FILM_LINEAR_SRGB,.output=FILM_LINEAR_SRGB,.has_adapter=true,.bypass_raw_shoulder=true};
    FilmRgb pixels[16],original[16];
    for (unsigned i=0;i<16;++i) pixels[i]=(FilmRgb){.2f,.1f,.05f};
    memcpy(original,pixels,sizeof pixels); FilmRawScene scene={4,4,pixels};
    FilmRecipe recipe=film_default_recipe(); recipe.grain_amount=0; recipe.base_saturation=0;
    recipe.input_tone.value[FILM_TONE_EV]=1; recipe.white_balance=(FilmWhiteBalance){6500,2,-1};
    /* Default profile says sRGB: explicit asset encoding must prevail in the CORE. */
    assert(recipe.profile.input==FILM_SRGB);
    FilmDevelopOptions opts={.raw_shoulder=.75f}; FilmDevelopCanvas canvas={0},neutral={0};
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,&lut,&opts,&canvas)==FILM_OK);
    float wb[3][3],v[3]={.2f,.1f,.05f},balanced[3]={0};
    assert(film_white_balance_matrix(recipe.white_balance,wb)==FILM_OK);
    for (unsigned c=0;c<3;++c) for (unsigned j=0;j<3;++j) balanced[c]+=wb[c][j]*v[j]*2;
    float expected[3]={balanced[2]/4+.1f,balanced[1]/4+.1f,balanced[0]/4+.1f};
    for (unsigned c=0;c<3;++c) assert(abs((int)canvas.pixels[c]-(int)lroundf(encoded(expected[c])*65535))<=3);
    opts.raw_shoulder=0;
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,&lut,&opts,&neutral)==FILM_OK);
    assert(!memcmp(canvas.pixels,neutral.pixels,16*3*2));
    film_develop_canvas_free(&canvas); film_develop_canvas_free(&neutral);
    /* Expose a superwhite scene through a compression LUT: disabling the knee
     * preserves input headroom; preserve keeps the old appearance. */
    for (unsigned i=0;i<16;++i) pixels[i]=(FilmRgb){2,2,2};
    recipe.white_balance=(FilmWhiteBalance){0}; recipe.input_tone.value[FILM_TONE_EV]=0;
    opts.raw_shoulder=.75f;
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,&lut,&opts,&canvas)==FILM_OK);
    lut.bypass_raw_shoulder=false;
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,&lut,&opts,&neutral)==FILM_OK);
    assert(canvas.pixels[0]>neutral.pixels[0]+10000);
    film_develop_canvas_free(&canvas); film_develop_canvas_free(&neutral);
    lut.bypass_raw_shoulder=true; recipe.profile.strength=0;
    assert(!film_lut_bypasses_raw_shoulder(&lut,&recipe));
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,&lut,&opts,&canvas)==FILM_OK);
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,NULL,&opts,&neutral)==FILM_OK);
    assert(!memcmp(canvas.pixels,neutral.pixels,16*3*2));
    film_develop_canvas_free(&canvas); film_develop_canvas_free(&neutral);
    memcpy(pixels,original,sizeof pixels); recipe.profile.strength=1;
    recipe.output_tone.value[FILM_TONE_MID]=.5f;
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,&lut,&opts,&canvas)==FILM_OK);
    recipe.output_tone.value[FILM_TONE_MID]=0;
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,&lut,&opts,&neutral)==FILM_OK);
    assert(canvas.pixels[0]>neutral.pixels[0]); assert(!memcmp(pixels,original,sizeof pixels));
    film_develop_canvas_free(&canvas); film_develop_canvas_free(&neutral);
    puts("PASS core metadata precedence, WB/exposure before LUT, explicit knee policy, disabled LUT neutrality, post-curve and immutable scene");
}

int main(void) { loading(); pipelines(); return 0; }
