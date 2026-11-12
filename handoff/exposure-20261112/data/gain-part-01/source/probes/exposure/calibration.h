#pragma once
static bool cal_open(Probe *p)
{
    const char *names[]={"calibration_plan.csv","frame_burst.csv","roi.csv","noise.csv","execution.csv"};
    FILE **files[]={&p->calibration,&p->burst,&p->roi,&p->noise,&p->execution};
    for(unsigned i=0;i<5;++i) {
        char path[240];snprintf(path,sizeof path,"%s/%s",p->directory,names[i]);
        *files[i]=fopen(path,"w");if(!buffer_log(p,*files[i]))return false;
    }
    fputs("sensor,phase,label,kind,settle,measured,baseline_phase,write_mask,coarse,gain_gr,gain_b,gain_r,gain_gb,locked\n",p->calibration);
    fputs("sensor,rate,frame,time_ms,wait_result,receive_result,received,buffer_error\n",p->burst);
    fputs("sensor,phase,label,frame,tile,x,y,width,height,mean,spatial_variance,low,high\n",p->roi);
    fputs("sensor,phase,label,frames,width,height,mean,temporal_variance,temporal_sigma\n",p->noise);
    fputs("sensor,phase,label,frame,step\n",p->execution);
    return true;
}
static bool cal_read(Probe *p,u16 *values,const char *label)
{
    bool ok=true;
    for(unsigned i=0;i<5;++i)
        if(R_FAILED(read_reg(p,false,cal_addresses[i],values+i,label,-1,"saved","calibration_saved")))ok=false;
    return ok;
}
static bool cal_locked(Probe *p,const char *label)
{
    bool awb=true;
    Result rc=record_call(p,label,"SetAutoWhiteBalance",0,0,CAMU_SetAutoWhiteBalance(p->select,false));
    Result state=PROBE_AWB(&awb,p->select);
    record_call(p,label,"IsAutoWhiteBalance",0,awb,state);
    if(!set_ae(p,false,label) || R_FAILED(rc) || R_FAILED(state) || awb)return false;
    u16 ae_mode=1,awb_mode=1;
    Result a=read_reg(p,true,0xa11d,&ae_mode,label,-1,"lock","sequencer_ae");
    Result b=read_reg(p,true,0xa11f,&awb_mode,label,-1,"lock","sequencer_awb");
    return R_SUCCEEDED(a) && R_SUCCEEDED(b) && !ae_mode && !awb_mode;
}
static bool cal_stage(Probe *p,const char *label,const char *kind,const u16 *expected,
                      unsigned mask,unsigned baseline,bool long_sample,bool restoring)
{
    if(!cal_rule(kind,mask))return false;
    ++p->phase;p->settle_frames=long_sample?1:8;p->measured_frames=long_sample?3:12;
    p->wait_ns=(long_sample || (restoring && p->wait_ns>INT64_C(2000000000)))?INT64_C(30000000000):INT64_C(2000000000);
    fprintf(p->calibration,"%u,%u,%s,%s,%u,%u,%u,%u,",p->sensor,p->phase,label,kind,
        p->settle_frames,p->measured_frames,baseline,mask);
    for(unsigned i=0;i<5;++i) {
        if(expected)fprintf(p->calibration,"%u",expected[i]);
        fputc(',',p->calibration);
    }
    fprintf(p->calibration,"%u\n",expected!=NULL);flush_logs(p);
    bool ok=true;
    for(unsigned i=0;i<5;++i)if(mask&(1u<<i)) {
        if(!restoring && !running(p)){ok=false;break;}
        if(!expected || !write_reg(p,cal_addresses[i],expected[i],label))ok=false;
    }
    if(expected) {
        u16 actual[5]={0};if(!cal_read(p,actual,label))ok=false;
        for(unsigned i=0;i<5;++i)if(actual[i]!=expected[i])ok=false;
        u16 ae_mode=1,awb_mode=1;
        Result a=read_reg(p,true,0xa11d,&ae_mode,label,-1,"verified","sequencer_ae");
        Result b=read_reg(p,true,0xa11f,&awb_mode,label,-1,"verified","sequencer_awb");
        ok=ok && R_SUCCEEDED(a) && R_SUCCEEDED(b) && !ae_mode && !awb_mode;
    }
    fprintf(p->meta,"CAL_STAGE sensor=%u phase=%u kind=%s controls_verified=%u restoring=%u\n",
        p->sensor,p->phase,kind,ok,restoring);flush_logs(p);
    if(!ok || !running(p))return false;
    return sample(p,label) && p->phase_success;
}
static bool cal_pair(Probe *p,const char *label,const char *kind,const u16 *target,
                     const u16 *base,unsigned mask,bool long_sample)
{
    unsigned baseline=long_sample?0:p->phase;
    bool measured=cal_stage(p,label,kind,target,mask,baseline,long_sample,false);
    /* Restoration is attempted even after abort, timeout, rejected write or SD error. */
    bool restored=cal_stage(p,"cal_restore","restore",base,mask,baseline,false,true);
    fprintf(p->meta,"CAL_PAIR sensor=%u phase=%u measured=%u restored=%u\n",p->sensor,p->phase,measured,restored);
    if(!restored)p->aborted=true;
    return measured && restored;
}
static void cal_burst(Probe *p,unsigned rate)
{
    struct {u64 time;Result wait,result;bool received;} evidence[CAL_BURST_FRAMES]={0};
    stop(p,"cal_burst_prepare");if(!p->capture_ready)return;
#ifdef __3DS__
    FILE *ipc=deep_ipc_log;deep_ipc_log=NULL;
    if(p->error_event && svcWaitSynchronization(p->error_event,0)==0)svcClearEvent(p->error_event);
#endif
    Result result=GSPGPU_FlushDataCache(p->pixels,BYTES);unsigned completed=0,attempted=0;
    for(unsigned i=0;i<CAL_BURST_FRAMES && R_SUCCEEDED(result) && running(p);++i) {
        Handle event=0;result=CAMU_SetReceiving(&event,p->pixels,p->port,BYTES,(s16)p->transfer);
        if(R_SUCCEEDED(result) && !p->streaming){result=CAMU_StartCapture(p->port);p->streaming=R_SUCCEEDED(result);}
        Result wait=R_SUCCEEDED(result)?svcWaitSynchronization(event,INT64_C(3000000000)):result;
        bool received=false;
        if(wait==0)result=PROBE_FINISHED(&received,p->port);
        evidence[i].time=osGetTime();evidence[i].wait=wait;evidence[i].result=result;evidence[i].received=received;
        ++attempted;if(event)svcCloseHandle(event);
        if(wait!=0 || R_FAILED(result) || !received)break;
        ++completed;
    }
    bool buffer_error=false;
#ifdef __3DS__
    if(p->error_event)buffer_error=svcWaitSynchronization(p->error_event,0)==0;
    deep_ipc_log=ipc;
#endif
    stop(p,"cal_burst_end");
    for(unsigned i=0;i<attempted;++i)fprintf(p->burst,"%u,%u,%u,%" PRIu64 ",%08" PRIx32 ",%08" PRIx32 ",%u,%u\n",
        p->sensor,rate,i,evidence[i].time,(u32)evidence[i].wait,(u32)evidence[i].result,evidence[i].received,buffer_error);
    fprintf(p->meta,"CAL_BURST sensor=%u rate=%u frames=%u expected=%u complete=%u evidence=dma_event_cadence\n",
        p->sensor,rate,completed,CAL_BURST_FRAMES,completed==CAL_BURST_FRAMES && !buffer_error);
    flush_logs(p);
}
static bool cal_setup_profile(Probe *p,unsigned rate)
{
    Result mode=record_call(p,"cal_setup","SetPhotoModeNormal",0,0,CAMU_SetPhotoMode(p->select,PHOTO_MODE_NORMAL));
    Result fps=record_call(p,"cal_setup","SetFrameRate",0,rate,CAMU_SetFrameRate(p->select,(CAMU_FrameRate)rate));
    Result exposure=record_call(p,"cal_setup","SetExposure",0,0,CAMU_SetExposure(p->select,0));
    Result wb=record_call(p,"cal_setup","SetAutoWhiteBalance",0,1,CAMU_SetAutoWhiteBalance(p->select,true));
    return R_SUCCEEDED(mode) && R_SUCCEEDED(fps) && R_SUCCEEDED(exposure) && R_SUCCEEDED(wb) && set_ae(p,true,"cal_seed");
}
static bool cal_normalize(const u16 *saved,u16 *base)
{
    memcpy(base,saved,5*sizeof *base);
    double green=cal_analog(saved[1]);if(green<=0)return false;
    for(unsigned i=1;i<5;++i)
        if(!cal_digital(saved[i],1,base+i) || !cal_encode_analog(base[i],cal_analog(saved[i])/green,base+i))return false;
    return true;
}
static bool cal_restore_controls(Probe *p,const u16 *saved,const char *label)
{
    bool ok=true;u16 actual[5]={0};
    for(unsigned i=0;i<5;++i)if(!write_reg(p,cal_addresses[i],saved[i],label))ok=false;
    if(!cal_read(p,actual,label))ok=false;
    for(unsigned i=0;i<5;++i)if(actual[i]!=saved[i])ok=false;
    return ok;
}
static bool calibration_probe(Probe *p)
{
    if(!cal_scope_stages(p->calibration_part) || !p->map_known || !p->capture_ready || !cal_open(p))return false;
    for(unsigned profile=0;profile<3 && running(p);++profile) {
        if(p->calibration_part==CAL_LIMITS || (p->calibration_part==CAL_GAIN && profile!=1) || (p->calibration_part==CAL_SLOW && profile!=2))continue;
        unsigned rate=cal_rates[profile];char label[40];
        if(!cal_setup_profile(p,rate))return false;
        snprintf(label,sizeof label,"cal_r%u_auto",rate);
        if(!cal_stage(p,label,"seed",NULL,0,0,false,false))return false;
        if(p->smoke)return p->successful_samples==1;
        if(!cal_locked(p,"cal_lock"))return false;
        u16 saved[5],base[5],frame_lines=0;
        if(!cal_read(p,saved,"cal_saved") || R_FAILED(read_reg(p,false,0x300a,&frame_lines,"cal_saved",-1,"saved","frame_lines")) || frame_lines<128)return false;
        if(!cal_normalize(saved,base))return false;
        snprintf(label,sizeof label,"cal_r%u_baseline",rate);
        bool profile_ok=cal_stage(p,label,"normalize",base,30,0,false,false);
        unsigned reference=p->phase;
        if(profile_ok)cal_burst(p,rate);
        const u16 coarse[]={1,4,16,64,(u16)(frame_lines/4),(u16)(frame_lines*3/4)};
        for(unsigned i=0;i<6 && p->calibration_part!=CAL_GAIN && profile_ok && running(p);++i) {
            u16 target[5];memcpy(target,base,sizeof target);target[0]=coarse[i];
            snprintf(label,sizeof label,"cal_r%u_t%u",rate,coarse[i]);
            profile_ok=cal_pair(p,label,"integration",target,base,1,false);
        }
        if(profile_ok && rate==0) {
            const double scales[]={.5,1,2,4};
            for(unsigned point=0;point<4 && profile_ok && running(p);++point) {
                u16 target[5];memcpy(target,base,sizeof target);bool valid=true;
                for(unsigned i=1;i<5;++i)valid=cal_encode_analog(base[i],cal_analog(base[i])*scales[point],target+i) && valid;
                if(!valid){fprintf(p->meta,"CAL_REJECT reason=analog_range point=%u\n",point);profile_ok=false;break;}
                snprintf(label,sizeof label,"cal_analog_%u",point);
                profile_ok=cal_pair(p,label,"analog",target,base,30,false);
            }
            for(unsigned multiplier=2;multiplier<=4 && profile_ok && running(p);multiplier*=2) {
                u16 target[5];memcpy(target,base,sizeof target);
                for(unsigned i=1;i<5;++i)if(!cal_digital(base[i],multiplier,target+i))profile_ok=false;
                snprintf(label,sizeof label,"cal_digital_%u",multiplier);
                if(profile_ok)profile_ok=cal_pair(p,label,"digital",target,base,30,false);
            }
        }
        /* Restore the seed settings even when a profile fails; no implicit rerun. */
        bool original_restored=cal_restore_controls(p,saved,"cal_profile_restore");
        fprintf(p->meta,"CAL_PROFILE rate=%u reference_phase=%u complete=%u seed_restored=%u\n",rate,reference,profile_ok,original_restored);
        flush_logs(p);
        if(!profile_ok || !original_restored)return false;
    }
    if(p->calibration_part==CAL_GAIN || p->calibration_part==CAL_SLOW)
        return running(p) && p->phase==cal_scope_stages(p->calibration_part)+2 && p->successful_samples==cal_scope_stages(p->calibration_part);
    /* Boundary experiments are last, so a rejection cannot erase core coverage.
     * Continue to the other boundary only after a fully sampled verified restore.
     * A rejected boundary remains incomplete and never increments success counts. */
    for(unsigned boundary=0;boundary<2 && running(p);++boundary) {
        unsigned rate=boundary?5:8;char label[40];
        u16 seed[5]={0};
        if(p->calibration_part==CAL_LIMITS) {
            if(!cal_setup_profile(p,rate))return false;
            snprintf(label,sizeof label,"cal_r%u_limit_auto",rate);
            if(!cal_stage(p,label,"seed",NULL,0,0,false,false) || !cal_locked(p,"cal_boundary_lock"))return false;
            u16 normalized[5];
            if(!cal_read(p,seed,"cal_boundary_seed") || !cal_normalize(seed,normalized))return false;
            snprintf(label,sizeof label,"cal_r%u_limit_baseline",rate);
            if(!cal_stage(p,label,"normalize",normalized,30,0,false,false)) {
                cal_restore_controls(p,seed,"cal_boundary_seed_restore_failed_stage");return false;
            }
        }
        Result fps=record_call(p,"cal_boundary_setup","SetFrameRate",0,rate,CAMU_SetFrameRate(p->select,(CAMU_FrameRate)rate));
        if(R_FAILED(fps) || !cal_locked(p,"cal_boundary_lock"))return false;
        u16 base[5],target[5];if(!cal_read(p,base,"cal_boundary_saved"))return false;
        memcpy(target,base,sizeof target);target[0]=65535;
        snprintf(label,sizeof label,"cal_r%u_long_limit",rate);
        bool measured=cal_pair(p,label,"integration_long",target,base,1,true);
        fprintf(p->meta,"CAL_BOUNDARY rate=%u measured=%u restore_sampled=%u\n",rate,measured,p->phase_success);
        if(p->calibration_part==CAL_LIMITS) {
            bool restored=cal_restore_controls(p,seed,"cal_boundary_seed_restore");
            fprintf(p->meta,"CAL_BOUNDARY_SEED rate=%u restored=%u\n",rate,restored);flush_logs(p);
            if(!restored)return false;
        }
        if(!measured && (!p->phase_success || !p->capture_ready || p->aborted))return false;
    }
    return running(p) && p->phase==cal_scope_stages(p->calibration_part)+2 && p->successful_samples==cal_scope_stages(p->calibration_part);
}
