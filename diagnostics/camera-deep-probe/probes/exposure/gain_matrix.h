#pragma once
/* A registered finite matrix. Overwrite is an observation; configuration or
 * transport failure stops the run. Branches select new targets, never retries. */
typedef struct {unsigned index,fps,gate,coarse;u16 dgain,line; s64 period;} GainMatrixNode;
static bool gain_matrix_state(Probe *p,GainMatrixNode *n,const char *label,int frame,const char *when,unsigned target,bool *held)
{
    bool same=true;
    for(unsigned i=0;i<sizeof gain_matrix_registers/sizeof *gain_matrix_registers;++i) {
        u16 a=(u16)gain_matrix_registers[i],v=0;bool mcu=(a&0x8000) || a==0x221f;
        bool valid=read_reg(p,mcu,a,&v,label,frame,when,"gain_matrix_state")==0;
        if(a==0xa102)valid=valid && v==p->sweep_seq_target && !(v&13);
        if(a==0xa103)valid=valid && !v;
        if(a==0xa104)valid=valid && v==3;
        if(a==0xa355)valid=valid && (v&96)==32;
        if(a==0xa34e || a==0xa34f || a==0xa350)valid=valid && v==128;
        if(a==0x3012)valid=valid && v==n->coarse;
        if(a>=0x3056 && a<=0x305c)valid=valid && v==528;
        if(a==0x300a)valid=valid && v==(n->fps==5?2114:704);
        if(a==0x300c) {if(!n->line)n->line=v;valid=valid && v && v==n->line;}
        if(a==0x221f) {if(!n->dgain)n->dgain=v;valid=valid && v>=1 && v<=512 && v==n->dgain;}
        if(a>=0x32d4 && a<=0x32da) {valid=valid && v>=1 && v<=512;if(v!=target)same=false;}
        if(!valid) {fprintf(p->meta,"MATRIX_REJECT node=%u when=%s addr=%04x value=%u\n",n->index,when,a,v);return false;}
    }
    if(held)*held=same;
    return true;
}
static bool gain_matrix_snapshot(Probe *p,GainMatrixNode *n,const char *label,const char *when)
{
    if(!hg_snapshot(p,label,when))return false;
    for(unsigned i=0;i<sizeof isp_update_mode/sizeof *isp_update_mode;++i) {
        u16 v=0;
        if(read_reg(p,true,(u16)isp_update_mode[i],&v,label,-1,when,"matrix_mode")!=0)return false;
    }
    fprintf(p->meta,"MATRIX_SNAPSHOT node=%u when=%s complete=1\n",n->index,when);return true;
}
static bool gain_matrix_profile(Probe *p,unsigned fps,const char *label)
{
    Result rc=CAMU_SetFrameRate(p->select,(CAMU_FrameRate)(fps==5?5:0));
    record_call(p,label,"SetFrameRate",0,fps==5?5:0,rc);if(rc!=0)return false;
    unsigned stable=0;
    for(unsigned i=0;i<100;++i) {
        u16 lines=0,cmd=0,state=0;
        if(read_reg(p,false,0x300a,&lines,label,(int)i,"profile_poll","matrix_profile")!=0 ||
           read_reg(p,true,0xa103,&cmd,label,(int)i,"profile_poll","matrix_profile")!=0 ||
           read_reg(p,true,0xa104,&state,label,(int)i,"profile_poll","matrix_profile")!=0)return false;
        stable=(lines==(fps==5?2114:704) && !cmd && state==3)?stable+1:0;
        if(stable==2){fprintf(p->meta,"MATRIX_PROFILE fps=%u polls=%u verified=1\n",fps,i+1);return true;}
        svcSleepThread(INT64_C(10000000));
    }
    return false;
}
static bool gain_matrix_dma(Probe *p,const char *label,int frame,bool start)
{
    Handle event=0;bool received=false;u64 began=osGetTime();
    Result rc=GSPGPU_FlushDataCache(p->pixels,BYTES);record_call(p,label,"FlushDataCache",0,0,rc);
    bool ok=rc==0 && isp_update_arm_dma(p,label,&event);
    if(ok && start){rc=CAMU_StartCapture(p->port);record_call(p,label,"StartCapture",0,0,rc);p->streaming=rc==0;ok=p->streaming;}
    Result wait=ok?svcWaitSynchronization(event,INT64_C(2000000000)):(Result)-1;
    record_call(p,label,"WaitFrame",0,event,wait);ok=ok && wait==0;
    if(ok){rc=PROBE_FINISHED(&received,p->port);record_call(p,label,"IsFinishedReceiving",0,received,rc);ok=rc==0 && received;}
    if(event){rc=svcCloseHandle(event);record_call(p,label,"CloseDMAEvent",0,event,rc);ok=ok && rc==0;}
    if(ok){rc=GSPGPU_InvalidateDataCache(p->pixels,BYTES);record_call(p,label,"InvalidateDataCache",0,0,rc);ok=rc==0;}
    fprintf(p->frames,"%u,%u,%s,%d,0,%u,%08" PRIx32 ",%u,0,0,0,0,00000000,%" PRIu64 ",%" PRIu64 "\n",
            p->sensor,p->phase,label,frame,ok,(u32)wait,received,(uint64_t)began,(uint64_t)osGetTime());
    if(ok)++p->captures;
    return ok;
}
static bool gain_matrix_wait(Probe *p,Handle event,const char *label,s64 *baseline,s64 *latest,u64 *returned)
{
    Result rc=CAMU_GetLatestVsyncTiming(baseline,p->port,1);record_call(p,label,"GetLatestVsyncTiming",0,1,rc);
    bool ok=rc==0 && *baseline>0;
    if(ok){rc=svcClearEvent(event);record_call(p,label,"ClearVsyncEvent",0,event,rc);ok=rc==0;}
    if(ok){rc=svcWaitSynchronization(event,INT64_C(2000000000));record_call(p,label,"WaitVsync",0,event,rc);ok=rc==0;}
    *returned=osGetTime();
    if(ok){rc=CAMU_GetLatestVsyncTiming(latest,p->port,1);record_call(p,label,"GetLatestVsyncTiming",0,1,rc);ok=rc==0 && *latest>*baseline;}
    return ok;
}
static bool gain_matrix_period(Probe *p,GainMatrixNode *n,Handle event,const char *label,FILE *events)
{
    s64 steps[3];u64 times[3];
    for(unsigned i=0;i<3;++i) {
        s64 baseline=0,latest=0;u64 returned=0;
        if(!gain_matrix_wait(p,event,label,&baseline,&latest,&returned))return false;
        steps[i]=latest-baseline;times[i]=returned;
        fprintf(events,"%u,%u,%u,-1,0,%u,3,%" PRId64 ",%" PRId64 ",0,%" PRIu64 ",0,0,0\n",
                n->index,p->phase,n->fps,i,(int64_t)baseline,(int64_t)latest,(uint64_t)returned);
    }
    for(unsigned i=0;i<3;++i)for(unsigned j=i+1;j<3;++j)if(steps[i]>steps[j]){s64 swap=steps[i];steps[i]=steps[j];steps[j]=swap;}
    n->period=steps[1];
    bool ok=n->period>0 && steps[0]*5>=n->period*4 && steps[2]*5<=n->period*6 && times[2]>times[0];
    u64 elapsed=times[2]-times[0];unsigned nominal=2000/n->fps;
    ok=ok && elapsed*5>=(u64)nominal*4 && elapsed*5<=(u64)nominal*6;
    fprintf(p->meta,"MATRIX_PERIOD node=%u raw=%" PRId64 " host_span_ms=%" PRIu64 " verified=%u\n",n->index,(int64_t)n->period,(uint64_t)elapsed,ok);
    return ok && fflush(events)==0 && !ferror(events);
}
static bool gain_matrix_trial(Probe *p,GainMatrixNode *n,Handle event,FILE *events,const char *node_label,unsigned trial,unsigned cycles,bool *all_held)
{
    unsigned target=gain_matrix_targets[trial];char label[80];snprintf(label,sizeof label,"%s_t%03u",node_label,target);
    fprintf(p->meta,"MATRIX_TRIAL node=%u trial=%u target=%u cycles=%u label=%s\n",n->index,trial,target,cycles,label);
    bool held=true;u64 began=osGetTime();
    for(unsigned i=0;i<4;++i)if(!write_reg(p,auto_isp_addresses[i],(u16)target,label))return false;
    for(unsigned i=0;i<4;++i){u16 v=0;if(read_reg(p,false,auto_isp_addresses[i],&v,label,-1,"rewrite_readback","matrix_rewrite")!=0 || v<1 || v>512)return false;if(v!=target)held=false;}
    u64 ended=osGetTime();s64 previous=0;
    for(unsigned cycle=0;cycle<cycles && running(p);++cycle) {
        s64 baseline=0,latest=0;u64 returned=0;
        if(!gain_matrix_wait(p,event,label,&baseline,&latest,&returned))return false;
        s64 step=latest-baseline;
        if(step*5<n->period*4 || step*5>n->period*6 || (previous && ((latest-previous)*5<n->period*4 || (latest-previous)*5>n->period*6)))return false;
        bool same=false;
        if(!gain_matrix_state(p,n,label,(int)cycle,"vsync",target,&same))return false;
        held=held && same;previous=latest;
        fprintf(events,"%u,%u,%u,%u,%u,%u,%u,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u\n",
                n->index,p->phase,n->fps,trial,target,cycle,cycles,(int64_t)baseline,(int64_t)latest,(int64_t)n->period,
                (uint64_t)returned,(uint64_t)began,(uint64_t)ended,same);
        if(fflush(events) || ferror(events))return false;
        flush_logs(p);if(!running(p))return false;
    }
    if(!running(p))return false;
    *all_held=held;
    fprintf(p->meta,"MATRIX_TRIAL_END node=%u trial=%u complete=1 held=%u image_source=none\n",n->index,trial,held);
    return true;
}
static bool gain_matrix_node(Probe *p,GainMatrixNode *n,FILE *events,FILE *plan)
{
    char label[64];snprintf(label,sizeof label,"gm_n%02u_f%02u_d%u_c%04u",n->index,n->fps,n->gate,n->coarse);
    ++p->phase;++p->samples;
    fprintf(p->meta,"MATRIX_NODE index=%u phase=%u fps=%u gate=%u coarse=%u label=%s\n",n->index,p->phase,n->fps,n->gate,n->coarse,label);
    bool ok=boot_mark(13,p->phase,0);
    if(ok && n->index==4)ok=gain_matrix_profile(p,15,"matrix_profile15");
    if(ok)ok=isp_update_prepare_arm(p,128,label);
    p->sweep_seq_target=(u16)((p->sweep_seq_saved&~15u)|n->gate);
    u16 value=0;
    if(ok)ok=write_reg(p,0xa102,p->sweep_seq_target,"matrix_gate") && read_reg(p,true,0xa102,&value,label,-1,"verified","matrix_gate")==0 && value==p->sweep_seq_target;
    for(unsigned i=0;i<5 && ok;++i){u16 v=i?528:(u16)n->coarse;ok=write_reg(p,cal_addresses[i],v,"matrix_controls") && read_reg(p,false,cal_addresses[i],&value,label,-1,"verified","matrix_control")==0 && value==v;}
    if(ok)ok=matrix_initial_isp(p,"matrix_initial") && gain_matrix_state(p,n,label,-1,"prepared",128,NULL) && gain_matrix_snapshot(p,n,label,"before_stream");
    Handle event=0;
    if(ok){Result rc=CAMU_GetVsyncInterruptEvent(&event,p->port);record_call(p,label,"GetVsyncInterruptEvent",0,event,rc);ok=rc==0 && event;}
    if(ok)ok=boot_mark(14,p->phase,0) && gain_matrix_dma(p,label,-1,true) && gain_matrix_state(p,n,label,-1,"after_initial_image",0,NULL) && save_frame(p,label) && gain_matrix_period(p,n,event,label,events) && gain_matrix_state(p,n,label,-1,"after_period",0,NULL);
    bool first_held=false;
    for(unsigned trial=0;trial<3 && ok;++trial) {
        unsigned cycles=trial?(first_held?8:2):8;bool held=false;
        fprintf(plan,"%u,%u,%u,%u,%s\n",n->index,trial,gain_matrix_targets[trial],cycles,trial?(first_held?"first_held":"first_overwritten"):"mandatory");
        ok=fflush(plan)==0 && !ferror(plan) && gain_matrix_trial(p,n,event,events,label,trial,cycles,&held);
        if(!trial)first_held=held;
    }
    if(p->streaming || !ok)stop(p,label);
    ok=ok && p->capture_ready && !p->streaming;
    if(ok)ok=gain_matrix_state(p,n,label,3,"after_stop",0,NULL) && gain_matrix_snapshot(p,n,label,"after_stop");
    if(event){Result rc=svcCloseHandle(event);record_call(p,label,"CloseVsyncEvent",0,event,rc);ok=ok && rc==0;}
    fprintf(p->meta,"MATRIX_NODE_END index=%u complete=%u first_held=%u\n",n->index,ok,first_held);
    if(ok)++p->successful_samples;
    return ok;
}
static bool gain_matrix_probe(Probe *p)
{
    if(p->calibration_part!=CAL_GAIN_MATRIX || p->sensor!=1 || !p->raw || !ram_log.exposure_sweep || p->automation_fps!=5)return false;
    fprintf(p->meta,"MATRIX_CONTRACT source=%s nodes=8 targets=128,96,160 cycles=8_or_2 calibration=0\n",GAIN_MATRIX_SOURCE);
    u16 saved[5]={0},isp[4]={0};bool saved_ok=false;
    bool ok=isp_update_baseline(p,saved,isp,&saved_ok);if(!saved_ok)return false;
    p->sweep_awb_target=(u16)((p->sweep_awb_saved|32u)&~64u);
    u16 mode=0;
    if(ok)ok=write_reg(p,0xa355,p->sweep_awb_target,"matrix_normalization") && read_reg(p,true,0xa355,&mode,"matrix_normalization",-1,"verified","matrix_normalization")==0 && (mode&96)==32;
    FILE *events=probe_fopen("matrix_events.csv","w"),*plan=probe_fopen("matrix_plan.csv","w");
    ok=ok && events && plan;
    if(events)fputs("node,phase,fps,trial,target,cycle,cycles,baseline,latest,period,returned_ms,write_begin_ms,write_end_ms,held\n",events);
    if(plan)fputs("node,trial,target,cycles,reason\n",plan);
    unsigned complete=0;
    for(unsigned i=0;i<8 && ok && running(p);++i){GainMatrixNode n={.index=i,.fps=gain_matrix_fps[i],.gate=gain_matrix_gate[i],.coarse=gain_matrix_coarse[i]};ok=gain_matrix_node(p,&n,events,plan);if(ok)++complete;}
    if(p->streaming)stop(p,"matrix_cleanup");
    bool restored=gain_matrix_profile(p,5,"matrix_profile_restore5");
    if(p->sweep_refresh_active){bool mcu=sweep_refresh_restore(p);restored=restored && mcu;}
    bool controls=cal_restore_controls(p,saved,"sweep_restore_controls"),gains=sweep_isp(p,isp,"sweep_restore_isp");
    restored=restored && controls && gains;
    if(events && fclose(events))ok=false;
    if(plan && fclose(plan))ok=false;
    bool done=ok && complete==8 && restored && running(p);
    fprintf(p->meta,"MATRIX_END complete=%u nodes=%u restored=%u\n",done,complete,restored);
    p->check_isp=false;p->isp_pin_pending=false;flush_logs(p);return done;
}
