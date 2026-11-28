#include "color_encoding.h"
#include "raw_darkroom.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void near(float actual, float expected, float tolerance)
{
    if (!isfinite(actual) || fabsf(actual - expected) > tolerance)
        fprintf(stderr, "encoding: %.9g != %.9g (tolerance %.9g)\n", actual, expected, tolerance);
    assert(isfinite(actual) && fabsf(actual - expected) <= tolerance);
}

static void rgb_near(FilmRgb a, FilmRgb b, float tolerance)
{
    near(a.r, b.r, tolerance); near(a.g, b.g, tolerance); near(a.b, b.b, tolerance);
}

static void reference_points(void)
{
    /* Frozen independent double-precision references. Matrix independently
     * derived from the published xy primaries + D65, not the production matrix.
     * Fuji v1.0 also gives rounded 10-bit points: black 95, 18% 400, 90% 570. */
    const FilmRgb linear[] = {{0,0,0}, {.18f,.18f,.18f}, {.9f,.9f,.9f},
                              {1,0,0}, {0,1,0}, {0,0,1}};
    const FilmRgb expected[] = {
        {.092864f,.092864f,.092864f},
        {.391007241891f,.391007241891f,.391007241891f},
        {.557132363979f,.557132363979f,.557132363979f},
        {.499099721149f,.322007142033f,.191367972988f},
        {.475735050181f,.545964741651f,.342111003780f},
        {.300127284468f,.335462759690f,.553906079983f}
    };
    for (unsigned i=0; i<sizeof linear/sizeof *linear; ++i) {
        rgb_near(film_color_encode_linear(linear[i], FILM_FLOG2C), expected[i], 1e-6f);
        rgb_near(film_color_decode_linear(expected[i], FILM_FLOG2C), linear[i], 2e-6f);
    }
    rgb_near(film_color_encode_linear((FilmRgb){.18f,.18f,.18f}, FILM_SRGB),
             (FilmRgb){.4613561295f,.4613561295f,.4613561295f}, 1e-6f);
    rgb_near(film_color_encode_linear((FilmRgb){.18f,.18f,.18f}, FILM_REC709),
             (FilmRgb){.4090077289f,.4090077289f,.4090077289f}, 1e-6f);
    const FilmColorEncoding *e=film_color_encoding(FILM_FLOG2C);
    assert(e && e->gamut==FILM_GAMUT_FGAMUT_C_D65 && e->curve==FILM_CURVE_FLOG2);
    puts("PASS encoding reference gray points, independent color primaries and inverse gamut");
}

static void range_and_validation(void)
{
    const FilmTransfer encodings[]={FILM_SRGB,FILM_LINEAR_SRGB,FILM_REC709,FILM_FLOG2C};
    const FilmRgb samples[]={{-.02f,.1f,.3f},{0,0,0},{.000888f,.000888f,.000888f},
        {.000889f,.000889f,.000889f},{.000890f,.000890f,.000890f},
        {.0031308f,.01799f,.018f},{.18f,.18f,.18f},{1,2,4},{8,16,32}};
    for (unsigned e=0;e<4;++e) {
        for (unsigned i=0;i<sizeof samples/sizeof *samples;++i) {
            FilmRgb coded={0},back={0};
            assert(film_color_convert(samples[i],FILM_LINEAR_SRGB,encodings[e],&coded)==FILM_OK);
            assert(film_color_convert(coded,encodings[e],FILM_LINEAR_SRGB,&back)==FILM_OK);
            float max=fmaxf(fabsf(samples[i].r),fmaxf(fabsf(samples[i].g),fabsf(samples[i].b)));
            rgb_near(back,samples[i],3e-6f*fmaxf(1,max));
        }
    }
    /* Gray ramp spans -12..+8 stops relative to 18%; Log retains superwhite. */
    float previous=-INFINITY;
    for (int i=-1200;i<=800;++i) {
        float v=.18f*exp2f(i/100.f);
        FilmRgb log=film_color_encode_linear((FilmRgb){v,v,v},FILM_FLOG2C);
        assert(log.r>previous); previous=log.r;
    }
    FilmRgb out={3,4,5},saved=out;
    assert(film_color_convert((FilmRgb){NAN,0,0},FILM_SRGB,FILM_FLOG2C,&out)==FILM_INVALID);
    assert(film_color_convert((FilmRgb){0,INFINITY,0},FILM_SRGB,FILM_FLOG2C,&out)==FILM_INVALID);
    assert(film_color_convert((FilmRgb){0,0,0},FILM_VENDOR_LOG,FILM_SRGB,&out)==FILM_UNSUPPORTED);
    assert(film_color_convert((FilmRgb){0,0,0},FILM_SRGB,(FilmTransfer)99,&out)==FILM_UNSUPPORTED);
    assert(film_color_convert((FilmRgb){FLT_MAX,0,0},FILM_FLOG2C,FILM_SRGB,&out)==FILM_INVALID);
    assert(!memcmp(&out,&saved,sizeof out));
    assert(!film_color_encoding((FilmTransfer)-1));
    FilmTransfer id=FILM_REC709;
    assert(!film_color_encoding_parse("LOG",&id)&&id==FILM_REC709);
    assert(!film_color_encoding_parse(NULL,&id));
    assert(film_color_convert(saved,FILM_SRGB,FILM_SRGB,&out)==FILM_OK);
    assert(!memcmp(&out,&saved,sizeof out));
    puts("PASS negative/superwhite, breakpoints, Log ramp, unchanged failure outputs and exact identity");
}

