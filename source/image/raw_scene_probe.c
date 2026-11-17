#include "raw_darkroom.h"
#include "film_runtime_contract.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static FilmStatus write_data(const char *dir,const char *name,const void *data,size_t bytes)
{
    char path[512];if(snprintf(path,sizeof path,"%s/%s",dir,name)>=(int)sizeof path)return FILM_INVALID;
    FILE *f=fopen(path,"wbx");if(!f)return FILM_IO;
    bool ok=fwrite(data,1,bytes,f)==bytes;if(fclose(f))ok=false;return ok?FILM_OK:FILM_IO;
}
FilmStatus film_raw_scene_probe(const char *source,const char *input,const char *profile,const char *dir,
    const FilmRecipe *recipe,const FilmDevelopOptions *options)
{
    if(!source||strcmp(source,"film-raw-scene-probe/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!input||!dir||!recipe)return FILM_INVALID;
    FilmFrame frame={0};FilmRawScene scene={0};FilmDevelopCanvas canvas={0};FilmImage preview={0};
    FilmRawIsp isp=film_raw_isp_default();FilmDevelopOptions o=options?*options:(FilmDevelopOptions){0};
    FilmStatus s=film_frame_read(input,&frame);if(s!=FILM_OK)return s;
    if(profile){FILE *f=fopen(profile,"rb");if(!f)s=FILM_IO;else{s=film_raw_profile_load("film-raw-profile-load/v1",f,&isp);fclose(f);}o.raw_isp=&isp;}
    frame.recipe=*recipe;
    if(s==FILM_OK)s=film_raw_frame_scene(&frame,&o,&scene);
    if(s==FILM_OK)s=film_raw_scene_grade("film-raw-scene-grade/v1",&scene,recipe,NULL,&o,&canvas);
    if(s==FILM_OK)s=film_develop_canvas_render(&canvas,recipe,&o,false,&preview);
    if(s==FILM_OK&&mkdir(dir,0700))s=FILM_IO;
    if(s==FILM_OK)s=write_data(dir,"canvas.rgb16",canvas.pixels,(size_t)canvas.width*canvas.height*3*sizeof(uint16_t));
    /* Bounded scene samples accompany display pixels; full scene stays owned and
     * unmodified. Four output files fit the existing 4 MiB evidence snapshot. */
    FilmRgb samples[1200];unsigned count=0;
    float low=INFINITY,high=-INFINITY;unsigned negative=0,superwhite=0;
    if(s==FILM_OK){
        for(size_t i=0;i<(size_t)scene.width*scene.height;++i){
            FilmRgb a=scene.pixels[i];float v[3]={a.r,a.g,a.b};
            for(unsigned c=0;c<3;++c){low=fminf(low,v[c]);high=fmaxf(high,v[c]);negative+=v[c]<0;superwhite+=v[c]>1;}
        }
        for(unsigned y=0;y<30;++y)for(unsigned x=0;x<40;++x){unsigned xx=(2*x+1)*scene.width/80,yy=(2*y+1)*scene.height/60;samples[count++]=scene.pixels[(size_t)yy*scene.width+xx];}
        s=write_data(dir,"samples.f32",samples,sizeof samples);
    }
    char path[512];
    if(s==FILM_OK){snprintf(path,sizeof path,"%s/preview.jpg",dir);s=film_jpeg_write(path,&preview,recipe->jpeg_quality);}
    if(s==FILM_OK){
        char report[1024];int n=snprintf(report,sizeof report,"{\"source\":\"film-raw-scene-probe/v1\",\"local_trace\":{\"entry\":\"explicit_frozen_RAW_probe\",\"scene_min\":%.9g,\"scene_max\":%.9g,\"scene_negative\":%u,\"scene_superwhite\":%u},\"width\":%u,\"height\":%u,\"exposure_ev\":%.9g,\"shadows_ev\":%.9g,\"midtones_ev\":%.9g,\"highlights_ev\":%.9g,\"shoulder\":%.9g,\"local_strength\":%.9g,\"local_limit_ev\":%.9g}\n",low,high,negative,superwhite,scene.width,scene.height,recipe->input_tone.value[FILM_TONE_EV],o.raw_shadows,o.raw_midtones,o.raw_highlights,o.raw_shoulder,o.raw_local,o.raw_local_limit);
        if(n<0||n>=(int)sizeof report)s=FILM_INVALID;else s=write_data(dir,"report.json",report,(size_t)n);
    }
    film_raw_scene_free(&scene);film_develop_canvas_free(&canvas);film_image_free(&preview);film_frame_free(&frame);return s;
}
