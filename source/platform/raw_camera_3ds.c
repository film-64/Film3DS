#include "platform_camera.h"
#include "raw_capture.h"
#include <3ds.h>
#include <string.h>
#include <time.h>

#ifndef FILM_CAMERA_VERBOSE
#define FILM_CAMERA_VERBOSE 0
#endif

typedef struct {FilmCamera *sensor_owner;FilmRegisterTransport reg;uint8_t *dma;FILE *log;bool unsafe,saved_ae,saved_awb;FilmFrame *out;} RawCamera;
static int32_t reg_open(void *ctx,const char *name,uint32_t *handle){(void)ctx;return srvGetServiceHandle(handle,name);}
static int32_t reg_exchange(void *ctx,uint32_t handle,const uint32_t request[4],uint32_t reply[3])
{(void)ctx;u32 *b=getThreadCommandBuffer();memset(b,0,64*sizeof *b);memcpy(b,request,4*sizeof *b);Result rc=svcSendSyncRequest(handle);memcpy(reply,b,3*sizeof *b);return rc;}
static void reg_close(void *ctx,uint32_t handle){(void)ctx;svcCloseHandle(handle);}
static FilmStatus access(RawCamera *c,bool writing,bool mcu,uint16_t a,uint16_t *v)
{if(atomic_load(&c->sensor_owner->debug_mode)&&c->log){fprintf(c->log,"RAW begin %s %s %04x\n",writing?"write":"read",mcu?"mcu":"reg",a);fflush(c->log);}
 FilmStatus s=film_register_access(&c->reg,writing,mcu,a,v);if(c->log&&(FILM_CAMERA_VERBOSE||atomic_load(&c->sensor_owner->debug_mode)||s!=FILM_OK))fprintf(c->log,"RAW %s %s %04x %04x %s\n",writing?"write":"read",mcu?"mcu":"reg",a,*v,film_status_name(s));return s;}
static FilmStatus read_reg(void *ctx,bool mcu,uint16_t a,uint16_t *v){*v=0;return access(ctx,false,mcu,a,v);}
static FilmStatus write_reg(void *ctx,bool mcu,uint16_t a,uint16_t v){return access(ctx,true,mcu,a,&v);}
static Result getter(RawCamera *c,unsigned command,unsigned argument,bool *value)
{return film_camera_read_bool(command,argument,value,atomic_load(&c->sensor_owner->debug_mode)?c->log:NULL);}
static void step(RawCamera *c,const char *name)
{if(atomic_load(&c->sensor_owner->debug_mode)){fprintf(c->log,"RAW step=%s begin\n",name);fflush(c->log);}}
static FilmStatus idle(void *ctx)
{
    RawCamera *c=ctx;bool safe=false;Result rc=film_camera_stop_port(PORT_CAM1,atomic_load(&c->sensor_owner->debug_mode)?c->log:NULL,&safe);c->unsafe=!safe;
    if(c->log&&(R_FAILED(rc)||!safe))fprintf(c->log,"RAW stop error result=%08lx dma_safe=%u\n",(unsigned long)rc,safe);
    return R_SUCCEEDED(rc)&&safe?FILM_OK:FILM_DEVICE;
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
    if(c->log&&(R_FAILED(rc)||ae!=ae_target||awb!=awb_target||atomic_load(&c->sensor_owner->debug_mode)))fprintf(c->log,"RAW auto target_ae=%u target_awb=%u ae=%u awb=%u result=%08lx\n",ae_target,awb_target,ae,awb,(unsigned long)rc);
    return R_SUCCEEDED(rc)&&ae==ae_target&&awb==awb_target?FILM_OK:FILM_DEVICE;
}
static FilmStatus lock(void *ctx){return auto_state(ctx,false,false);}
static FilmStatus restore_auto(void *ctx){RawCamera *c=ctx;return auto_state(c,c->saved_ae,c->saved_awb);}
static void trace(void *ctx,const char *phase,uint16_t a,uint16_t expected,uint16_t actual,FilmStatus s)
{RawCamera *c=ctx;
 static const char *const names[]={"preconditions","lock_auto","native_gate","raw_switch","raw_settle","raw_retain","restore"};
 for(unsigned i=0;i<7;++i)if(!strcmp(phase,names[i]))atomic_store(&c->sensor_owner->capture_phase,i+1);
 if(c->log&&(s!=FILM_OK||atomic_load(&c->sensor_owner->debug_mode)))fprintf(c->log,"RAW phase=%s address=%04x expected=%04x observed=%04x status=%s\n",phase,a,expected,actual,film_status_name(s));if(c->log&&(s!=FILM_OK||atomic_load(&c->sensor_owner->debug_mode)))fflush(c->log);}
