#include "platform_camera.h"
#include "camera_stop.h"
#include <3ds.h>
typedef struct {unsigned port;FILE *log;Result error;} StopContext;
static FilmStatus result(StopContext *c,const char *name,Result rc)
{if(R_FAILED(rc)&&R_SUCCEEDED(c->error))c->error=rc;if(c->log){fprintf(c->log,"CAM stop step=%s result=%08lx\n",name,(unsigned long)rc);fflush(c->log);}return R_FAILED(rc)?FILM_DEVICE:FILM_OK;}
static FilmStatus stop(void *ctx){StopContext *c=ctx;if(c->log){fputs("CAM stop begin\n",c->log);fflush(c->log);}return result(c,"stop",CAMU_StopCapture(c->port));}
static FilmStatus clear(void *ctx){StopContext *c=ctx;if(c->log){fputs("CAM clear begin\n",c->log);fflush(c->log);}return result(c,"clear",CAMU_ClearBuffer(c->port));}
static FilmStatus busy(void *ctx,bool *value){StopContext *c=ctx;return result(c,"busy",film_camera_read_bool(3,c->port,value,c->log));}
static void delay(void *ctx,unsigned ms){(void)ctx;svcSleepThread((s64)ms*1000000);}
uint32_t film_camera_stop_port(unsigned port,FILE *log,bool *dma_safe)
{
    if(!dma_safe)return MAKERESULT(RL_PERMANENT,RS_INVALIDARG,RM_CAM,RD_INVALID_RESULT_VALUE);
    StopContext c={.port=port,.log=log};FilmStatus s=film_camera_stop_transaction("film-camera-stop/v1",(FilmCameraStopIo){&c,stop,clear,busy,delay},dma_safe);
    if(log){fprintf(log,"CAM stop status=%s dma_safe=%u\n",film_status_name(s),*dma_safe);fflush(log);}
    return s==FILM_OK?0:R_FAILED(c.error)?c.error:MAKERESULT(RL_PERMANENT,RS_INVALIDSTATE,RM_CAM,RD_INVALID_RESULT_VALUE);
}
