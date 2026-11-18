#pragma once
/* No pending DMA after the first completed receive. Only a cleared VSYNC
 * event with a strictly newer service timestamp counts as a new frame cycle.
 * Timestamps retain the service's raw unit; no sleep-to-frame inference. */
static bool no_receive_timing(Probe *p,const char *label,unsigned observation,const char *when,s64 *timing)
{
    *timing=0;
    Result rc=CAMU_GetLatestVsyncTiming(timing,p->port,1);
    record_call(p,label,"GetLatestVsyncTiming",0,1,rc);
    fprintf(p->meta,"NO_RECEIVE_TIMING observation=%u when=%s raw=%" PRId64 " result=%08" PRIx32 "\n",
            observation,when,(int64_t)*timing,(u32)rc);
    return rc==0 && *timing>0;
}
static bool no_receive_strict_rewrite(Probe *p)
{
    const u16 unity[4]={128,128,128,128};
    return sweep_isp(p,unity,"no_receive_rewrite");
}
static bool no_receive_frames_controlled(Probe *p,const char *label,bool (*rewrite)(Probe *))
{
    Handle vsync=0,dma=0;bool received=false;
    Result rc=CAMU_GetVsyncInterruptEvent(&vsync,p->port);
    record_call(p,label,"GetVsyncInterruptEvent",0,vsync,rc);
    bool ok=rc==0 && vsync && isp_update_arm_dma(p,label,&dma);
    if(ok) {
        rc=CAMU_StartCapture(p->port);record_call(p,label,"StartCapture",0,0,rc);
        p->streaming=rc==0;ok=p->streaming;
    }
    u64 start=osGetTime();
    Result wait=ok?svcWaitSynchronization(dma,INT64_C(2000000000)):(Result)-1;
    record_call(p,label,"WaitFrame",0,dma,wait);
    if(ok && wait==0) {
        rc=PROBE_FINISHED(&received,p->port);
        record_call(p,label,"IsFinishedReceiving",0,received,rc);
        ok=rc==0 && received;
    } else ok=false;
    if(dma) {
        rc=svcCloseHandle(dma);record_call(p,label,"CloseDMAEvent",0,dma,rc);
        ok=ok && rc==0;dma=0;
    }
    fprintf(p->frames,"%u,%u,%s,0,0,%u,%08" PRIx32 ",%u,0,0,0,0,00000000,%" PRIu64 ",%" PRIu64 "\n",
            p->sensor,p->phase,label,ok,(u32)wait,received,(uint64_t)start,(uint64_t)osGetTime());
    if(ok)++p->captures;
    if(ok)ok=isp_update_observe(p,label,0,"after_dma");
    if(ok) {
        fprintf(p->meta,"NO_RECEIVE_READY phase=%u pending=0 received=1 dma_closed=1\n",p->phase);
        ok=rewrite(p) && isp_update_observe(p,label,0,"after_rewrite");
    }
    s64 previous=0;unsigned completed=0;
    for(unsigned observation=1;observation<=NO_RECEIVE_VSYNC_COUNT && ok && running(p);++observation) {
        s64 baseline=0,latest=0;
        ok=no_receive_timing(p,label,observation,"baseline",&baseline);
        if(ok) {
            rc=svcClearEvent(vsync);record_call(p,label,"ClearVsyncEvent",0,vsync,rc);ok=rc==0;
        }
        Result signal=ok?svcWaitSynchronization(vsync,INT64_C(2000000000)):(Result)-1;
        if(ok)record_call(p,label,"WaitVsync",0,vsync,signal);
        ok=ok && signal==0;
        if(ok)ok=no_receive_timing(p,label,observation,"latest",&latest) && latest>baseline && latest>previous;
        fprintf(p->meta,"NO_RECEIVE_VSYNC observation=%u baseline=%" PRId64 " latest=%" PRId64 " verified=%u\n",
                observation,(int64_t)baseline,(int64_t)latest,ok);
        for(unsigned i=0;i<NO_RECEIVE_RED_READS && ok;++i) {
            u16 value=0;
            ok=read_reg(p,false,0x32d4,&value,label,(int)observation,"post_vsync","no_receive_red")==0 && value>=1 && value<=512;
        }
        if(ok)ok=isp_update_observe(p,label,(int)observation,"post_vsync");
        if(ok){previous=latest;++completed;}
        flush_logs(p);
    }
    if(p->streaming)stop(p,label);
    ok=ok && completed==NO_RECEIVE_VSYNC_COUNT && running(p) && p->capture_ready && !p->streaming;
    if(ok)ok=isp_update_observe(p,label,NO_RECEIVE_VSYNC_COUNT,"after_stop");
    if(vsync) {
        rc=svcCloseHandle(vsync);record_call(p,label,"CloseVsyncEvent",0,vsync,rc);ok=ok && rc==0;
    }
    if(ok) {
        rc=GSPGPU_InvalidateDataCache(p->pixels,BYTES);record_call(p,label,"InvalidateDataCache",0,0,rc);
        ok=rc==0 && save_frame(p,label);
    }
    fprintf(p->meta,"NO_RECEIVE_WINDOW complete=%u observations=%u raw_provenance=first_DMA_only\n",ok,completed);
    if(!ok)++p->failures;
    return ok;
}
static bool no_receive_frames(Probe *p,const char *label)
{return no_receive_frames_controlled(p,label,no_receive_strict_rewrite);}
static bool no_receive_probe(Probe *p)
{
    if(p->calibration_part!=CAL_NO_RECEIVE_UPDATE || p->sensor!=1 || !p->raw ||
       !ram_log.exposure_sweep || p->automation_fps!=5)return false;
    fprintf(p->meta,"NO_RECEIVE_CONTRACT source=%s arms=2 vsync=3 sensor=1 fps=5 retry=none\n",NO_RECEIVE_SOURCE);
    u16 saved[5]={0},isp[4]={0};bool saved_ok=false;
    bool ok=isp_update_baseline(p,saved,isp,&saved_ok);
    if(!saved_ok)return false;
    unsigned complete=0;
    for(unsigned arm=0;arm<2 && ok && running(p);++arm) {
        ++p->phase;++p->samples;
        const char *label=arm?"no_receive_128":"receive_control_128";
        fprintf(p->meta,"NO_RECEIVE_ARM index=%u phase=%u label=%s\n",arm,p->phase,label);
        ok=isp_update_prepare_arm(p,128,label);
        if(ok)ok=arm?no_receive_frames(p,label):isp_update_frames(p,label,true,false);
        fprintf(p->meta,"NO_RECEIVE_ARM_END index=%u phase=%u complete=%u\n",arm,p->phase,ok);
        if(ok){++complete;++p->successful_samples;}
        flush_logs(p);
    }
    if(p->streaming)stop(p,"no_receive_cleanup");
    bool restored=true;
    if(p->sweep_refresh_active)restored=sweep_refresh_restore(p);
    bool controls=cal_restore_controls(p,saved,"sweep_restore_controls");
    bool gains=sweep_isp(p,isp,"sweep_restore_isp");
    restored=restored && controls && gains;
    bool done=ok && complete==2 && restored && running(p);
    fprintf(p->meta,"NO_RECEIVE_END complete=%u arms=%u restored=%u\n",done,complete,restored);
    p->check_isp=false;p->isp_pin_pending=false;flush_logs(p);return done;
}