static FilmLut log_identity(void)
{
    FILE *file=tmpfile(); assert(file);
    fputs("# FILM3DS_INPUT FLOG2C\n# FILM3DS_OUTPUT FLOG2C\nLUT_3D_SIZE 2\n",file);
    for (unsigned b=0;b<2;++b) for (unsigned g=0;g<2;++g) for (unsigned r=0;r<2;++r)
        fprintf(file,"%u %u %u\n",r,g,b);
    rewind(file); FilmLut lut={0};
    assert(film_lut_load(file,&lut,NULL)==FILM_OK); fclose(file);
    return lut;
}

static void production_pipeline(void)
{
    FilmLut lut=log_identity();
    FilmRecipe recipe=film_default_recipe();
    recipe.grain_amount=0; recipe.base_saturation=0;
    recipe.profile.input=lut.input; recipe.profile.output=lut.output;
    assert(film_profile_validate(&recipe.profile)==FILM_OK);
    const char *profile_path="build/color-encoding-roundtrip.f3p";
    assert(film_profile_write(profile_path,&recipe.profile)==FILM_OK);
    FilmProfile loaded={0};
    assert(film_profile_read(profile_path,&loaded)==FILM_OK);
    assert(loaded.input==FILM_FLOG2C && loaded.output==FILM_FLOG2C && loaded.strength==recipe.profile.strength);
    assert(!remove(profile_path));
    const FilmRgb colors[]={{.2f,.4f,.7f},{.7f,.2f,.1f},{.46135613f,.46135613f,.46135613f}};
    for (unsigned i=0;i<3;++i) for (unsigned strength=0;strength<=2;++strength) {
        recipe.profile.strength=strength/2.f;
        FilmDevelopGrade grade=film_develop_grade_prepare(&lut,&recipe);
        rgb_near(film_develop_grade_pixel(colors[i],&grade),colors[i],3e-6f);
    }
    recipe.profile.strength=1;
    /* A constant Log result must be decoded even when the input is sRGB. */
    for (unsigned i=0;i<8;++i) lut.samples[i]=(FilmRgb){.391007241891f,.391007241891f,.391007241891f};
    recipe.profile.input=FILM_SRGB;
    FilmDevelopGrade grade=film_develop_grade_prepare(&lut,&recipe);
    rgb_near(film_develop_grade_pixel((FilmRgb){.2f,.3f,.4f},&grade),
             (FilmRgb){.46135613f,.46135613f,.46135613f},3e-6f);
    film_lut_free(&lut);

    /* The actual RAW scene -> RGB16 canvas path uses the same adapter.
     * No knee/creative curve: a neutral Log LUT must preserve exposure + WB. */
    lut=log_identity(); recipe.profile.input=recipe.profile.output=FILM_FLOG2C;
    FilmRgb pixels[16],original[16];
    for (unsigned i=0;i<16;++i) pixels[i]=(FilmRgb){.05f+i*.01f,.1f,.2f};
    memcpy(original,pixels,sizeof pixels);
    FilmRawScene scene={4,4,pixels};
    recipe.input_tone.value[FILM_TONE_EV]=1;
    recipe.white_balance=(FilmWhiteBalance){6500,2,-1};
    FilmDevelopCanvas with={0},without={0}; FilmDevelopOptions options={0};
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,&lut,&options,&with)==FILM_OK);
    recipe.profile.strength=0;
    assert(film_raw_scene_grade("film-raw-scene-grade/v1",&scene,&recipe,NULL,&options,&without)==FILM_OK);
    assert(with.width==without.width && with.height==without.height);
    for (unsigned i=0;i<with.width*with.height*3;++i)
        assert(abs((int)with.pixels[i]-(int)without.pixels[i])<=1);
    assert(!memcmp(original,pixels,sizeof pixels));
    film_develop_canvas_free(&with); film_develop_canvas_free(&without); film_lut_free(&lut);
    puts("PASS parsed Log input/output, production LUT mixing, RAW exposure/WB and immutable scene");
}

int main(void)
{
    reference_points(); range_and_validation(); production_pipeline();
    return 0;
}
