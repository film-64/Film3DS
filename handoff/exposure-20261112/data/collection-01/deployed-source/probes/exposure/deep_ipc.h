#pragma once
#include "ipc_decode.h"
#ifdef __3DS__
/* A separate diagnostic path, not a replacement for libctru or the application. */
extern Handle camHandle;
static FILE *deep_ipc_log;
static Handle deep_register_handle;
static Handle deep_extra_handles[2];
static const char *deep_register_service="cam:u";
static unsigned deep_sensor;
typedef struct { Result transport, service; u32 words[64]; } DeepReply;
static Result deep_exchange(Handle handle, const char *service, unsigned command,
                            const u32 *args, unsigned count, u32 poison, DeepReply *reply)
{
    u32 request[64]={0};
    request[0]=IPC_MakeHeader(command,count,0);
    for (unsigned i=0; i<count; ++i) request[i+1]=args[i];
    if (count<2) request[2]=poison;
    u32 *cmdbuf=getThreadCommandBuffer();
    memcpy(cmdbuf,request,sizeof request);
    u64 started=osGetTime();
    reply->transport=svcSendSyncRequest(handle);
    /* Copy before any filesystem/clock/service calls can reuse thread IPC. */
    memcpy(reply->words,cmdbuf,sizeof reply->words);
    reply->service=R_FAILED(reply->transport)?reply->transport:(Result)reply->words[1];
    if (deep_ipc_log) {
        fprintf(deep_ipc_log,"%u,%s,%04x,%08" PRIx32 ",%08" PRIx32 ",%" PRIu64 ",",
                deep_sensor,service,command,(u32)reply->transport,(u32)reply->service,started);
        for (unsigned i=0; i<8; ++i) fprintf(deep_ipc_log,"%08" PRIx32,request[i]);
        fputc(',',deep_ipc_log);
        for (unsigned i=0; i<64; ++i) fprintf(deep_ipc_log,"%08" PRIx32,reply->words[i]);
        fputc('\n',deep_ipc_log);
    }
    return reply->service;
}
static Result deep_value(Handle handle,const char *service,unsigned command,
                         const u32 *args,unsigned count,unsigned width,u32 poison,u32 *value)
{
    DeepReply reply;
    Result rc=deep_exchange(handle,service,command,args,count,poison,&reply);
    *value=0;
    if (R_SUCCEEDED(rc) && !probe_reply_value(reply.words[0],command,reply.words[2],width,value))
        return MAKERESULT(RL_PERMANENT,RS_INVALIDSTATE,RM_CAM,RD_INVALID_RESULT_VALUE);
    return rc;
}
static Result deep_auto(bool *value,u32 select,bool ae)
{
    u32 raw=0;
    Result rc=deep_value(camHandle,"cam:u",ae?0x1a:0x1c,&select,1,8,0,&raw);
    *value=R_SUCCEEDED(rc) && raw!=0;
    return rc;
}
static Result deep_busy(bool *value,u32 port)
{
    u32 raw=0;
    Result rc=deep_value(camHandle,"cam:u",3,&port,1,8,0,&raw);
    *value=R_SUCCEEDED(rc) && raw!=0;
    return rc;
}
static Result deep_finished(bool *value,u32 port)
{
    u32 raw=0;
    Result rc=deep_value(camHandle,"cam:u",8,&port,1,8,0,&raw);
    *value=R_SUCCEEDED(rc) && raw!=0;
    return rc;
}
static Result deep_read_reg(u16 *value,u32 select,u16 addr,bool mcu)
{
    u32 args[]={select,addr},raw=0;
    Result rc=deep_value(deep_register_handle?deep_register_handle:camHandle,
        deep_register_service,mcu?0x30:0x2f,args,2,16,0,&raw);
    *value=R_SUCCEEDED(rc)?(u16)raw:0;
    return rc;
}
static Result deep_write_reg(u32 select,u16 addr,u16 value,bool mcu)
{
    u32 args[]={select,addr,value}; DeepReply reply;
    return deep_exchange(deep_register_handle?deep_register_handle:camHandle,
        deep_register_service,mcu?0x2e:0x2d,args,3,0,&reply);
}
static void deep_ports(FILE *meta,u32 select)
{
    deep_register_handle=camHandle; deep_register_service="cam:u";
    const char *names[]={"cam:u","cam:c","cam:s"};
    for (unsigned i=0; i<3; ++i) {
        Handle h=camHandle;
        Result opened=i?srvGetServiceHandle(&h,names[i]):0;
        fprintf(meta,"SERVICE sensor=%u name=%s open=%08" PRIx32 "\n",deep_sensor,names[i],(u32)opened);
        if (R_FAILED(opened)) continue;
        if (i) deep_extra_handles[i-1]=h;
        u32 args[]={select,0},id=0;
        Result rc=deep_value(h,names[i],0x2f,args,2,16,0,&id);
        fprintf(meta,"SERVICE_ID sensor=%u name=%s result=%08" PRIx32 " id=%04" PRIx32 "\n",deep_sensor,names[i],(u32)rc,id);
        if (R_SUCCEEDED(rc) && id==0x2280) { deep_register_handle=h;deep_register_service=names[i]; }
        /* Getter poison comparison diagnoses stale upper bits without changing camera state. */
        for (unsigned j=0;j<2;++j) {
            u32 value=0;
            rc=deep_value(h,names[i],0x1a,&select,1,8,j?0xa5a5a5a5:0,&value);
            fprintf(meta,"GETTER sensor=%u service=%s poison=%u result=%08" PRIx32 " ae_low8=%" PRIu32 "\n",deep_sensor,names[i],j,(u32)rc,value);
        }
    }
}
static void deep_ports_close(void)
{
    for(unsigned i=0;i<2;++i) if(deep_extra_handles[i]) {svcCloseHandle(deep_extra_handles[i]);deep_extra_handles[i]=0;}
    deep_register_handle=0;deep_register_service="cam:u";
}
#define PROBE_AE(v,s) deep_auto(v,s,true)
#define PROBE_AWB(v,s) deep_auto(v,s,false)
#define PROBE_FINISHED(v,p) deep_finished(v,p)
#define PROBE_BUSY(v,p) deep_busy(v,p)
#define PROBE_READ(v,s,a,m) deep_read_reg(v,s,a,m)
#define PROBE_WRITE(s,a,v,m) deep_write_reg(s,a,v,m)
#else
#define PROBE_AE(v,s) CAMU_IsAutoExposure(v,s)
#define PROBE_AWB(v,s) CAMU_IsAutoWhiteBalance(v,s)
#define PROBE_FINISHED(v,p) CAMU_IsFinishedReceiving(v,p)
#define PROBE_BUSY(v,p) CAMU_IsBusy(v,p)
#define PROBE_READ(v,s,a,m) ((m)?CAMU_ReadMcuVariableI2cExclusive(v,s,a):CAMU_ReadRegisterI2cExclusive(v,s,a))
#define PROBE_WRITE(s,a,v,m) ((m)?CAMU_WriteMcuVariableI2c(s,a,v):CAMU_WriteRegisterI2c(s,a,v))
#endif
