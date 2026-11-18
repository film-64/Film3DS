#pragma once
/* Registered observational experiment. IFP changes are data, not calibration.
 * Every fixed control, DMA completion, write readback and restoration still
 * fails closed. A rewrite is one planned intervention, never a capture retry.
 */
static bool isp_update_observe(Probe *p,const char *label,int frame,const char *when)
{
    for(unsigned i=0;i<sizeof isp_update_registers/sizeof *isp_update_registers;++i) {
        u16 address=(u16)isp_update_registers[i],value=0;
        bool mcu=(address&0x8000) || address==0x221f;
        Result rc=read_reg(p,mcu,address,&value,label,frame,when,"isp_update_state");
        bool valid=R_SUCCEEDED(rc);
        if(address==0xa102)valid=valid && value==p->sweep_seq_target && !(value&5);
        if(address==0xa103)valid=valid && !value;
        if(address==0xa104)valid=valid && value==3;
        if(address==0xa355)valid=valid && (value&SWEEP_AWB_CONTROL_MASK)==(p->sweep_awb_target&SWEEP_AWB_CONTROL_MASK);
        if(address==0xa34e || address==0xa34f || address==0xa350)valid=valid && value==128;
        if(address==0x3012)valid=valid && value==4;
        if(address==0x3056 || address==0x3058 || address==0x305a || address==0x305c)valid=valid && value==528;
        if(address>=0x32d4 && address<=0x32da)valid=valid && value>=1 && value<=512;
        if(!valid) {
            fprintf(p->meta,"ISP_UPDATE_REJECT phase=%u frame=%d when=%s addr=%04x value=%u result=%08" PRIx32 "\n",
                    p->phase,frame,when,address,value,(u32)rc);
            p->control_drift=true;return false;
        }
    }
    return true;
}
static bool isp_update_mode_snapshot(Probe *p,const char *label,int frame,const char *when)
{
    for(unsigned i=0;i<sizeof isp_update_mode/sizeof *isp_update_mode;++i) {
        u16 value=0;
        if(R_FAILED(read_reg(p,true,(u16)isp_update_mode[i],&value,label,frame,when,"isp_update_mode")))return false;
    }
    return true;
}
static bool isp_update_arm_dma(Probe *p,const char *label,Handle *event)
{
    Result rc=CAMU_SetReceiving(event,p->pixels,p->port,BYTES,(s16)p->transfer);
    record_call(p,label,"SetReceiving",0,BYTES,rc);
    return R_SUCCEEDED(rc);
}
static bool isp_update_frames(Probe *p,const char *label,bool continuous,bool include_mode)
{
    Handle event=0;bool ok=true;
    for(int frame=0;frame<3 && ok && running(p);++frame) {
        char call_label[80];snprintf(call_label,sizeof call_label,"%s_f%d",label,frame);
        if(!event) {
            ok=isp_update_observe(p,label,frame,"before_arm");
            if(ok)ok=isp_update_arm_dma(p,call_label,&event);
            if(ok)ok=isp_update_observe(p,label,frame,"after_arm");
        }
        if(ok && !p->streaming) {
            Result rc=CAMU_StartCapture(p->port);
            record_call(p,call_label,"StartCapture",0,0,rc);
            p->streaming=R_SUCCEEDED(rc);ok=p->streaming;
            if(ok)ok=isp_update_observe(p,label,frame,"after_start");
        }
        u64 start=osGetTime();
        Result wait=ok?svcWaitSynchronization(event,INT64_C(2000000000)):(Result)-1;
        record_call(p,call_label,"WaitFrame",0,event,wait);
        bool received=false;Result rc=wait;
        if(ok && wait==0) {
            rc=PROBE_FINISHED(&received,p->port);
            record_call(p,call_label,"IsFinishedReceiving",0,received,rc);
        }
        ok=ok && wait==0 && R_SUCCEEDED(rc) && received;
        if(event){svcCloseHandle(event);event=0;}
        /* One outstanding next DMA, explicitly registered. No image processing
         * or reuse of a failed frame; arm before slow reads and the rewrite.
         * Frame1 may overlap the rewrite; frame2 is armed after it completes.
         */
        if(ok && continuous && frame<2) {
            char next[80];snprintf(next,sizeof next,"%s_f%d",label,frame+1);
            ok=isp_update_arm_dma(p,next,&event);
        }
        if(ok)ok=isp_update_observe(p,label,frame,"after_wait");
        if(ok && include_mode && (frame==0 || frame==2))ok=isp_update_mode_snapshot(p,label,frame,frame==0?"after_frame0":"after_frame2");
        if(!continuous || frame==2 || !ok) {
            stop(p,call_label);
            ok=ok && p->capture_ready && !p->streaming;
            if(ok)ok=isp_update_observe(p,label,frame,"after_stop");
        }
        fprintf(p->frames,"%u,%u,%s,%d,0,%u,%08" PRIx32 ",%u,0,0,0,0,00000000,%" PRIu64 ",%" PRIu64 "\n",
                p->sensor,p->phase,label,frame,ok,(u32)wait,received,(uint64_t)start,(uint64_t)osGetTime());
        fprintf(p->meta,"ISP_UPDATE_FRAME phase=%u frame=%d ok=%u wait=%08" PRIx32 " received=%u\n",
                p->phase,frame,ok,(u32)wait,received);
        if(ok)++p->captures;
        if(ok && frame==0) {
            const u16 unity[4]={128,128,128,128};
            ok=isp_update_observe(p,label,frame,"before_rewrite") &&
                sweep_isp(p,unity,"isp_update_rewrite") &&
                isp_update_observe(p,label,frame,"after_rewrite");
        }
    }
    if(p->streaming)stop(p,label);
    if(event)svcCloseHandle(event);
    if(!ok || !running(p) || !p->capture_ready){++p->failures;return false;}
    Result invalidated=GSPGPU_InvalidateDataCache(p->pixels,BYTES);
    record_call(p,label,"InvalidateDataCache",0,0,invalidated);
    return R_SUCCEEDED(invalidated) && save_frame(p,label);
}
static bool isp_update_baseline(Probe *p,u16 saved[5],u16 isp[4],bool *saved_ok)
{
    *saved_ok=false;
    if(!cal_setup_profile(p,5))return false;
    Y2RU_StandardCoefficient matrix;
    if(R_FAILED(CAMU_GetSuitableY2rStandardCoefficient(&matrix)) || (unsigned)matrix>3)return false;
    p->auto_matrix=(unsigned)matrix;
    if(!cal_stage(p,"sweep_seed","seed",NULL,0,0,false,false) || !cal_locked(p,"sweep_lock"))return false;
    u16 native[5];
    if(!cal_read(p,saved,"sweep_original"))return false;
    *saved_ok=true;
    memcpy(native,saved,sizeof native);
    if(native[0]<4)native[0]=4;
    if(native[0]>1585)native[0]=1585;
    for(unsigned i=1;i<5;++i) {
        if(saved[i]&0xf000)return false;
        native[i]=(u16)((saved[i]&0x1ff)|0x200);
    }
    p->isp_pin_pending=true;
    bool ok=sweep_sample(p,"sweep_ref_00",native);
    memcpy(isp,p->expected_isp,sizeof p->expected_isp);
    for(unsigned i=0;i<4;++i)if(isp[i]<1 || isp[i]>512)ok=false;
    if(ok)cal_burst(p,5);
    if(ok)ok=R_SUCCEEDED(read_reg(p,true,0xa102,&p->sweep_seq_saved,"isp_update_save",-1,"saved","seq_mode")) &&
        R_SUCCEEDED(read_reg(p,true,0xa355,&p->sweep_awb_saved,"isp_update_save",-1,"saved","awb_mode")) &&
        R_SUCCEEDED(read_reg(p,true,0xa11f,&p->sweep_preview_awb_saved,"isp_update_save",-1,"saved","preview_awb")) &&
        p->sweep_seq_saved<=255 && p->sweep_awb_saved<=255 && !p->sweep_preview_awb_saved;
    if(ok) {
        p->sweep_seq_gate=true;p->sweep_awb_force=true;p->sweep_refresh_active=true;
        p->sweep_awb_target=(u16)(p->sweep_awb_saved|32u);
        ok=write_reg(p,0xa355,p->sweep_awb_target,"isp_update_force");
    }
    return ok;
}
/* Only the registered matrix subject gains are observational at setup.
 * Fixed exposure/control checks and original-value restoration remain strict. */
