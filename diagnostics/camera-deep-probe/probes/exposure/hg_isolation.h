#pragma once
/* Registered HG/CCM control plans. Gamma/CCM/HG table data are read-only.
 * Timing polls are bounded observations, never retries or correction writes. */
static bool hg_observe(Probe *p,const char *label,int frame,const char *when)
{
    if(!isp_update_observe(p,label,frame,when))return false;
    u16 ae_gain=0;
    return read_reg(p,true,0x221f,&ae_gain,label,frame,when,"hg_fixed_ae")==0 && ae_gain==128;
}
/* Matrix v3 validates the alias transport against a stable configured width.
 * Live CCM correspondence is reported separately and is never an atomic claim. */
static bool ccm_matrix_alias_transport(Probe *p,const char *label,const char *when)
{
    u16 before=0,hi=0,lo=0,after=0;
    bool ok=read_reg(p,true,0x2703,&before,label,-1,when,"matrix_alias_native_before")==0 &&
        read_reg(p,true,0xa703,&hi,label,-1,when,"matrix_alias_hi")==0 &&
        read_reg(p,true,0xa704,&lo,label,-1,when,"matrix_alias_lo")==0 &&
        read_reg(p,true,0x2703,&after,label,-1,when,"matrix_alias_native_after")==0;
    ok=ok && hi<=255 && lo<=255 && before==640 && after==640 && ((hi<<8)|lo)==640;
    fprintf(p->meta,"MATRIX_ALIAS_TRANSPORT phase=%u when=%s before=%u combined=%u after=%u verified=%u\n",p->phase,when,before,(hi<<8)|lo,after,ok);
    return ok;
}
static bool ccm_byte_snapshot(Probe *p,const char *label,const char *when)
{
    if(p->calibration_part!=CAL_CCM_BYTES && p->calibration_part!=CAL_CCM_BOOT && !cal_matrix_scope(p->calibration_part))return false;
    if(!boot_mark(15,p->phase,0))return false;
    bool matrix=cal_matrix_scope(p->calibration_part);
    if(matrix && !ccm_matrix_alias_transport(p,label,when))return false;
    u16 reference=0;
    if(read_reg(p,true,0x2332,&reference,label,-1,when,"ccm_native_reference")!=0)return false;
    for(unsigned i=0;i<sizeof ccm_words/sizeof *ccm_words;++i) {
        u16 address=(u16)ccm_words[i],hi=0,lo=0;
        if(!boot_mark(16,address|0x8000u,0))return false;
        Result hi_rc=read_reg(p,true,(u16)(address|0x8000u),&hi,label,-1,when,"ccm_byte");
        if(!boot_mark(17,address|0x8000u,hi_rc) || hi_rc!=0 || hi>255)return false;
        if(!boot_mark(16,(address|0x8000u)+1u,0))return false;
        Result lo_rc=read_reg(p,true,(u16)((address|0x8000u)+1u),&lo,label,-1,when,"ccm_byte");
        if(!boot_mark(17,(address|0x8000u)+1u,lo_rc) || lo_rc!=0 || lo>255)return false;
        unsigned combined=((unsigned)hi<<8)|lo;
        fprintf(p->meta,"CCM_WORD phase=%u when=%s addr=%04x msb=%u lsb=%u value=%u\n",p->phase,when,address,hi,lo,combined);
        if(i==0) {
            if(matrix) {
                u16 after=0;
                if(read_reg(p,true,0x2332,&after,label,-1,when,"ccm_native_after")!=0)return false;
                fprintf(p->meta,"CCM_ALIAS_WINDOW phase=%u when=%s addr=2332 before=%u combined=%u after=%u match_before=%u match_after=%u\n",p->phase,when,reference,combined,after,combined==reference,combined==after);
            } else {
                bool valid=combined==reference;
                fprintf(p->meta,"CCM_ALIAS_CHECK phase=%u when=%s addr=2332 native=%u combined=%u verified=%u\n",p->phase,when,reference,combined,valid);
                if(!valid)return false;
            }
        }
    }
    return boot_mark(18,p->phase,0);
}
static bool hg_snapshot(Probe *p,const char *label,const char *when)
{
    for(unsigned space=0;space<2;++space) {
        bool bytes=(p->calibration_part==CAL_CCM_BYTES || p->calibration_part==CAL_CCM_BOOT || cal_matrix_scope(p->calibration_part));
        bool ccm=p->calibration_part==CAL_CCM_NORMALIZATION || bytes;
        const unsigned *addresses=space?(bytes?ccm_bytes_mcu:ccm?ccm_mcu:hg_mcu):hg_physical;
        unsigned count=space?(bytes?sizeof ccm_bytes_mcu/sizeof *ccm_bytes_mcu:ccm?sizeof ccm_mcu/sizeof *ccm_mcu:sizeof hg_mcu/sizeof *hg_mcu):sizeof hg_physical/sizeof *hg_physical;
        for(unsigned i=0;i<count;++i) {
            u16 value=0;
            if(read_reg(p,space!=0,(u16)addresses[i],&value,label,-1,when,"hg_snapshot")!=0 ||
               (space && (addresses[i]&0x8000) && value>255))return false;
        }
    }
    return (p->calibration_part!=CAL_CCM_BYTES && p->calibration_part!=CAL_CCM_BOOT && !cal_matrix_scope(p->calibration_part)) || ccm_byte_snapshot(p,label,when);
}
static bool hg_rewrite(Probe *p,const char *label,int trial)
{
    if(p->calibration_part!=CAL_HG_ISOLATION && p->calibration_part!=CAL_CCM_NORMALIZATION && p->calibration_part!=CAL_CCM_BYTES && p->calibration_part!=CAL_CCM_BOOT && !cal_matrix_scope(p->calibration_part))return false;
    u16 target=(p->calibration_part==CAL_CCM_NORMALIZATION || p->calibration_part==CAL_CCM_BYTES || p->calibration_part==CAL_CCM_BOOT)?(u16)ccm_targets[trial]:128;
    for(unsigned i=0;i<4;++i)
        if(!write_reg(p,auto_isp_addresses[i],target,"hg_trial_rewrite"))return false;
    for(unsigned i=0;i<4;++i) {
        u16 value=0;
        if(read_reg(p,false,auto_isp_addresses[i],&value,label,trial,"rewrite_readback","hg_rewrite")!=0 ||
           value<1 || value>512)return false;
    }
    return true;
}
static bool hg_frames(Probe *p,const char *label)
{
    Handle vsync=0,dma_event=0;bool received=false;
    Result rc=CAMU_GetVsyncInterruptEvent(&vsync,p->port);
    record_call(p,label,"GetVsyncInterruptEvent",0,vsync,rc);
    bool ok=rc==0 && vsync && isp_update_arm_dma(p,label,&dma_event);
    if(ok) {
        rc=CAMU_StartCapture(p->port);record_call(p,label,"StartCapture",0,0,rc);
        p->streaming=rc==0;ok=p->streaming;
    }
    u64 started=osGetTime();
    Result waited=ok?svcWaitSynchronization(dma_event,INT64_C(2000000000)):(Result)-1;
    record_call(p,label,"WaitFrame",0,dma_event,waited);
    ok=ok && waited==0;
    if(ok) {
        rc=PROBE_FINISHED(&received,p->port);record_call(p,label,"IsFinishedReceiving",0,received,rc);
        ok=rc==0 && received;
    }
    if(dma_event) {
        rc=svcCloseHandle(dma_event);record_call(p,label,"CloseDMAEvent",0,dma_event,rc);
        ok=ok && rc==0;
    }
    fprintf(p->frames,"%u,%u,%s,0,0,%u,%08" PRIx32 ",%u,0,0,0,0,00000000,%" PRIu64 ",%" PRIu64 "\n",
            p->sensor,p->phase,label,ok,(u32)waited,received,(uint64_t)started,(uint64_t)osGetTime());
    if(ok){++p->captures;ok=hg_observe(p,label,0,"after_dma");}
    s64 previous=0;unsigned complete=0;
    for(unsigned trial=0;trial<sizeof hg_delays/sizeof *hg_delays && ok && running(p);++trial) {
        s64 baseline=0,latest=0,after=0;
        ok=no_receive_timing(p,label,trial,"baseline",&baseline);
        if(ok) {rc=svcClearEvent(vsync);record_call(p,label,"ClearVsyncEvent",0,vsync,rc);ok=rc==0;}
        if(ok) {rc=svcWaitSynchronization(vsync,INT64_C(2000000000));record_call(p,label,"WaitVsync",0,vsync,rc);ok=rc==0;}
        u64 returned=osGetTime();
        if(ok)ok=no_receive_timing(p,label,trial,"latest",&latest) && latest>baseline && latest>previous;
        fprintf(p->meta,"HG_VSYNC phase=%u trial=%u baseline=%" PRId64 " latest=%" PRId64 " returned_ms=%" PRIu64 " verified=%u\n",
                p->phase,trial,(int64_t)baseline,(int64_t)latest,(uint64_t)returned,ok);
        if(ok) {
            svcSleepThread((s64)hg_delays[trial]*1000000);
            /* libctru sleep returns void; zero records issuance only. */
            record_call(p,label,"HGPhaseDelay",0,hg_delays[trial],0);
        }
        u16 target=(p->calibration_part==CAL_CCM_NORMALIZATION || p->calibration_part==CAL_CCM_BYTES || p->calibration_part==CAL_CCM_BOOT)?(u16)ccm_targets[trial]:128;
        if(p->calibration_part==CAL_CCM_NORMALIZATION || p->calibration_part==CAL_CCM_BYTES || p->calibration_part==CAL_CCM_BOOT)
            fprintf(p->meta,"CCM_TRIAL_TARGET phase=%u trial=%u target=%u\n",p->phase,trial,target);
        u64 write_begin=osGetTime();
        if(ok)ok=hg_rewrite(p,label,(int)trial);
        u64 write_end=osGetTime();unsigned polls=0;u16 value=0;
        for(unsigned i=0;i<HG_POLL_LIMIT && ok && running(p);++i) {
            u64 begin=osGetTime();
            rc=read_reg(p,false,0x32d4,&value,label,(int)trial,"poll","hg_red");
            u64 end=osGetTime();ok=rc==0 && value>=1 && value<=512;
            fprintf(p->meta,"HG_POLL phase=%u trial=%u index=%u begin_ms=%" PRIu64 " end_ms=%" PRIu64 " value=%u verified=%u\n",
                    p->phase,trial,i,(uint64_t)begin,(uint64_t)end,value,ok);
            ++polls;
            if(!ok || value!=target)break;
            if(i+1<HG_POLL_LIMIT)svcSleepThread(HG_POLL_SLEEP_NS);
        }
        if(ok)ok=no_receive_timing(p,label,trial,"after_poll",&after) && after>=latest;
        if(ok)ok=hg_observe(p,label,(int)trial,"after_poll");
        fprintf(p->meta,"HG_TRIAL phase=%u trial=%u delay_ms=%u write_begin_ms=%" PRIu64 " write_end_ms=%" PRIu64 " polls=%u last=%u after_raw=%" PRId64 " complete=%u\n",
                p->phase,trial,hg_delays[trial],(uint64_t)write_begin,(uint64_t)write_end,polls,value,(int64_t)after,ok);
        if(ok){previous=latest;++complete;}
        flush_logs(p);
    }
    if(p->streaming)stop(p,label);
    ok=ok && complete==4 && running(p) && p->capture_ready && !p->streaming;
    if(ok)ok=hg_observe(p,label,4,"after_stop") && hg_snapshot(p,label,"after_stop");
    if(vsync) {rc=svcCloseHandle(vsync);record_call(p,label,"CloseVsyncEvent",0,vsync,rc);ok=ok && rc==0;}
    if(ok) {
        rc=GSPGPU_InvalidateDataCache(p->pixels,BYTES);record_call(p,label,"InvalidateDataCache",0,0,rc);
        ok=rc==0 && save_frame(p,label);
    }
    fprintf(p->meta,"HG_WINDOW phase=%u complete=%u trials=%u raw_provenance=first_DMA_only\n",p->phase,ok,complete);
    if(!ok)++p->failures;
    return ok;
}
static bool hg_isolation_run(Probe *p,bool ccm,bool bytes)
{
    unsigned scope=bytes?(p->calibration_part==CAL_CCM_BOOT?CAL_CCM_BOOT:CAL_CCM_BYTES):ccm?CAL_CCM_NORMALIZATION:CAL_HG_ISOLATION;
    if(p->calibration_part!=scope || p->sensor!=1 || !p->raw ||
       !ram_log.exposure_sweep || p->automation_fps!=5)return false;
    fprintf(p->meta,"HG_CONTRACT source=%s arms=3 trials=4 fps=5 observer=none retry=none\n",bytes?(p->calibration_part==CAL_CCM_BOOT?CCM_BOOT_SOURCE:CCM_BYTES_SOURCE):ccm?CCM_SOURCE:HG_SOURCE);
    const unsigned *gates=ccm?ccm_hg_arms:hg_arms;
    u16 saved[5]={0},isp[4]={0};bool saved_ok=false;
    bool ok=isp_update_baseline(p,saved,isp,&saved_ok);
    if(!saved_ok)return false;
    unsigned complete=0;
    for(unsigned arm=0;arm<sizeof hg_arms/sizeof *hg_arms && ok && running(p);++arm) {
        ++p->phase;++p->samples;
        if(!boot_mark(13,p->phase,0)){ok=false;break;}
        char label[48];
        if(ccm)snprintf(label,sizeof label,"ccm_%u_%s",arm,ccm_disable[arm]?"disabled":"normalized");
        else snprintf(label,sizeof label,"hg_%u_%s",arm,hg_arms[arm]?"on":"off");
        if(ccm) {
            p->sweep_awb_target=(u16)(((p->sweep_awb_saved|32u)&~64u)|(ccm_disable[arm]?64u:0u));
            u16 mode=0;
            ok=write_reg(p,0xa355,p->sweep_awb_target,"ccm_mode") &&
                read_reg(p,true,0xa355,&mode,label,-1,"before_refresh","ccm_mode")==0 &&
                (mode&SWEEP_AWB_CONTROL_MASK)==(p->sweep_awb_target&SWEEP_AWB_CONTROL_MASK);
            fprintf(p->meta,"CCM_MODE index=%u phase=%u disabled=%u target=%u actual=%u verified=%u\n",
                    arm,p->phase,ccm_disable[arm],p->sweep_awb_target,mode,ok);
        }
        if(ok)ok=isp_update_prepare_arm(p,128,label);
        p->sweep_seq_target=(u16)((p->sweep_seq_saved&~13u)|(gates[arm]?8u:0u));
        u16 actual=0;
        if(ok)ok=write_reg(p,0xa102,p->sweep_seq_target,"hg_gate") &&
            read_reg(p,true,0xa102,&actual,label,-1,"gate","hg_gate")==0 && actual==p->sweep_seq_target;
        fprintf(p->meta,"HG_ARM index=%u phase=%u label=%s enabled=%u target=%u gate_verified=%u\n",
                arm,p->phase,label,gates[arm],p->sweep_seq_target,ok);
        /* Finish physical settings after the final sequencer write, no Refresh. */
        for(unsigned i=0;i<5 && ok;++i) {
            u16 target=i?528:4;
            ok=write_reg(p,cal_addresses[i],target,"hg_final_controls") &&
                read_reg(p,false,cal_addresses[i],&actual,label,-1,"verified","hg_final_control")==0 && actual==target;
        }
        const u16 unity[4]={128,128,128,128};
        if(ok)ok=sweep_isp(p,unity,"hg_final_isp") && hg_observe(p,label,-1,"gated") && hg_snapshot(p,label,"before_stream");
        if(ok)ok=boot_mark(14,p->phase,0) && hg_frames(p,label);
        fprintf(p->meta,"HG_ARM_END index=%u phase=%u complete=%u\n",arm,p->phase,ok);
        if(ok){++complete;++p->successful_samples;}
        flush_logs(p);
    }
    if(p->streaming)stop(p,"hg_cleanup");
    bool restored=true;
    if(p->sweep_refresh_active)restored=sweep_refresh_restore(p);
    bool controls=cal_restore_controls(p,saved,"sweep_restore_controls");
    bool gains=sweep_isp(p,isp,"sweep_restore_isp");
    restored=restored && controls && gains;
    bool done=ok && complete==3 && restored && running(p);
    fprintf(p->meta,"HG_END complete=%u arms=%u restored=%u\n",done,complete,restored);
    p->check_isp=false;p->isp_pin_pending=false;flush_logs(p);return done;
}

static bool hg_isolation_probe(Probe *p) {return hg_isolation_run(p,false,false);}
static bool ccm_normalization_probe(Probe *p) {return hg_isolation_run(p,true,false);}

static bool ccm_bytes_probe(Probe *p) {return hg_isolation_run(p,true,true);}

static bool ccm_boot_probe(Probe *p) {return hg_isolation_run(p,true,true);}
