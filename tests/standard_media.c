#include "film.h"
#include "lut_catalog.h"
#include "debug_mode.h"
#include "camera_stop.h"
#include "camera_receive.h"
#include "platform_camera.h"
#include <assert.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static void write_text(const char *name,const char *text){FILE *f=fopen(name,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));}
static void dng(void)
{
    FilmFrame raw={.recipe=film_default_recipe(),.camera=1,.captured_at=1700000000};assert(film_image_alloc(&raw.source,640,480,FILM_RAW10_RGGB)==FILM_OK);
    for(unsigned i=0;i<640*480;++i){unsigned value=(i*13+i/640)%1024;raw.source.pixels[2*i]=value>>2;raw.source.pixels[2*i+1]=value&3;}
    const char *name="build/standard-sensor.DNG";unlink(name);
    assert(film_dng_write("scheduler_decision:admitted",name,&raw)==FILM_STATE&&access(name,F_OK));
    assert(film_dng_write("film-dng-write/v1",name,&raw)==FILM_OK);
    FilmFrame copy={0};assert(film_frame_read(name,&copy)==FILM_OK);
    assert(copy.source.width==640&&copy.source.height==480&&copy.source.format==FILM_RAW10_RGGB);
    assert(!memcmp(copy.source.pixels,raw.source.pixels,614400));film_frame_free(&copy);
    FILE *f=fopen(name,"rb+");assert(f&&fseek(f,2048+20,SEEK_SET)==0);int byte=fgetc(f);assert(byte>=0&&fseek(f,-1,SEEK_CUR)==0);fputc(byte^1,f);fclose(f);
    assert(film_frame_read(name,&copy)==FILM_CORRUPT&&!copy.source.pixels);
    assert(film_dng_write("film-dng-write/v1",name,&raw)==FILM_OK);
    raw.source.pixels[1]=4;assert(film_dng_write("film-dng-write/v1","build/invalid-sensor.DNG",&raw)==FILM_CORRUPT&&access("build/invalid-sensor.DNG",F_OK));
    film_frame_free(&raw);
    puts("PASS standard DNG lossless samples, digest corruption rejection, no fake proof, no partial file");
}
static void cubes(void)
{
    char root[]="build/cubes.XXXXXX";int temporary=mkstemp(root);assert(temporary>=0);close(temporary);assert(!unlink(root)&&!mkdir(root,0700));char packed[512],sd[512],name[768];
    snprintf(packed,sizeof packed,"%s/packed",root);snprintf(sd,sizeof sd,"%s/sd",root);assert(!mkdir(packed,0700)&&!mkdir(sd,0700));
    for(unsigned i=0;i<70;++i){snprintf(name,sizeof name,"%s/%03u.CUBE",sd,i);write_text(name,"LUT_3D_SIZE 2\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n");}
    char long_name[130];memset(long_name,'x',sizeof long_name);memcpy(long_name+124,".cube",6);snprintf(name,sizeof name,"%s/%s",sd,long_name);write_text(name,"long-name visible");
    FilmLookCatalog c={0};assert(film_lut_catalog_scan("unknown",sd,&c)==FILM_STATE);
    assert(film_lut_catalog_scan("film-lut-catalog/v1",sd,&c)==FILM_OK&&c.count==71);
    assert(!strcmp(c.looks[0].name,"000.CUBE"));assert(strlen(c.looks[70].key)<64&&!strcmp(c.looks[70].name,long_name));film_lut_catalog_free(&c);
    snprintf(name,sizeof name,"%s/000.CUBE",packed);write_text(name,"do not overwrite user file");
    snprintf(name,sizeof name,"%s/new.cube",packed);write_text(name,"plain cube only");unsigned installed=0;
    assert(film_lut_install_missing("unknown",packed,sd,&installed)==FILM_STATE);
    assert(film_lut_install_missing("film-lut-install/v1",packed,sd,&installed)==FILM_OK&&installed==1);
    assert(film_lut_install_missing("film-lut-install/v1",packed,sd,&installed)==FILM_OK&&!installed);
    snprintf(name,sizeof name,"%s/000.CUBE",sd);FILE *f=fopen(name,"rb");assert(f&&fgetc(f)=='L');fclose(f);
    for(unsigned k=0;k<2;++k){const char *folder=k?sd:packed;DIR *dir=opendir(folder);assert(dir);struct dirent *e;while((e=readdir(dir)))if(e->d_name[0]!='.'){snprintf(name,sizeof name,"%s/%s",folder,e->d_name);assert(!unlink(name));}closedir(dir);assert(!rmdir(folder));}assert(!rmdir(root));
    puts("PASS standalone CUBE catalog, case-insensitive extension, >64 files, long names, missing-only installation, no private profiles");
}
static void generic_cube(void)
{
    FILE *f=tmpfile();assert(f);fputs("TITLE \"Plain 32\"\nLUT_3D_SIZE 32\n",f);
    for(unsigned b=0;b<32;++b)for(unsigned g=0;g<32;++g)for(unsigned r=0;r<32;++r)fprintf(f,"%.8f %.8f %.8f\n",r/31.f,g/31.f,b/31.f);
    rewind(f);FilmLut lut={0};assert(film_lut_load(f,&lut,NULL)==FILM_OK&&lut.size==32);fclose(f);
    FilmFrame frame={.recipe=film_default_recipe()};strcpy(frame.recipe.lut_id,"Open.cube");
    assert(film_image_alloc(&frame.source,8,6,FILM_RGB24)==FILM_OK);memset(frame.source.pixels,64,8*6*3);
    FilmImage result={0};assert(film_develop_with_options(&frame,&lut,NULL,&result)==FILM_OK);
    assert(result.pixels[0]>=63&&result.pixels[0]<=65);film_image_free(&result);film_frame_free(&frame);film_lut_free(&lut);
    puts("PASS standard 32-point CUBE without f3p through actual develop pipeline");
}
typedef struct {unsigned stops,clears,polls,delays;bool stuck,stop_error,clear_error,read_error;} Stop;
static FilmStatus stop_io(void *v){Stop *s=v;++s->stops;return s->stop_error?FILM_DEVICE:FILM_OK;}
static FilmStatus clear_io(void *v){Stop *s=v;++s->clears;return s->clear_error?FILM_DEVICE:FILM_OK;}
static FilmStatus busy_io(void *v,bool *busy){Stop *s=v;++s->polls;*busy=s->stuck||s->polls<3;return s->read_error?FILM_CORRUPT:FILM_OK;}
static void delay_io(void *v,unsigned ms){assert(ms==10);++((Stop *)v)->delays;}
static void safe_stop(void)
{
    Stop s={0};bool safe=true;FilmCameraStopIo io={&s,stop_io,clear_io,busy_io,delay_io};
    assert(film_camera_stop_transaction("scheduler_decision:admitted",io,&safe)==FILM_STATE&&!safe&&!s.stops);
    assert(film_camera_stop_transaction("film-camera-stop/v1",io,&safe)==FILM_OK&&safe&&s.stops==1&&s.clears==1&&s.polls==3);
    s=(Stop){.stuck=true};assert(film_camera_stop_transaction("film-camera-stop/v1",io,&safe)==FILM_DEVICE&&!safe&&s.polls==20&&s.delays==19);
    s=(Stop){.stop_error=true};assert(film_camera_stop_transaction("film-camera-stop/v1",io,&safe)==FILM_DEVICE&&!safe);
    s=(Stop){.clear_error=true};assert(film_camera_stop_transaction("film-camera-stop/v1",io,&safe)==FILM_DEVICE&&!safe);
    s=(Stop){.read_error=true};assert(film_camera_stop_transaction("film-camera-stop/v1",io,&safe)==FILM_CORRUPT&&!safe&&s.polls==1);
    puts("PASS bounded camera idle proof, stop/clear/read faults, no DMA release without proof and no recapture");
}
static void raw_only_shutter(void)
{
    FilmCamera camera={.initialized=true,.sensor=FILM_CAMERA_REAR1,.exposure_mode=FILM_EXPOSURE_NATIVE_AE};
    assert(film_camera_raw_shutter_check("film-camera-shutter-raw/v1",&camera)==FILM_OK);
    assert(film_camera_raw_shutter_check("scheduler_decision:admitted",&camera)==FILM_STATE);
    assert(film_camera_raw_shutter_check("film-camera-shutter-pair/v1",&camera)==FILM_STATE);
    camera.initialized=false;assert(film_camera_raw_shutter_check("film-camera-shutter-raw/v1",&camera)==FILM_STATE);
    camera.initialized=true;camera.sensor=FILM_CAMERA_FRONT;
    assert(film_camera_raw_shutter_check("film-camera-shutter-raw/v1",&camera)==FILM_UNSUPPORTED);
    camera.sensor=FILM_CAMERA_REAR2;assert(film_camera_raw_shutter_check("film-camera-shutter-raw/v1",&camera)==FILM_UNSUPPORTED);
    camera.sensor=FILM_CAMERA_REAR1;camera.exposure_mode=FILM_EXPOSURE_MANUAL;
    assert(film_camera_raw_shutter_check("film-camera-shutter-raw/v1",&camera)==FILM_STATE);
    camera.exposure_requested=true;camera.exposure.active=camera.exposure.saved_valid=camera.exposure.ready=true;
    assert(film_camera_raw_shutter_check("film-camera-shutter-raw/v1",&camera)==FILM_OK);
    camera.exposure.failed=true;assert(film_camera_raw_shutter_check("film-camera-shutter-raw/v1",&camera)==FILM_STATE);
    puts("PASS RAW-only shutter: closed camera, unsupported sensors, unready/failed Manual and retired/fake sources reject without native fallback");
}
typedef struct {unsigned waits,polls,delays;uint64_t clock;bool never,wait_error,read_error,immediate;} Receive;
static FilmStatus event_wait(void *v,unsigned ms){Receive *r=v;++r->waits;assert(ms==1500);return r->wait_error?FILM_DEVICE:FILM_OK;}
static FilmStatus event_finished(void *v,bool *done){Receive *r=v;++r->polls;*done=!r->never&&(r->immediate||r->polls>=3);return r->read_error?FILM_CORRUPT:FILM_OK;}
static uint64_t event_now(void *v){return ((Receive *)v)->clock;}
static void event_delay(void *v,unsigned ms){Receive *r=v;assert(ms==1);++r->delays;r->clock+=ms;}
static void receive_completion(void)
{
    Receive r={0};FilmCameraReceiveIo io={&r,event_wait,event_finished,event_now,event_delay};
    assert(film_camera_receive_complete("unknown",io)==FILM_STATE&&!r.waits);
    assert(film_camera_receive_complete("film-camera-receive-complete/v1",io)==FILM_OK&&r.waits==1&&r.polls==3&&r.delays==2);
    /* Manual startup regression: first seed is complete immediately; the
     * next event precedes its finished flag. Both must finish without rearm. */
    r=(Receive){.immediate=true};
    assert(film_camera_receive_complete("film-camera-receive-complete/v1",io)==FILM_OK&&r.waits==1&&r.polls==1&&!r.delays);
    r=(Receive){0};
    bool legacy_finished=true;
    assert(event_wait(&r,1500)==FILM_OK&&event_finished(&r,&legacy_finished)==FILM_OK&&!legacy_finished);
    /* The deployed one-read branch rejects this exact transition. */
    r=(Receive){0};
    assert(film_camera_receive_complete("film-camera-receive-complete/v1",io)==FILM_OK&&r.waits==1&&r.polls==3&&r.delays==2);
    r=(Receive){.never=true};assert(film_camera_receive_complete("film-camera-receive-complete/v1",io)==FILM_DEVICE&&r.waits==1&&r.polls==1501&&r.delays==1500);
    r=(Receive){.wait_error=true};assert(film_camera_receive_complete("film-camera-receive-complete/v1",io)==FILM_DEVICE&&r.waits==1&&!r.polls);
    r=(Receive){.read_error=true};assert(film_camera_receive_complete("film-camera-receive-complete/v1",io)==FILM_CORRUPT&&r.waits==1&&r.polls==1&&!r.delays);
    puts("PASS receive event plus actual finished proof, transient false waits same frame, bounded timeout, no rearm/recapture and read fault rejection");
}
static void gesture(void)
{
    FilmDebugMode state={0};unsigned keys[]={1,1,2,2,4,4,8,8};
    for(unsigned round=0;round<2;++round){for(unsigned i=0;i<8;++i)assert(film_debug_mode_key(&state,keys[i])==(i==7));assert(state.debug_mode==(round==0));}
    for(unsigned i=0;i<7;++i)assert(!film_debug_mode_key(&state,keys[i]));assert(!film_debug_mode_key(&state,FILM_DEBUG_OTHER));assert(!state.debug_mode);
    assert(!film_debug_mode_key(&state,0));assert(!film_debug_mode_key(NULL,1));
    puts("PASS AABB up-up down-down debug ON/OFF and malformed gesture rejection");
}
int main(void){dng();cubes();generic_cube();gesture();safe_stop();raw_only_shutter();receive_completion();return 0;}