static bool matrix_initial_isp(Probe *p,const char *label)
{
    if(!cal_matrix_scope(p->calibration_part))return false;
    for(unsigned i=0;i<4;++i)if(!write_reg(p,auto_isp_addresses[i],128,label))return false;
    for(unsigned i=0;i<4;++i){u16 v=0;if(read_reg(p,false,auto_isp_addresses[i],&v,label,-1,"initial_observation","matrix_initial_gain")!=0 || v<1 || v>512)return false;}
    return true;
}
static bool isp_update_prepare_arm(Probe *p,unsigned initial,const char *label)
{
    p->sweep_manual_locked=false;
    p->sweep_seq_target=(u16)((p->sweep_seq_saved&~5u)|4u);
    bool ok=!p->streaming && p->capture_ready && write_reg(p,0xa102,p->sweep_seq_target,"isp_update_awb_on") &&
        write_reg(p,0xa11f,1,"isp_update_preview_on") && sweep_refresh(p,false) && sweep_post_refresh_manual(p);
    for(unsigned i=0;i<5 && ok;++i) {
        u16 actual=0,target=i?528:4;
        ok=write_reg(p,cal_addresses[i],target,"isp_update_controls") &&
            R_SUCCEEDED(read_reg(p,false,cal_addresses[i],&actual,label,-1,"verified","isp_update_control")) && actual==target;
    }
    u16 target[4]={(u16)initial,(u16)initial,(u16)initial,(u16)initial};
    return ok && (cal_matrix_scope(p->calibration_part)?matrix_initial_isp(p,"isp_update_initial"):sweep_isp(p,target,"isp_update_initial")) && isp_update_observe(p,label,-1,"initial");
}
static bool isp_update_probe(Probe *p)
{
    if(p->calibration_part!=CAL_EXPOSURE_ISP_UPDATE || p->sensor!=1 || !p->raw ||
       !ram_log.exposure_sweep || p->automation_fps!=5)return false;
    fprintf(p->meta,"ISP_UPDATE_CONTRACT source=%s arms=4 frames=3 sensor=1 fps=5 retry=none context_writes=0 mcu_ram_writes=0\n",ISP_UPDATE_SOURCE);
    u16 saved[5]={0},isp[4]={0};bool saved_ok=false;
    bool ok=isp_update_baseline(p,saved,isp,&saved_ok);
    if(!saved_ok)return false;
    unsigned complete=0;
    for(unsigned arm=0;arm<sizeof isp_update_initial/sizeof *isp_update_initial && ok && running(p);++arm) {
        ++p->phase;++p->samples;p->sweep_manual_locked=false;
        unsigned initial=isp_update_initial[arm];bool continuous=isp_update_continuous[arm]!=0;
        char label[64];snprintf(label,sizeof label,"isp_update_%s_%03u",continuous?"continuous":"restart",initial);
        fprintf(p->meta,"ISP_UPDATE_ARM index=%u phase=%u label=%s initial=%u continuous=%u\n",arm,p->phase,label,initial,continuous);
        ok=isp_update_prepare_arm(p,initial,label) && isp_update_mode_snapshot(p,label,-1,"before_initial");
        if(ok)ok=isp_update_frames(p,label,continuous,true);
        fprintf(p->meta,"ISP_UPDATE_ARM_END index=%u phase=%u complete=%u\n",arm,p->phase,ok);
        if(ok){++complete;++p->successful_samples;}
        flush_logs(p);
    }
    if(p->streaming)stop(p,"isp_update_cleanup");
    bool restored=true;
    if(p->sweep_refresh_active)restored=sweep_refresh_restore(p);
    bool controls=cal_restore_controls(p,saved,"sweep_restore_controls");
    bool gains=sweep_isp(p,isp,"sweep_restore_isp");
    restored=restored && controls && gains;
    bool done=ok && complete==4 && restored && running(p);
    fprintf(p->meta,"ISP_UPDATE_END complete=%u arms=%u restored=%u\n",done,complete,restored);
    p->check_isp=false;p->isp_pin_pending=false;flush_logs(p);return done;
}