static FilmStatus frame(void *ctx,bool retain,bool raw)
{
    RawCamera *c=ctx;FilmStatus s=idle(c);if(s!=FILM_OK)return s;
    memset(c->dma,0x5a,614400+256);Handle complete=0;
    step(c,"flush");Result rc=GSPGPU_FlushDataCache(c->dma,614400+256);
    const char *stage="flush";uint64_t frame_started=0;
    if(R_SUCCEEDED(rc)){stage="set_receiving";step(c,stage);rc=CAMU_SetReceiving(&complete,c->dma,PORT_CAM1,614400,5120);}
    if(R_SUCCEEDED(rc)){stage="start";step(c,stage);frame_started=(uint64_t)time(NULL);rc=CAMU_StartCapture(PORT_CAM1);}
    bool finished=false;
    if(R_SUCCEEDED(rc)){stage="wait_finished";step(c,stage);rc=film_camera_receive_wait(c->sensor_owner,complete,atomic_load(&c->sensor_owner->debug_mode)?c->log:NULL,&finished);}
    if(c->log&&(R_FAILED(rc)||!finished||atomic_load(&c->sensor_owner->debug_mode)))fprintf(c->log,"RAW receive stage=%s raw=%u retain=%u result=%08lx finished=%u\n",stage,raw,retain,(unsigned long)rc,finished);
    s=idle(c);if(complete)svcCloseHandle(complete);
    if(R_FAILED(rc)||!finished||s!=FILM_OK)return FILM_DEVICE;
    if(R_FAILED(GSPGPU_InvalidateDataCache(c->dma,614400+256)))return FILM_DEVICE;
    for(unsigned i=614400;i<614400+256;++i)if(c->dma[i]!=0x5a)return FILM_CORRUPT;
    if(raw)for(unsigned i=1;i<614400;i+=2)if(c->dma[i]>3)return FILM_CORRUPT;
    if(c->log&&atomic_load(&c->sensor_owner->debug_mode))fprintf(c->log,"RAW frame raw=%u retained=%u complete=1 idle=1 guard=1\n",raw,retain);
    if(retain){FilmFrame *out=c->out;
        if(!raw)return FILM_STATE;
        if(!out)return FILM_INVALID;
        s=film_image_alloc(&out->source,640,480,FILM_RAW10_RGGB);
        if(s==FILM_OK){memcpy(out->source.pixels,c->dma,614400);out->captured_at=frame_started;}}
    return s;
}
FilmStatus film_camera_prepare_raw(FilmCamera *camera,FILE *local_trace)
{
    if(!camera||!camera->initialized||camera->sensor!=FILM_CAMERA_REAR1)return FILM_UNSUPPORTED;
    if(camera->raw_preparation.attempted&&camera->raw_preparation.epoch==camera->raw_epoch)return camera->raw_preparation.status;
    if(!camera->raw_epoch)film_camera_raw_invalidate(camera);
    camera->raw_preparation=(FilmRawPreparation){.attempted=true,.epoch=camera->raw_epoch,.status=FILM_STATE};
    FilmStatus s=FILM_OK;
    if(camera->exposure_mode==FILM_EXPOSURE_MANUAL){
        if(camera->exposure.step<FILM_HW_MIN||camera->exposure.step>FILM_HW_MAX)s=FILM_STATE;
        else s=film_camera_verify_manual_capture(camera);
    }
    RawCamera c={.sensor_owner=camera,.log=local_trace};
    if(s==FILM_OK)s=film_register_open(&c.reg,(FilmRegisterIo){&c,reg_open,reg_exchange,reg_close});
    if(s==FILM_OK)s=film_raw_capture_prepare("film-camera-raw-prepare/v1",
        (FilmRawCaptureIo){.context=&c,.read=read_reg,.trace=trace},camera->raw_epoch,&camera->raw_preparation);
    if(s==FILM_OK&&camera->exposure_mode==FILM_EXPOSURE_MANUAL){
        unsigned step=(unsigned)(camera->exposure.step-FILM_HW_MIN);
        camera->raw_preparation.prepared_manual=true;
        camera->raw_preparation.prepared_exposure[0]=film_hw_coarse[step];
        for(unsigned i=1;i<5;++i)camera->raw_preparation.prepared_exposure[i]=film_hw_core[step];
    }
    if(c.reg.register_handle)camera->last_result=c.reg.native_result;
    film_register_close(&c.reg);camera->raw_preparation.status=s;
    if(local_trace){fprintf(local_trace,"source=film-camera-raw-prepare/v1 epoch=%lu ready=%u status=%s\n",
        (unsigned long)camera->raw_epoch,camera->raw_preparation.ready,film_status_name(s));fflush(local_trace);}
    return s;
}
FilmStatus film_camera_capture_raw(FilmCamera *camera,FilmFrame *out,FILE *local_trace)
{
    if(!camera||!out||out->source.pixels||!local_trace)return FILM_INVALID;
    if(!camera->initialized||camera->sensor!=FILM_CAMERA_REAR1)return FILM_UNSUPPORTED;
    FilmStatus allowed=film_camera_raw_shutter_check("film-camera-shutter-raw/v1",camera);
    if(allowed!=FILM_OK)return allowed; /* Never prepare or probe on the shutter. */
    FilmRawPreparation prepared=camera->raw_preparation;
    FilmStatus s=film_camera_preview_stop(camera);
    if(s==FILM_OK)s=film_camera_set_zoom(camera,1);
    if(s!=FILM_OK)return s;
    if(atomic_load(&camera->debug_mode))fprintf(local_trace,"RAW entry transfer=%u preview_stop_zoom=%s\n",camera->transfer,film_status_name(s));
    if(camera->transfer!=5120)return FILM_STATE;
    RawCamera c={.sensor_owner=camera,.log=local_trace,.out=out};
    Result rc=getter(&c,0x1a,SELECT_OUT1,&c.saved_ae);
    if(R_SUCCEEDED(rc))rc=getter(&c,0x1c,SELECT_OUT1,&c.saved_awb);
    if(R_FAILED(rc)||atomic_load(&camera->debug_mode))fprintf(local_trace,"RAW saved_auto result=%08lx ae=%u awb=%u mode=%u\n",(unsigned long)rc,c.saved_ae,c.saved_awb,camera->exposure_mode);
    if(R_FAILED(rc)||c.saved_ae!=(camera->exposure_mode==FILM_EXPOSURE_NATIVE_AE))return FILM_STATE;
    c.dma=linearMemAlign(614400+256,0x80);if(!c.dma)return FILM_NOMEM;
    if(atomic_load(&camera->debug_mode))fprintf(local_trace,"source=film-camera-raw-capture/v1 mode=%s exposure=live_shutter_snapshot no_fixed_coarse_or_gain=1\n",
        camera->exposure_mode==FILM_EXPOSURE_NATIVE_AE?"native_AE":"manual");
    if(s==FILM_OK)s=film_register_open(&c.reg,(FilmRegisterIo){&c,reg_open,reg_exchange,reg_close});
    FilmRawCaptureResult result={0};
    if(s==FILM_OK)s=film_raw_capture_transaction_result("film-camera-raw-capture/v1",
        (FilmRawCaptureIo){&c,read_reg,write_reg,lock,frame,idle,restore_auto,trace},&prepared,camera->raw_epoch,&result);
    FilmStatus stopped=idle(&c);if(stopped!=FILM_OK)s=stopped;
    film_register_close(&c.reg);
    if(s!=FILM_OK)film_camera_close(camera);
    if(!c.unsafe)linearFree(c.dma); /* Unsafe DMA is deliberately retained until process exit. */
    bool keep=result.raw_valid&&out->source.pixels;
    fprintf(local_trace,"source=film-camera-raw-capture/v1 RAW transaction=%s raw_received=%u raw_valid=%u camera_restored=%u dma_quarantined=%u camera_live=%u\n",
        film_status_name(s),result.raw_received,keep,result.camera_restored&&stopped==FILM_OK,c.unsafe,camera->initialized);fflush(local_trace);
    if(!keep)film_image_free(&out->source);
    else{out->camera=1;out->iso=out->exposure_us=0;
}
    return s;
}
