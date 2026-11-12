#pragma once
static bool auto_stage(Probe *p,const char *label,const char *kind,const u16 *target,unsigned mask)
{
    p->auto_luma_sum=0;
    bool ok=cal_stage(p,label,kind,target,mask,0,false,false);
    double last=p->auto_last_luma;
    if(ok)p->auto_last_luma=p->auto_luma_sum/12;
    fprintf(p->meta,"AUTO_METER phase=%u label=%s complete=%u matrix=%u linear_luma=%.10f last_linear_luma=%.10f target=0.18\n",
            p->phase,label,ok,p->auto_matrix,p->auto_last_luma,last);
    flush_logs(p);return ok;
}
static bool auto_exposure_probe(Probe *p)
{
    if(p->sensor!=1 || !cal_setup_profile(p,0))return false;
    Y2RU_StandardCoefficient matrix;
    Result rc=record_call(p,"auto_setup","GetSuitableY2rStandardCoefficient",0,0,CAMU_GetSuitableY2rStandardCoefficient(&matrix));
    if(R_FAILED(rc) || (unsigned)matrix>3)return false;
    p->auto_matrix=(unsigned)matrix;
    if(!auto_stage(p,"auto_seed","seed",NULL,0) || !cal_locked(p,"auto_lock"))return false;
    u16 saved[5],base[5];
    if(!cal_read(p,saved,"auto_seed_saved"))return false;
    for(unsigned i=0;i<4;++i)
        if(R_FAILED(read_reg(p,false,auto_isp_addresses[i],p->expected_isp+i,"auto_pin_isp",-1,"saved","isp_gain")))return false;
    p->check_isp=true;
    memcpy(base,saved,sizeof base);
    if(base[0]<AUTO_MIN_COARSE)base[0]=AUTO_MIN_COARSE;
    if(base[0]>AUTO_MAX_COARSE)base[0]=AUTO_MAX_COARSE;
    for(unsigned i=1;i<5;++i)base[i]=0x220;
    bool ok=auto_stage(p,"auto_normal_base","code_baseline",base,31);
    if(ok)cal_burst(p,0);
    const char *normal="budget_exhausted";
    u16 visited[AUTO_NORMAL_STEPS+1]={base[0]};unsigned count=1;
    for(unsigned step=0;ok && step<AUTO_NORMAL_STEPS && running(p);++step) {
        u16 next=auto_next_coarse(base[0],p->auto_last_luma);
        double error=log2(.18/fmax(p->auto_last_luma,1e-4));
        if(fabs(error)<=.125){normal="converged";break;}
        bool seen=false;for(unsigned i=0;i<count;++i)if(visited[i]==next)seen=true;
        if(!next || seen){normal="bounded_or_repeated_setting";break;}
        fprintf(p->meta,"AUTO_DECISION step=%u current=%u next=%u error_ev=%.8f gain_locked=1\n",step,base[0],next,error);
        base[0]=next;visited[count++]=next;
        char label[48];snprintf(label,sizeof label,"auto_normal_%u",step);
        ok=auto_stage(p,label,"integration",base,1);
    }
    if(ok && fabs(log2(.18/fmax(p->auto_last_luma,1e-4)))<=.125)normal="converged";
    fprintf(p->meta,"AUTO_NORMAL outcome=%s coarse=%u linear_luma=%.10f decisions=%u\n",ok?normal:"failed",base[0],p->auto_last_luma,count-1);
    /* One fixed integration for six registered gain points. This scan never
     * changes ISP settings or claims that equal composite codes mean pure RAW. */
    if(ok) {
        base[0]/=4;if(!base[0])base[0]=1;
        ok=auto_stage(p,"auto_gain_base","code_baseline",base,31);
    }
    unsigned points=0;
    for(unsigned i=0;i<6 && ok && running(p);++i) {
        u16 target[5];memcpy(target,base,sizeof target);
        for(unsigned c=1;c<5;++c)target[c]=cal_fine_codes[i];
        char label[48];snprintf(label,sizeof label,"auto_gain_%04x",cal_fine_codes[i]);
        ok=auto_stage(p,label,"code",target,30);
        if(ok)++points;
    }
    unsigned digital_points=0;
    for(unsigned multiplier=2;multiplier<=4 && ok && running(p);multiplier*=2) {
        u16 target[5];memcpy(target,base,sizeof target);
        for(unsigned c=1;c<5;++c)target[c]=(u16)((multiplier<<9)|0x20);
        char label[48];snprintf(label,sizeof label,"auto_digital_%u",multiplier);
        ok=auto_stage(p,label,"digital",target,30);if(ok)++digital_points;
    }
    /* No failure recovery continuation: restore and stop. Original five
     * controls are always attempted; only a clean run samples restoration. */
    bool restored=cal_restore_controls(p,saved,"auto_seed_restore");
    if(ok && restored && running(p))restored=auto_stage(p,"auto_restore","restore",saved,30);
    fprintf(p->meta,"AUTO_END source=exposure-auto complete=%u stages=%u max_stages=%u gain_points=%u digital_points=%u seed_restored=%u normal=%s\n",
            ok&&restored&&points==6&&digital_points==2,p->successful_samples,AUTO_MAX_STAGES,points,digital_points,restored,normal);
    flush_logs(p);p->check_isp=false;
    return ok && restored && points==6 && digital_points==2 && p->successful_samples<=AUTO_MAX_STAGES && running(p);
}
