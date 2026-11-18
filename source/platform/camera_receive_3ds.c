#include "platform_camera.h"
#include "camera_receive.h"
#include <3ds.h>
typedef struct {FilmCamera *camera;Handle event;Result result;FILE *log;} ReceiveContext;
static FilmStatus receive_wait(void *context,unsigned ms)
{
    ReceiveContext *r=context;
    if(r->log){fputs("CAM receive event_wait begin\n",r->log);fflush(r->log);}
    r->result=svcWaitSynchronization(r->event,(s64)ms*1000000);
    return R_FAILED(r->result)?FILM_DEVICE:FILM_OK;
}
static FilmStatus receive_finished(void *context,bool *finished)
{
    ReceiveContext *r=context;r->result=film_camera_read_bool(8,r->camera->port,finished,
        r->log);
    return R_FAILED(r->result)?FILM_DEVICE:FILM_OK;
}
static uint64_t receive_now(void *context){(void)context;return osGetTime();}
static void receive_delay(void *context,unsigned ms){(void)context;svcSleepThread((s64)ms*1000000);}

uint32_t film_camera_receive_wait(FilmCamera *camera,uint32_t event,FILE *trace,bool *received)
{
    if(!received||!camera)return MAKERESULT(RL_PERMANENT,RS_INVALIDARG,RM_CAM,RD_INVALID_RESULT_VALUE);
    *received=false;
    ReceiveContext context={.camera=camera,.event=event,.log=trace};
    FilmStatus completion=film_camera_receive_complete("film-camera-receive-complete/v1",
        (FilmCameraReceiveIo){&context,receive_wait,receive_finished,receive_now,receive_delay});
    *received=completion==FILM_OK;
    Result rc=*received?0:R_FAILED(context.result)?context.result:MAKERESULT(RL_PERMANENT,RS_INVALIDSTATE,RM_CAM,RD_TIMEOUT);
    FILE *log=trace?trace:camera->runtime_log;
    if(log){fprintf(log,"CAM receive completion=%s finished=%u result=%08lx\n",film_status_name(completion),*received,(unsigned long)rc);
        if(trace||completion!=FILM_OK)fflush(log);}
    return rc;
}
