#include "raw_darkroom.h"
#include "layered_grain.h"
#include "film_runtime_contract.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct { unsigned w,h;FilmRawIsp isp;FilmRecipe recipe;FilmLayeredGrain grain;FilmRawBadPixel *bad;float denoise; } Config;
static const char *keys[]={"width","height","cfa","black","white","wb","ccm","exposure_ev","shoulder","denoise",
    "highlights","midtones","shadows","saturation","sharpness","grain_amount","grain_size","grain_coarse",
    "grain_shadow","grain_mid","grain_highlight","seed","lut_strength","bad_pixel","ccm_offset","local_strength","local_limit_ev","shading_r","shading_g","shading_b"};
static bool integer(double x,unsigned *out)
{ if(!isfinite(x)||x<0||x>UINT_MAX||floor(x)!=x)return false;*out=(unsigned)x;return true; }
static bool config(const char *path,Config *c)
{
    FILE *f=fopen(path,"rb");if(!f)return false;
    char line[1024];uint32_t seen=0;bool first=true,ok=true;
    while (fgets(line,sizeof line,f)) {
        if (!strchr(line,'\n') && !feof(f)) {ok=false;break;}
        char *comment=strchr(line,'#');if(comment)*comment=0;
        char *key=strtok(line," \t\r\n");if(!key)continue;
        if(first) {first=false;if(strcmp(key,"F3RAW1")||strtok(NULL," \t\r\n")){ok=false;break;}continue;}
        unsigned k=0;while(k<sizeof keys/sizeof *keys && strcmp(key,keys[k]))++k;
        if(k==sizeof keys/sizeof *keys || (k!=23 && (seen&(1u<<k)))){ok=false;break;}
        seen|=1u<<k;double v[9];unsigned n=0;char *word;
        while((word=strtok(NULL," \t\r\n"))) {
            if(n==9){ok=false;break;}char *end;errno=0;v[n]=strtod(word,&end);
            if(*end || errno || !isfinite(v[n])){ok=false;break;}++n;
        }
        unsigned expected=k>=27?6:k==3?4:(k==5||k==24)?3:k==6?9:k==23?2:1;
        if(!ok||n!=expected){ok=false;break;}
        unsigned u=0;
        if((k<=2 || k==21) && !integer(v[0],&u)){ok=false;break;}
        switch(k) {
        case 0:c->w=u;break;case 1:c->h=u;break;case 2:c->isp.cfa=(FilmCfa)u;break;
        case 3:for(unsigned i=0;i<4;++i)c->isp.black[i]=(float)v[i];break;
        case 4:c->isp.white=(float)v[0];break;case 5:for(unsigned i=0;i<3;++i)c->isp.wb[i]=(float)v[i];break;
        case 6:for(unsigned i=0;i<9;++i)c->isp.ccm[i]=(float)v[i];break;
        case 7:c->isp.exposure_ev=(float)v[0];break;case 8:c->isp.shoulder=(float)v[0];break;
        case 9:c->denoise=(float)v[0];break;
        case 10:c->recipe.output_tone.value[FILM_TONE_HIGH]=(float)v[0];break;
        case 11:c->recipe.output_tone.value[FILM_TONE_MID]=(float)v[0];break;
        case 12:c->recipe.output_tone.value[FILM_TONE_SHADOWS]=(float)v[0];break;
        case 13:c->recipe.tone.value[FILM_TONE_SATURATION]=(float)v[0];break;
        case 14:if(v[0]<-5||v[0]>5||floor(v[0])!=v[0])ok=false;else c->recipe.sharpness=(int)v[0];break;
        case 15:c->grain.amount=(float)v[0];break;case 16:c->grain.size=(float)v[0];break;
        case 17:c->grain.coarse=(float)v[0];break;case 18:c->grain.shadow=(float)v[0];break;
        case 19:c->grain.mid=(float)v[0];break;case 20:c->grain.highlight=(float)v[0];break;
        case 21:c->grain.seed=u;break;case 22:c->recipe.profile.strength=(float)v[0];break;
        case 24:for(unsigned i=0;i<3;++i)c->isp.ccm_offset[i]=(float)v[i];break;
        case 25:c->isp.local_strength=(float)v[0];break;
        case 26:c->isp.local_limit_ev=(float)v[0];break;
        case 27:case 28:case 29:for(unsigned i=0;i<6;++i)c->isp.lens_shading[k-27][i]=(float)v[i];break;
        case 23: {
            unsigned x,y;
            if(!integer(v[0],&x)||!integer(v[1],&y)||c->isp.bad_pixel_count==FILM_RAW_MAX_BAD_PIXELS){ok=false;break;}
            if(!c->bad)c->bad=calloc(FILM_RAW_MAX_BAD_PIXELS,sizeof *c->bad);
            if(!c->bad){ok=false;break;}
            c->bad[c->isp.bad_pixel_count++]=(FilmRawBadPixel){x,y};c->isp.bad_pixels=c->bad;break;
        }}
        if(!ok)break;
    }
    if(ferror(f)||first || !(seen&1)||!(seen&2))ok=false;
    if(fclose(f))ok=false;
    c->grain.reference_width=c->w;
    return ok;
}

