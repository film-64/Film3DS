#pragma once
typedef struct {unsigned phase,coarse;char label[40];double luma,integration,dose,prediction;bool measured;} FieldImage;
typedef struct {
    FieldImage images[3];FieldModel prior;
    char directory[160];unsigned matrix;
    bool complete,converged,reference_saved;
    double drift_ev,reference_seconds;
} FieldRun;
static double field_dose(const Probe *p,unsigned coarse,double *integration)
{
    double lines=p->field_state[3],line=p->field_state[4];
    if(!lines || !line || !p->field_state[7] || !p->expected_isp[1] || p->field_period<=0)return NAN;
    *integration=(coarse+p->field_state[6]/line)*p->field_period/lines;
    /* Green normalization is a TESTABLE gain-response hypothesis, not an ISO
     * identity. Native analog/WB and all ISP channels remain in the raw proof. */
    return *integration*(p->field_state[7]/8.0)*(p->expected_isp[1]/128.0);
}
static void field_describe(Probe *p,FieldRun *run,unsigned slot,const char *label,unsigned coarse)
{
    FieldImage *image=run->images+slot;
    *image=(FieldImage){.phase=p->phase,.coarse=coarse,.luma=p->auto_last_luma,.measured=true};
    snprintf(image->label,sizeof image->label,"%s",label);
    image->dose=field_dose(p,coarse,&image->integration);
    image->prediction=field_predict(&run->prior,image->dose);
    fprintf(p->meta,"FIELD_IMAGE slot=%u phase=%u label=%s coarse=%u integration_est_s=%.10f dose=%.10f predicted_A_s=%.10f prior_labels=%u extrapolated=%u linear_luma=%.10f\n",
        slot,p->phase,label,coarse,image->integration,image->dose,image->prediction,run->prior.count,
        !field_in_domain(&run->prior,image->dose),image->luma);
}
static bool field_capture(Probe *p,FieldRun *run)
{
    if(p->calibration_part!=CAL_EXPOSURE_FIELD || p->sensor!=1 || p->automation_fps!=5 || !p->raw || ram_log.active ||
       !p->map_known || !p->capture_ready || !cal_open(p))return false;
    fprintf(p->meta,"FIELD_CONTRACT source=exposure-field version=1 max_stages=8 fps=5 coarse_min=1 coarse_max=1585 settle_gain=8 settle_coarse=2 measured=4 references=post_prediction retry=none\n");
    if(!cal_setup_profile(p,5))return false;
    Y2RU_StandardCoefficient matrix;
    if(R_FAILED(record_call(p,"field_setup","GetSuitableY2rStandardCoefficient",0,0,CAMU_GetSuitableY2rStandardCoefficient(&matrix))) || (unsigned)matrix>3)return false;
    p->auto_matrix=run->matrix=(unsigned)matrix;
    if(!auto_stage(p,"field_seed","seed",NULL,0) || !cal_locked(p,"field_lock"))return false;
    u16 saved[5],base[5];
    if(!cal_read(p,saved,"field_seed_saved"))return false;
    memcpy(base,saved,sizeof base);
    if(base[0]<1)base[0]=1;
    if(base[0]>1585)base[0]=1585;
    for(unsigned i=1;i<5;++i) {
        if(saved[i]&0xf000)return false;
        base[i]=(u16)((saved[i]&0x1ff)|0x200);
    }
    static const u16 addresses[]={0x0010,0x0012,0x0014,0x300a,0x300c,0x3016,0x3014,0x302a,0x302c,0x302e,0x3030};
    bool ok=true;
    for(unsigned i=0;i<11;++i)
        if(R_FAILED(read_reg(p,false,addresses[i],p->field_state+i,"field_state",-1,"saved","field_state")))ok=false;
    if(p->field_state[3]!=2114 || !p->field_state[4] || !p->field_state[7])ok=false;
    p->field_check_state=ok;p->check_isp=false;p->isp_pin_pending=ok;
    if(ok)ok=auto_stage(p,"field_normal_base","code_baseline",base,31);
    if(ok) {
        cal_burst(p,5);
        if(p->field_period<=0 || !isfinite(p->field_period))ok=false;
    }
    unsigned visited[4]={base[0]},count=1;
    char normal_label[40]="field_normal_base";
    for(unsigned step=0;ok && step<3 && running(p);++step) {
        if(fabs(log2(.18/fmax(p->auto_last_luma,1e-4)))<=.125)break;
        unsigned next=auto_next_limited(base[0],p->auto_last_luma,1585);bool seen=false;
        for(unsigned i=0;i<count;++i)if(visited[i]==next)seen=true;
        if(!next || seen)break;
        base[0]=(u16)next;visited[count++]=next;
        snprintf(normal_label,sizeof normal_label,"field_normal_%u",step);
        ok=auto_stage(p,normal_label,"integration",base,1);
    }
    if(ok) {
        run->converged=fabs(log2(.18/fmax(p->auto_last_luma,1e-4)))<=.125;
        field_describe(p,run,1,normal_label,base[0]);
    }
    u16 normal_coarse=base[0];double normal_luma=p->auto_last_luma;
    for(unsigned slot=0;slot<3 && ok && running(p);slot+=2) {
        unsigned coarse=slot?normal_coarse*2:normal_coarse/2;
        if(coarse<1)coarse=1;
        if(coarse>1585)coarse=1585;
        if(coarse==normal_coarse) {
            fprintf(p->meta,"FIELD_BOUND slot=%u status=not_measured_clamped coarse=%u\n",slot,coarse);continue;
        }
        base[0]=(u16)coarse;
        const char *label=slot?"field_upper":"field_lower";
        ok=auto_stage(p,label,"integration",base,1);
        if(ok)field_describe(p,run,slot,label,coarse);
    }
    if(ok && running(p)) {
        base[0]=normal_coarse;
        ok=auto_stage(p,"field_normal_check","integration",base,1);
        run->drift_ev=log2(fmax(p->auto_last_luma,1e-8)/fmax(normal_luma,1e-8));
        if(fabs(run->drift_ev)>.125)ok=false;
    }
    p->field_check_state=false;p->check_isp=false;p->isp_pin_pending=false;
    bool restored=cal_restore_controls(p,saved,"field_restore");
    run->complete=ok && restored && p->successful_samples<=FIELD_MAX_STAGES && running(p);
    fprintf(p->meta,"FIELD_END complete=%u converged=%u drift_ev=%.8f stages=%u max_stages=8 restored=%u\n",
        run->complete,run->converged,run->drift_ev,p->successful_samples,restored);
    flush_logs(p);return run->complete;
}
