#pragma once
/* The generated sweep_contract.h is the source registration and write budget. */
static bool sweep_isp(Probe *p,const u16 *target,const char *label)
{
    if(!cal_sweep(p->calibration_part))return false;
    for(unsigned i=0;i<4;++i)if(target[i]<1 || target[i]>512)return false;
    bool ok=true;
    for(unsigned i=0;i<4;++i) {
        u16 actual=0;
        if(!write_reg(p,auto_isp_addresses[i],target[i],label))ok=false;
        if(R_FAILED(read_reg(p,false,auto_isp_addresses[i],&actual,label,-1,"verified","sweep_isp")) || actual!=target[i])ok=false;
    }
    /* Expected state comes from the plan, never from accepting drift. */
    memcpy(p->expected_isp,target,sizeof p->expected_isp);
    p->check_isp=true;return ok;
}
static bool sweep_sample(Probe *p,const char *label,const u16 *target)
{
    return cal_stage(p,label,"code_baseline",target,31,0,false,false);
}
static bool sweep_awb_restore(Probe *p)
{
    if(!p->sweep_awb_force)return true;
    u16 value=0;
    bool wrote=write_reg(p,0xa355,p->sweep_awb_saved,"sweep_awb_restore");
    Result rc=read_reg(p,true,0xa355,&value,"sweep_awb_restore",-1,"verified","awb_mode");
    u16 mask=cal_sweep_refresh(p->calibration_part)?SWEEP_AWB_CONTROL_MASK:65535;
    bool ok=wrote && R_SUCCEEDED(rc) && (value&mask)==(p->sweep_awb_saved&mask);
    fprintf(p->meta,"AWB_RESTORE saved=%u actual=%u verified=%u\n",p->sweep_awb_saved,value,ok);
    p->sweep_awb_force=false;return ok;
}
static bool sweep_seq_restore(Probe *p)
{
    if(!p->sweep_seq_gate)return true;
    u16 value=0;
    bool wrote=write_reg(p,0xa102,p->sweep_seq_saved,"sweep_seq_restore");
    Result rc=read_reg(p,true,0xa102,&value,"sweep_seq_restore",-1,"verified","seq_mode_live");
    bool ok=wrote && R_SUCCEEDED(rc) && value==p->sweep_seq_saved;
    fprintf(p->meta,"SEQ_RESTORE saved=%u actual=%u verified=%u\n",p->sweep_seq_saved,value,ok);
    p->sweep_seq_gate=false;return ok;
}
/* Bounded command completion, not a write retry or another capture attempt. */
static bool sweep_refresh(Probe *p,bool restoring)
{
    const char *label=restoring?"sweep_refresh_restore":"sweep_refresh_apply";
    u16 command=0,state=0;
    if(R_FAILED(read_reg(p,true,0xa103,&command,label,-1,"precondition","seq_cmd")) || command ||
       R_FAILED(read_reg(p,true,0xa104,&state,label,-1,"precondition","seq_state")) ||
       (state!=3 && state!=7))return false;
    if(!write_reg(p,0xa103,5,label))return false;
    unsigned stable=0;
    for(unsigned i=0;i<SWEEP_REFRESH_POLLS;++i) {
        if(!restoring && !running(p))return false;
        u16 current=0,seq=0,awb=0,preview=0;
        if(R_FAILED(read_reg(p,true,0xa103,&command,label,(int)i,"poll","seq_cmd")) ||
           R_FAILED(read_reg(p,true,0xa104,&current,label,(int)i,"poll","seq_state")) ||
           R_FAILED(read_reg(p,true,0xa102,&seq,label,(int)i,"poll","seq_mode_live")) ||
           R_FAILED(read_reg(p,true,0xa355,&awb,label,(int)i,"poll","awb_mode")) ||
           R_FAILED(read_reg(p,true,0xa11f,&preview,label,(int)i,"poll","seq_preview_awb")))return false;
        u16 expected_seq=restoring?p->sweep_seq_saved:p->sweep_seq_target;
        u16 expected_awb=restoring?p->sweep_awb_saved:p->sweep_awb_target;
        bool matched=!command && current==state && seq==expected_seq &&
            (awb&SWEEP_AWB_CONTROL_MASK)==(expected_awb&SWEEP_AWB_CONTROL_MASK) && preview==(restoring?p->sweep_preview_awb_saved:1);
        stable=matched?stable+1:0;
        if(stable>=SWEEP_REFRESH_STABLE) {
            fprintf(p->meta,"SEQ_REFRESH restoring=%u command=5 writes=%u polls=%u state=%u verified=1\n",
                    restoring,p->sweep_refresh_writes,i+1,state);
            return true;
        }
        svcSleepThread(SWEEP_REFRESH_SLEEP_NS);
    }
    fprintf(p->meta,"SEQ_REFRESH restoring=%u command=5 writes=%u polls=%u verified=0\n",
            restoring,p->sweep_refresh_writes,SWEEP_REFRESH_POLLS);
    return false;
}
static bool sweep_refresh_restore(Probe *p)
{
    p->sweep_manual_locked=false; /* Sampling has ended; restoration may refresh. */
    u16 value=0;
    bool wrote=write_reg(p,0xa11f,p->sweep_preview_awb_saved,"sweep_preview_awb_restore");
    Result rc=read_reg(p,true,0xa11f,&value,"sweep_preview_awb_restore",-1,"verified","seq_preview_awb");
    bool preview=wrote && R_SUCCEEDED(rc) && value==p->sweep_preview_awb_saved;
    fprintf(p->meta,"PREVIEW_AWB_RESTORE saved=%u actual=%u verified=%u\n",p->sweep_preview_awb_saved,value,preview);
    bool awb=sweep_awb_restore(p);
    bool seq=sweep_seq_restore(p);
    bool refresh=sweep_refresh(p,true);
    p->sweep_refresh_active=false;
    return preview && awb && seq && refresh;
}
static bool sweep_post_refresh_manual(Probe *p)
{
    u16 gains[3]={0},seq=0,preview=0,command=0,state=0;
    const u16 addresses[3]={0xa34e,0xa34f,0xa350};
    for(unsigned i=0;i<3;++i)
        if(R_FAILED(read_reg(p,true,addresses[i],gains+i,"sweep_manual_gate",-1,"before_disable","awb_gain")) || gains[i]!=128)return false;
    if(!write_reg(p,0xa11f,0,"sweep_manual_preview_off") ||
       R_FAILED(read_reg(p,true,0xa11f,&preview,"sweep_manual_gate",-1,"disabled","seq_preview_awb")) || preview)return false;
    p->sweep_seq_target=(u16)(p->sweep_seq_saved&~5u);
    if(!write_reg(p,0xa102,p->sweep_seq_target,"sweep_manual_drivers_off") ||
       R_FAILED(read_reg(p,true,0xa102,&seq,"sweep_manual_gate",-1,"disabled","seq_mode_live")) || seq!=p->sweep_seq_target || (seq&5))return false;
    if(R_FAILED(read_reg(p,true,0xa103,&command,"sweep_manual_gate",-1,"disabled","seq_cmd")) || command ||
       R_FAILED(read_reg(p,true,0xa104,&state,"sweep_manual_gate",-1,"disabled","seq_state")) || (state!=3 && state!=7))return false;
    for(unsigned i=0;i<3;++i)
        if(R_FAILED(read_reg(p,true,addresses[i],gains+i,"sweep_manual_gate",-1,"disabled","awb_gain")) || gains[i]!=128)return false;
    p->sweep_manual_locked=true;
    fprintf(p->meta,"POST_REFRESH_MANUAL seq=%u preview_awb=0 awb_r=%u awb_g=%u awb_b=%u command=%u state=%u verified=1\n",
            seq,gains[0],gains[1],gains[2],command,state);
    return true;
}
static bool sweep_probe(Probe *p)
{
    if(!cal_sweep(p->calibration_part) || p->sensor!=1 || !p->raw || !ram_log.exposure_sweep || p->automation_fps!=5)return false;
    bool diagnostic=cal_sweep_diagnostic(p->calibration_part);
    bool refresh=cal_sweep_refresh(p->calibration_part);
    bool manual=p->calibration_part==CAL_EXPOSURE_SWEEP_MANUAL;
    unsigned expected=diagnostic?5:SWEEP_STAGES;
    fprintf(p->meta,"SWEEP_CONTRACT source=%s states=%u points=%u stages=%u sensor=1 fps=5 settle=8 measured=4 retry=none ipc_dump=0 raw_x=256 raw_y=192 raw_w=128 raw_h=96\n",manual?SWEEP_MANUAL_SOURCE:refresh?SWEEP_REFRESH_SOURCE:diagnostic?SWEEP_CHECK_SOURCE:"exposure-sweep/v2",diagnostic?1:SWEEP_STATES,diagnostic?1:SWEEP_POINTS,expected);
    if(!cal_setup_profile(p,5))return false;
    Y2RU_StandardCoefficient matrix;
    if(R_FAILED(CAMU_GetSuitableY2rStandardCoefficient(&matrix)) || (unsigned)matrix>3)return false;
    p->auto_matrix=(unsigned)matrix;
    fprintf(p->meta,"SWEEP_MATRIX value=%u\n",p->auto_matrix);
    if(!cal_stage(p,"sweep_seed","seed",NULL,0,0,false,false) || !cal_locked(p,"sweep_lock"))return false;
    u16 saved[5],native[5],isp[4],unity[4]={128,128,128,128};
    if(!cal_read(p,saved,"sweep_original"))return false;
    memcpy(native,saved,sizeof native);
    if(native[0]<4)native[0]=4;
    if(native[0]>1585)native[0]=1585;
    for(unsigned i=1;i<5;++i) {
        if(saved[i]&0xf000)return false;
        native[i]=(u16)((saved[i]&0x1ff)|0x200);
    }
    /* Allow settling once for the original ISP state before establishing it. */
    p->isp_pin_pending=true;
    bool ok=sweep_sample(p,"sweep_ref_00",native);
    memcpy(isp,p->expected_isp,sizeof isp);
    for(unsigned i=0;i<4;++i)if(isp[i]<1 || isp[i]>512)ok=false;
    if(ok)cal_burst(p,5);
    if(ok && diagnostic) {
        u16 value=0;
        Result rc=read_reg(p,true,0xa102,&value,"sweep_seq_save",-1,"saved","seq_mode_live");
        ok=R_SUCCEEDED(rc) && value<=255;
        if(ok) {
            p->sweep_seq_saved=value;p->sweep_seq_target=(u16)(value&~5u);p->sweep_seq_gate=true;
            if(refresh)p->sweep_seq_target|=4;
            bool wrote=write_reg(p,0xa102,p->sweep_seq_target,"sweep_seq_disable");
            rc=read_reg(p,true,0xa102,&value,"sweep_seq_disable",-1,"verified","seq_mode_live");
            ok=wrote && R_SUCCEEDED(rc) && value==p->sweep_seq_target;
            fprintf(p->meta,"SEQ_GATE saved=%u target=%u verified=%u\n",p->sweep_seq_saved,p->sweep_seq_target,ok);
        }
    }
    bool awb_restored=true;
    if(ok && diagnostic) {
        u16 value=0;
        Result rc=read_reg(p,true,0xa355,&value,"sweep_awb_save",-1,"saved","awb_mode");
        ok=R_SUCCEEDED(rc) && value<=255;
        if(ok) {
            p->sweep_awb_saved=value;p->sweep_awb_target=(u16)(value|32u);p->sweep_awb_force=true;
            bool wrote=write_reg(p,0xa355,p->sweep_awb_target,"sweep_awb_force");
            rc=read_reg(p,true,0xa355,&value,"sweep_awb_force",-1,"verified","awb_mode");
            u16 mask=refresh?SWEEP_AWB_CONTROL_MASK:65535;
            ok=wrote && R_SUCCEEDED(rc) && (value&mask)==(p->sweep_awb_target&mask);
            fprintf(p->meta,"AWB_FORCE saved=%u target=%u verified=%u\n",p->sweep_awb_saved,p->sweep_awb_target,ok);
        }
    }
    if(ok && refresh) {
        u16 value=0;
        Result rc=read_reg(p,true,0xa11f,&value,"sweep_preview_awb_save",-1,"saved","seq_preview_awb");
        ok=R_SUCCEEDED(rc) && value==0;
        if(ok) {
            p->sweep_preview_awb_saved=value;p->sweep_refresh_active=true;
            bool wrote=write_reg(p,0xa11f,1,"sweep_preview_awb_enable");
            rc=read_reg(p,true,0xa11f,&value,"sweep_preview_awb_enable",-1,"verified","seq_preview_awb");
            ok=wrote && R_SUCCEEDED(rc) && value==1;
        }
    }
    if(ok && refresh)ok=sweep_refresh(p,false);
    if(ok && manual)ok=sweep_post_refresh_manual(p);
    for(unsigned s=diagnostic?13:0;s<(diagnostic?14:SWEEP_STATES) && ok && running(p);++s) {
        u16 target[5];memcpy(target,native,sizeof target);
        if(s)for(unsigned ch=1;ch<5;++ch)target[ch]=(u16)sweep_codes[s];
        if(!manual)ok=sweep_isp(p,sweep_unity[s]?unity:isp,"sweep_state");
        for(unsigned j=0;j<(diagnostic?1:SWEEP_POINTS) && ok && running(p);++j) {
            target[0]=(u16)sweep_coarse[j];char label[48];
            snprintf(label,sizeof label,"sweep_s%02u_c%04u",s,target[0]);
            printf("\x1b[8;0HSWEEP state %u/%u point %u/%u C=%u\n",s+1,SWEEP_STATES,j+1,SWEEP_POINTS,target[0]);
            fprintf(p->meta,"SWEEP_POINT state=%u point=%u coarse=%u isp=%s\n",s,j,target[0],sweep_unity[s]?"unity":"native");
            ok=sweep_sample(p,label,target);
        }
        if(ok && running(p)) {
            char label[48];snprintf(label,sizeof label,"sweep_ref_%02u",s+1);
            awb_restored=refresh?sweep_refresh_restore(p):sweep_awb_restore(p);
            ok=awb_restored && sweep_isp(p,isp,"sweep_reference") && sweep_sample(p,label,native);
        }
    }
    /* Always attempt original controls, even after capture failure or abort. */
    if(p->sweep_refresh_active)awb_restored=sweep_refresh_restore(p);
    else if(p->sweep_awb_force)awb_restored=sweep_awb_restore(p);
    bool restored=cal_restore_controls(p,saved,"sweep_restore_controls");
    bool isp_restored=sweep_isp(p,isp,"sweep_restore_isp");
    restored=restored && isp_restored && awb_restored;
    if(ok && restored && running(p))ok=sweep_sample(p,"sweep_restore",saved);
    bool seq_restored=sweep_seq_restore(p);restored=restored && seq_restored;
    bool complete=ok && restored && running(p) && p->successful_samples==expected;
    fprintf(p->meta,"SWEEP_END complete=%u stages=%u restored=%u\n",complete,p->successful_samples,restored);
    p->check_isp=false;p->isp_pin_pending=false;flush_logs(p);return complete;
}