static FilmStatus ppm(const char *path,const FilmImage *im)
{
    FILE *f=fopen(path,"wbx");if(!f)return FILM_IO;
    bool ok=fprintf(f,"P6\n%u %u\n255\n",im->width,im->height)>0 &&
        fwrite(im->pixels,3,(size_t)im->width*im->height,f)==(size_t)im->width*im->height;
    if(fclose(f))ok=false;return ok?FILM_OK:FILM_IO;
}
static FilmStatus save(const char *dir,const char *name,const FilmImage *im,unsigned quality)
{
    char path[1024];if(snprintf(path,sizeof path,"%s/%s.ppm",dir,name)>=(int)sizeof path)return FILM_INVALID;
    FilmStatus s=ppm(path,im);if(s!=FILM_OK)return s;
    if(snprintf(path,sizeof path,"%s/%s.jpg",dir,name)>=(int)sizeof path)return FILM_INVALID;
    return film_jpeg_write(path,im,quality);
}

int main(int argc,char **argv)
{
    if(argc<5 || argc>8){fprintf(stderr,"filmraw input.bin parameters.txt NEW_OUTPUT_DIR grain.f3g [look.cube look.f3p] [grain.f3gt]\n");return 1;}
    bool has_lut=argc>=7,has_texture=argc==6||argc==8;
    if(!film_runtime_source_valid("film-raw-darkroom-export/v1"))return 6;
    Config c={.isp=film_raw_isp_default(),.recipe=film_default_recipe(),
        .grain={.amount=.35f,.size=1,.coarse=.5f,.shadow=.8f,.mid=1.1f,.highlight=.7f,.seed=1}};
    c.isp.shoulder=.75f;c.denoise=0;
    FilmStatus s=FILM_INVALID;FilmRawPrepared prepared={0};FilmLut lut={0};FilmGrainTexture texture={0};
    uint8_t *wire=NULL;uint16_t *raw=NULL;FILE *f=NULL;size_t n=0;
    if(!config(argv[2],&c))goto done;
    if(c.w<4||c.h<4||(size_t)c.w>FILM_RAW_MAX_PIXELS/c.h)goto done;
    n=(size_t)c.w*c.h;
    wire=malloc(n*2);raw=malloc(n*sizeof *raw);if(!wire||!raw){s=FILM_NOMEM;goto done;}
    f=fopen(argv[1],"rb");if(!f){s=FILM_IO;goto done;}
    bool complete=fread(wire,2,n,f)==n && fgetc(f)==EOF && !ferror(f);
    if(fclose(f))complete=false;f=NULL;if(!complete){s=FILM_CORRUPT;goto done;}
    FilmGrainProfile grain;f=fopen(argv[4],"rb");if(!f){s=FILM_IO;goto done;}
    s=film_grain_profile_load(f,&grain);fclose(f);f=NULL;if(s!=FILM_OK)goto done;
    if(has_texture){f=fopen(argv[argc-1],"rb");if(!f){s=FILM_IO;goto done;}s=film_grain_texture_load(f,&texture);fclose(f);f=NULL;if(s!=FILM_OK)goto done;}
    if(has_lut) {
        float strength=c.recipe.profile.strength;
        s=film_profile_read(argv[6],&c.recipe.profile);if(s!=FILM_OK)goto done;
        c.recipe.profile.strength=strength;
        f=fopen(argv[5],"rb");if(!f){s=FILM_IO;goto done;}unsigned line;
        s=film_lut_load(f,&lut,&line);fclose(f);f=NULL;if(s!=FILM_OK)goto done;
        strcpy(c.recipe.lut_id,"explicit-asset");
    }
    if(mkdir(argv[3],0700)){s=FILM_IO;goto done;}
    char path[1024];
    if(snprintf(path,sizeof path,"%s/local_trace.json",argv[3])>=(int)sizeof path){s=FILM_INVALID;goto done;}
    f=fopen(path,"wx");if(!f){s=FILM_IO;goto done;}
    bool traced=fputs("{\"source\":\"film-raw-darkroom-export/v1\",\"local_trace\":{\"source\":\"film-raw-darkroom-export/v1\",\"entry\":\"explicit_host_CLI\"}}\n",f)>=0;
    if(fclose(f))traced=false;f=NULL;if(!traced){s=FILM_IO;goto done;}
    s=film_raw10_unpack("film-raw10-unpack/v1",wire,2*n,c.w,c.h,raw,n);if(s!=FILM_OK)goto done;
    FilmRaw10View view={c.w,c.h,raw};
    s=film_raw_isp_prepare("film-raw-isp/v1",&view,&c.isp,&prepared);if(s!=FILM_OK)goto done;
    s=film_darkroom_denoise("film-darkroom-denoise/v1",prepared.pixels,c.w,c.h,c.denoise);if(s!=FILM_OK)goto done;
    /* Local host float cache: extended sRGB transfer, native float32 RGB.
     * Retains the pre-quantization values for offline calibration analysis. */
    if(snprintf(path,sizeof path,"%s/prepared.rgbf32",argv[3])>=(int)sizeof path){s=FILM_INVALID;goto done;}
    f=fopen(path,"wx");if(!f){s=FILM_IO;goto done;}
    bool floated=sizeof(FilmRgb)==3*sizeof(float) && sizeof(float)==4 && fwrite(prepared.pixels,sizeof(FilmRgb),n,f)==n;
    if(fclose(f))floated=false;f=NULL;if(!floated){s=FILM_IO;goto done;}
    for(unsigned variant=0;variant<(has_lut?2u:1u);++variant) {
        FilmRecipe recipe=c.recipe;FilmDevelopCanvas canvas={0};FilmImage image={0};
        if(!variant){recipe.profile.strength=0;recipe.lut_id[0]=0;}
        s=film_raw_grade_canvas("film-raw-grade/v1",&prepared,&recipe,variant?&lut:NULL,&canvas);
        if(s==FILM_OK)s=film_develop_canvas_render(&canvas,&recipe,NULL,true,&image);
        film_develop_canvas_free(&canvas);
        if(s==FILM_OK)s=save(argv[3],variant?"look":"plain",&image,recipe.jpeg_quality);
        if(s==FILM_OK)s=film_grain_layered_texture("film-grain-layered/v1",&image,&c.grain,&grain,has_texture?&texture:NULL);
        if(s==FILM_OK)s=save(argv[3],variant?"look-grain":"plain-grain",&image,recipe.jpeg_quality);
        film_image_free(&image);if(s!=FILM_OK)goto done;
    }
    if(snprintf(path,sizeof path,"%s/completed.json",argv[3])>=(int)sizeof path){s=FILM_INVALID;goto done;}
    f=fopen(path,"wx");if(!f){s=FILM_IO;goto done;}
    bool recorded=fprintf(f,"{\"source\":\"film-raw-darkroom-export/v1\",\"width\":%u,\"height\":%u,\"isp_preparations\":1,\"lut_variants\":%u,\"texture_atlas\":%s,\"calibration_certified\":false,\"gpu_verified\":false}\n",c.w,c.h,has_lut?2u:1u,has_texture?"true":"false")>0;
    if(fclose(f))recorded=false;f=NULL;if(!recorded)s=FILM_IO;
done:
    if(f)fclose(f);free(c.bad);free(wire);free(raw);film_raw_prepared_free(&prepared);film_lut_free(&lut);film_grain_texture_free(&texture);
    fprintf(s==FILM_OK?stdout:stderr,"%s\n",film_status_name(s));return (int)s;
}
