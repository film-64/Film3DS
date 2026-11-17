#include "platform_camera.h"
#include "raw_capture.h"
#include <3ds.h>
#include <string.h>
#include <time.h>

typedef struct {FilmCamera *sensor_owner;FilmRegisterTransport reg;uint8_t *dma;FILE *log;bool unsafe,saved_ae,saved_awb;FilmFrame *out,*native_capture;} RawCamera;
static int32_t reg_open(void *ctx,const char *name,uint32_t *handle){(void)ctx;return srvGetServiceHandle(handle,name);}
static int32_t reg_exchange(void *ctx,uint32_t handle,const uint32_t request[4],uint32_t reply[3])
{(void)ctx;u32 *b=getThreadCommandBuffer();memset(b,0,64*sizeof *b);memcpy(b,request,4*sizeof *b);Result rc=svcSendSyncRequest(handle);memcpy(reply,b,3*sizeof *b);return rc;}
static void reg_close(void *ctx,uint32_t handle){(void)ctx;svcCloseHandle(handle);}
static FilmStatus access(RawCamera *c,bool writing,bool mcu,uint16_t a,uint16_t *v)
{FilmStatus s=film_register_access(&c->reg,writing,mcu,a,v);fprintf(c->log,"RAW %s %s %04x %04x %s\n",writing?"write":"read",mcu?"mcu":"reg",a,*v,film_status_name(s));return s;}
static FilmStatus read_reg(void *ctx,bool mcu,uint16_t a,uint16_t *v){*v=0;return access(ctx,false,mcu,a,v);}
static FilmStatus write_reg(void *ctx,bool mcu,uint16_t a,uint16_t v){return access(ctx,true,mcu,a,&v);}
extern Handle camHandle;
static Result getter(RawCamera *c,unsigned command,unsigned argument,bool *value)
{
    u32 *b=getThreadCommandBuffer();memset(b,0,64*sizeof *b);
    b[0]=IPC_MakeHeader(command,1,0);b[1]=argument;b[2]=0xa5a50000;
    Result transport=svcSendSyncRequest(camHandle);
    uint32_t reply[3];memcpy(reply,b,sizeof reply);
    FilmStatus status=R_FAILED(transport)?FILM_DEVICE:film_camera_bool_reply(command,reply,value);
    fprintf(c->log,"RAW getter cmd=%02x header=%08lx word=%08lx transport=%08lx service=%08lx status=%s value=%u\n",
        command,(unsigned long)reply[0],(unsigned long)reply[2],(unsigned long)transport,(unsigned long)reply[1],film_status_name(status),status==FILM_OK?*value:0);
    if(R_FAILED(transport))return transport;
    if(R_FAILED((Result)reply[1]))return (Result)reply[1];
    return status==FILM_OK?0:MAKERESULT(RL_PERMANENT,RS_INVALIDRESVAL,RM_CAM,RD_INVALID_RESULT_VALUE);
}
static FilmStatus idle(void *ctx)
{
    RawCamera *c=ctx;Result a=CAMU_StopCapture(PORT_CAM1),b=CAMU_ClearBuffer(PORT_CAM1);bool stopped=false;
    for(unsigned i=0;i<20;++i){bool busy=true;Result rc=getter(c,3,PORT_CAM1,&busy);if(R_FAILED(rc))break;if(!busy){stopped=true;break;}svcSleepThread(10000000LL);}
    c->unsafe=R_FAILED(a)||R_FAILED(b)||!stopped;
    fprintf(c->log,"RAW idle stop=%08lx clear=%08lx stopped=%u\n",(unsigned long)a,(unsigned long)b,stopped);
    return c->unsafe?FILM_DEVICE:FILM_OK;
}
static FilmStatus auto_state(RawCamera *c,bool ae_target,bool awb_target)
{
    bool ae=!ae_target,awb=!awb_target;
    Result rc=getter(c,0x1a,SELECT_OUT1,&ae);
    if(R_SUCCEEDED(rc))rc=getter(c,0x1c,SELECT_OUT1,&awb);
    if(R_SUCCEEDED(rc)&&ae!=ae_target)rc=CAMU_SetAutoExposure(SELECT_OUT1,ae_target);
    if(R_SUCCEEDED(rc)&&awb!=awb_target)rc=CAMU_SetAutoWhiteBalance(SELECT_OUT1,awb_target);
    if(R_SUCCEEDED(rc))rc=getter(c,0x1a,SELECT_OUT1,&ae);
    if(R_SUCCEEDED(rc))rc=getter(c,0x1c,SELECT_OUT1,&awb);
    fprintf(c->log,"RAW auto target_ae=%u target_awb=%u ae=%u awb=%u result=%08lx\n",ae_target,awb_target,ae,awb,(unsigned long)rc);
    return R_SUCCEEDED(rc)&&ae==ae_target&&awb==awb_target?FILM_OK:FILM_DEVICE;
}
static FilmStatus lock(void *ctx){return auto_state(ctx,false,false);}
static FilmStatus restore_auto(void *ctx){RawCamera *c=ctx;return auto_state(c,c->saved_ae,c->saved_awb);}
static void trace(void *ctx,const char *phase,uint16_t a,uint16_t expected,uint16_t actual,FilmStatus s)
{RawCamera *c=ctx;fprintf(c->log,"RAW phase=%s address=%04x expected=%04x observed=%04x status=%s\n",phase,a,expected,actual,film_status_name(s));fflush(c->log);}
static FilmStatus frame(void *ctx,bool retain,bool raw)
{
    RawCamera *c=ctx;FilmStatus s=idle(c);if(s!=FILM_OK)return s;
    memset(c->dma,0x5a,614400+256);Handle complete=0;
    Result rc=GSPGPU_FlushDataCache(c->dma,614400+256);
    const char *stage="flush";
    if(R_SUCCEEDED(rc)){stage="set_receiving";rc=CAMU_SetReceiving(&complete,c->dma,PORT_CAM1,614400,5120);}
    if(R_SUCCEEDED(rc)){stage="start";rc=CAMU_StartCapture(PORT_CAM1);}
    if(R_SUCCEEDED(rc)){stage="wait";rc=svcWaitSynchronization(complete,1500000000LL);}
    bool finished=false;if(R_SUCCEEDED(rc)){stage="finished";rc=getter(c,8,PORT_CAM1,&finished);}
    fprintf(c->log,"RAW receive stage=%s raw=%u retain=%u result=%08lx finished=%u\n",stage,raw,retain,(unsigned long)rc,finished);
    s=idle(c);if(complete)svcCloseHandle(complete);
    if(R_FAILED(rc)||!finished||s!=FILM_OK)return FILM_DEVICE;
    if(R_FAILED(GSPGPU_InvalidateDataCache(c->dma,614400+256)))return FILM_DEVICE;
    for(unsigned i=614400;i<614400+256;++i)if(c->dma[i]!=0x5a)return FILM_CORRUPT;
    if(raw)for(unsigned i=1;i<614400;i+=2)if(c->dma[i]>3)return FILM_CORRUPT;
    fprintf(c->log,"RAW frame raw=%u retained=%u complete=1 idle=1 guard=1\n",raw,retain);
    if(retain){FilmFrame *out=raw?c->out:c->native_capture;
        if(!out)return FILM_INVALID;
        s=film_image_alloc(&out->source,640,480,raw?FILM_RAW10_RGGB:FILM_YUYV);
        if(!raw&&s==FILM_NOMEM){fprintf(c->log,"RAW native_JPEG_retention=no_memory continue_RAW=1\n");return FILM_OK;}
        if(s==FILM_OK)memcpy(out->source.pixels,c->dma,614400);}
    return s;
}
static FilmStatus native_frame(void *ctx){return frame(ctx,true,false);}
static FilmStatus capture_pair(FilmCamera *camera,FilmFrame *out,FilmFrame *native,FILE *local_trace)
{
    if(!film_runtime_source_valid("film-camera-raw-capture/v1"))return FILM_STATE;
    if(!camera||!out||out->source.pixels||!local_trace||(native&&(native==out||native->source.pixels)))return FILM_INVALID;
    if(!camera->initialized||camera->sensor!=FILM_CAMERA_REAR1)return FILM_UNSUPPORTED;
    if(camera->exposure_mode==FILM_EXPOSURE_MANUAL) {
        FilmStatus verified=film_camera_verify_manual_capture(camera);if(verified!=FILM_OK)return verified;
    }
    FilmStatus s=film_camera_preview_stop(camera);
    if(s==FILM_OK)s=film_camera_set_zoom(camera,1);
    if(s!=FILM_OK)return s;
    fprintf(local_trace,"RAW entry transfer=%u preview_stop_zoom=%s\n",camera->transfer,film_status_name(s));
    if(camera->transfer!=5120)return FILM_STATE;
    RawCamera c={.sensor_owner=camera,.log=local_trace,.out=out,.native_capture=native};
    Result rc=getter(&c,0x1a,SELECT_OUT1,&c.saved_ae);
    if(R_SUCCEEDED(rc))rc=getter(&c,0x1c,SELECT_OUT1,&c.saved_awb);
    fprintf(local_trace,"RAW saved_auto result=%08lx ae=%u awb=%u mode=%u\n",(unsigned long)rc,c.saved_ae,c.saved_awb,camera->exposure_mode);
    if(R_FAILED(rc)||c.saved_ae!=(camera->exposure_mode==FILM_EXPOSURE_NATIVE_AE))return FILM_STATE;
    c.dma=linearMemAlign(614400+256,0x80);if(!c.dma)return FILM_NOMEM;
    fprintf(local_trace,"source=film-camera-raw-capture/v1 mode=%s exposure=live_shutter_snapshot no_fixed_coarse_or_gain=1\n",
        camera->exposure_mode==FILM_EXPOSURE_NATIVE_AE?"native_AE":"manual");
    if(s==FILM_OK)s=film_register_open(&c.reg,(FilmRegisterIo){&c,reg_open,reg_exchange,reg_close});
    if(s==FILM_OK)s=film_raw_capture_transaction("film-camera-raw-capture/v1",
        (FilmRawCaptureIo){&c,read_reg,write_reg,lock,frame,idle,restore_auto,trace,native?native_frame:NULL});
    FilmStatus stopped=idle(&c);if(stopped!=FILM_OK)s=stopped;
    film_register_close(&c.reg);
    if(s!=FILM_OK)film_camera_close(camera);
    if(!c.unsafe)linearFree(c.dma); /* Unsafe DMA is deliberately retained until process exit. */
    fprintf(local_trace,"RAW transaction=%s dma_quarantined=%u camera_live=%u\n",film_status_name(s),c.unsafe,camera->initialized);fflush(local_trace);
    if(s!=FILM_OK){film_image_free(&out->source);if(native)film_frame_free(native);}
    else{out->camera=1;out->captured_at=(uint64_t)time(NULL);out->iso=out->exposure_us=0;
        if(native){native->camera=out->camera;native->captured_at=out->captured_at;native->iso=native->exposure_us=0;}}
    return s;
}
FilmStatus film_camera_capture_raw(FilmCamera *camera,FilmFrame *out,FILE *local_trace)
{return capture_pair(camera,out,NULL,local_trace);}
FilmStatus film_camera_capture_pair(FilmCamera *camera,FilmFrame *raw,FilmFrame *native,FILE *local_trace)
{
    if(!film_runtime_source_valid("film-camera-shutter-pair/v1"))return FILM_STATE;
    if(!native)return FILM_INVALID;
    if(!camera||camera->format!=FILM_YUYV)return FILM_UNSUPPORTED;
    return capture_pair(camera,raw,native,local_trace);
}
