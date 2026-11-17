#include "raw_darkroom.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
int main(int argc,char **argv)
{
    if(argc!=11){fprintf(stderr,"filmscene input.F3S calibration.f3isp|- NEW_DIR EV shadows midtones highlights knee local limit\n");return 1;}
    float v[7];
    for(unsigned i=0;i<7;++i){char *end;errno=0;v[i]=strtof(argv[i+4],&end);if(errno||*end||!isfinite(v[i]))return 1;}
    FilmRecipe r=film_default_recipe();r.grain_amount=0;r.input_tone.value[FILM_TONE_EV]=v[0];
    FilmDevelopOptions o={.raw_shadows=v[1],.raw_midtones=v[2],.raw_highlights=v[3],.raw_shoulder=v[4],.raw_local=v[5],.raw_local_limit=v[6]};
    FilmStatus s=film_raw_scene_probe("film-raw-scene-probe/v1",argv[1],argv[2][0]=='-'&&!argv[2][1]?NULL:argv[2],argv[3],&r,&o);
    fprintf(s==FILM_OK?stdout:stderr,"%s\n",film_status_name(s));return s;
}
