#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
/* A single registered rendezvous, not a command server or work queue. No
 * camera operations while waiting READY/GO or DONE/RELEASE. Socket fragments
 * may complete one fixed word; malformed/disconnected input never restarts. */
static bool writer_word(int fd,const char *word,bool outgoing,unsigned timeout_ms)
{
    char buffer[64];size_t length=strlen(word),used=0;
    if(length>=sizeof buffer)return false;
    u64 end=osGetTime()+timeout_ms;
    while(used<length) {
        u64 now=osGetTime();if(now>=end)return false;
        struct pollfd ready={fd,outgoing?POLLOUT:POLLIN,0};
        if(poll(&ready,1,(int)(end-now))<=0 || (ready.revents&(POLLERR|POLLNVAL)))return false;
        ssize_t n=outgoing?send(fd,word+used,length-used,0):recv(fd,buffer+used,length-used,0);
        if(n<=0)return false;
        used+=(size_t)n;
    }
    return outgoing || !memcmp(buffer,word,length);
}
static bool writer_observed_rewrite(Probe *p)
{
    if(p->calibration_part!=CAL_CAMERA_WRITER_OBSERVE)return false;
    u16 values[4]={0};bool ok=true,unity=true;
    for(unsigned i=0;i<4;++i) {
        if(!write_reg(p,auto_isp_addresses[i],128,"no_receive_rewrite"))ok=false;
        Result rc=read_reg(p,false,auto_isp_addresses[i],&values[i],"no_receive_rewrite",-1,"observed","writer_trace_readback");
        if(rc!=0 || values[i]<1 || values[i]>512)ok=false;
        if(values[i]!=128)unity=false;
    }
    fprintf(p->meta,"WRITER_REWRITE writes_ok=%u unity_readback=%u r=%u gr=%u gb=%u b=%u\n",ok,unity,values[0],values[1],values[2],values[3]);
    return ok;
}
static bool writer_gated_frames(Probe *p,const char *label)
{
    int listener=-1,client=-1;void *soc_buffer=NULL;bool soc_ready=false,ok=false;
#ifdef __3DS__
    soc_buffer=memalign(0x1000,0x100000);
    Result soc=soc_buffer?socInit(soc_buffer,0x100000):(Result)-1;
    record_call(p,label,"WriterGateSocInit",0,0,soc);
    if(R_FAILED(soc))goto cleanup;
    soc_ready=true;
#endif
    listener=socket(AF_INET,SOCK_STREAM,0);int reuse=1;
    struct sockaddr_in address={0};address.sin_family=AF_INET;
    address.sin_port=htons(WRITER_GATE_PORT);address.sin_addr.s_addr=INADDR_ANY;
    if(listener<0)goto cleanup;
    setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof reuse);
    if(bind(listener,(struct sockaddr *)&address,sizeof address) || listen(listener,1))goto cleanup;
    fprintf(p->meta,"WRITER_GATE state=listening port=%u phase=%u\n",WRITER_GATE_PORT,p->phase);flush_logs(p);
    struct pollfd ready={listener,POLLIN,0};
    if(poll(&ready,1,WRITER_ACCEPT_MS)<=0)goto cleanup;
    client=accept(listener,NULL,NULL);close(listener);listener=-1;
    if(client<0 || !writer_word(client,"FILMTRACE1 READY\n",true,5000) ||
       !writer_word(client,"GO\n",false,WRITER_START_MS))goto cleanup;
    fprintf(p->meta,"WRITER_GATE state=GO phase=%u\n",p->phase);flush_logs(p);
    ok=p->calibration_part==CAL_CAMERA_WRITER_OBSERVE?
        no_receive_frames_controlled(p,label,writer_observed_rewrite):no_receive_frames(p,label);
    if(!writer_word(client,ok?"FILMTRACE1 DONE ok=1\n":"FILMTRACE1 DONE ok=0\n",true,5000))ok=false;
    bool release=writer_word(client,"RELEASE\n",false,WRITER_RELEASE_MS);
    fprintf(p->meta,"WRITER_GATE state=RELEASE received=%u phase=%u\n",release,p->phase);
    ok=ok && release;
cleanup:
    if(client>=0)close(client);
    if(listener>=0)close(listener);
#ifdef __3DS__
    if(soc_ready)socExit();
#else
    (void)soc_ready;
#endif
    free(soc_buffer);
    fprintf(p->meta,"WRITER_GATE state=closed complete=%u phase=%u\n",ok,p->phase);
    return ok;
}
static bool writer_window_probe(Probe *p)
{
    if((p->calibration_part!=CAL_CAMERA_WRITER_WINDOW && p->calibration_part!=CAL_CAMERA_WRITER_OBSERVE) || p->sensor!=1 || !p->raw ||
       !ram_log.exposure_sweep || p->automation_fps!=5)return false;
    fprintf(p->meta,"WRITER_WINDOW_CONTRACT source=%s arms=1 vsync=3 sensor=1 fps=5 retry=none\n",
            p->calibration_part==CAL_CAMERA_WRITER_OBSERVE?WRITER_OBSERVE_SOURCE:WRITER_WINDOW_SOURCE);
    u16 saved[5]={0},isp[4]={0};bool saved_ok=false;
    bool ok=isp_update_baseline(p,saved,isp,&saved_ok);
    if(!saved_ok)return false;
    if(ok) {
        ++p->phase;++p->samples;
        const char *label="no_receive_128";
        ok=isp_update_prepare_arm(p,128,label);
        if(ok && p->calibration_part==CAL_CAMERA_WRITER_OBSERVE) {
            for(unsigned i=0;i<4 && ok;++i) {
                u16 value=0;
                ok=read_reg(p,false,auto_isp_addresses[i],&value,label,-1,"pre_GO","writer_unit_baseline")==0 && value==128;
            }
        }
        if(ok)ok=writer_gated_frames(p,label);
        if(ok)++p->successful_samples;
    }
    if(p->streaming)stop(p,"writer_window_cleanup");
    bool restored=true;
    if(p->sweep_refresh_active)restored=sweep_refresh_restore(p);
    bool controls=cal_restore_controls(p,saved,"sweep_restore_controls");
    bool gains=sweep_isp(p,isp,"sweep_restore_isp");restored=restored && controls && gains;
    bool done=ok && restored && running(p);
    fprintf(p->meta,"WRITER_WINDOW_END complete=%u restored=%u\n",done,restored);
    p->check_isp=false;p->isp_pin_pending=false;flush_logs(p);return done;
}
