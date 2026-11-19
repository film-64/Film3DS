#include "gray_capture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void gray_capture(void)
{
    char directory[]="/tmp/film-gray-capture-XXXXXX";
    assert(mkdtemp(directory));
    uint8_t *pixels=malloc(640u*480u*2u);assert(pixels);
    for(unsigned i=0;i<640u*480u*2u;++i)pixels[i]=(uint8_t)(i*13u);
    FilmGrayPair sample={
        .preview={640,480,FILM_YUYV,pixels},.meter_region={160,120,320,240},
        .yuv=FILM_601_FULL,.preview_time_ms=UINT64_C(1700000000123),
        .id=27,.raw_source_crc=UINT32_C(0x1f2f3f4f),
        .native_level=-5,.exposure_mode=0,.native_ae=true,.native_awb=true
    };
    assert(film_gray_pair_write("film-gray-pair-write/unknown",directory,&sample)==FILM_STATE);
    assert(film_gray_pair_write("film-gray-pair-write/v1",directory,&sample)==FILM_OK);
    assert(film_gray_pair_verify("film-gray-pair-verify/v1",directory,27,sample.raw_source_crc)==FILM_OK);
    assert(film_gray_pair_write("film-gray-pair-write/v1",directory,&sample)==FILM_STATE);
    assert(film_gray_pair_verify("film-gray-pair-verify/v1",directory,27,0)==FILM_CORRUPT);
    assert(film_gray_pair_verify("film-gray-pair-verify/v1",directory,28,sample.raw_source_crc)==FILM_IO);
    char filename[256];snprintf(filename,sizeof filename,"%s/ROLL0027.F3Y",directory);
    FILE *f=fopen(filename,"r+b");assert(f);
    assert(fseek(f,80+119,SEEK_SET)==0);
    assert(fputc(0xff,f)!=EOF);assert(fclose(f)==0);
    assert(film_gray_pair_verify("film-gray-pair-verify/v1",directory,27,sample.raw_source_crc)==FILM_CORRUPT);
    assert(unlink(filename)==0);
    sample.native_level=5;
    assert(film_gray_pair_write("film-gray-pair-write/v1",directory,&sample)==FILM_OK);
    assert(film_gray_pair_verify("film-gray-pair-verify/v1",directory,27,sample.raw_source_crc)==FILM_OK);
    assert(unlink(filename)==0);
    sample.native_level=6;
    assert(film_gray_pair_write("film-gray-pair-write/v1",directory,&sample)==FILM_INVALID);
    sample.native_level=-6;
    assert(film_gray_pair_write("film-gray-pair-write/v1",directory,&sample)==FILM_INVALID);
    sample.native_level=5;
    sample.exposure_mode=2;
    assert(film_gray_pair_write("film-gray-pair-write/v1",directory,&sample)==FILM_INVALID);
    sample.exposure_mode=0;sample.preview_time_ms=0;
    assert(film_gray_pair_write("film-gray-pair-write/v1",directory,&sample)==FILM_INVALID);
    sample.preview_time_ms=3;
    sample.meter_region.width=317;
    assert(film_gray_pair_write("film-gray-pair-write/v1",directory,&sample)==FILM_INVALID);
    sample.meter_region.width=320;
    sample.preview.width=320;
    assert(film_gray_pair_write("film-gray-pair-write/v1",directory,&sample)==FILM_INVALID);
    free(pixels);assert(rmdir(directory)==0);
    puts("gray capture: paired YUV, source binding, no overwrite and corruption checks OK");
}
int main(void){gray_capture();return 0;}
