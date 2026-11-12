/* Execute the actual production trial/cleanup functions against injected failures. */
#define main filmprobe_app_main
#include "../probes/exposure/main.c"
#undef main
#include <assert.h>

static u16 values[65536];
static unsigned writes, receives, closed_events, stops;
static unsigned fail_write_at;
static unsigned drift_at_receive,rate_sets;
static bool clear_write_bit8;
static bool reject_ffff_read;
static bool cancel_after_write, timeout_capture, fail_restore, deny_read, fail_stop;
static bool ae, awb, fail_second_receive, positive_timeout, stay_busy;
static bool auto_response;
static unsigned isp_drift_at;
static unsigned isp_settle_at;
static u8 *dma;
void hidScanInput(void) {}
u32 hidKeysHeld(void) { return cancel_after_write && writes ? KEY_B : 0; }
bool aptMainLoop(void) { return true; }
u64 osGetTime(void) { static u64 now; return ++now; }
Result CAMU_ReadRegisterI2cExclusive(u16 *v,u32 s,u16 a)
{ if (deny_read) return (Result)0xd900182f; *v=values[a]; return reject_ffff_read && *v==0xffff?(Result)0xc9602fef:0; }
Result CAMU_ReadMcuVariableI2cExclusive(u16 *v,u32 s,u16 a)
{ return CAMU_ReadRegisterI2cExclusive(v,s,a); }
Result CAMU_WriteRegisterI2c(u32 s,u16 a,u16 v)
{ ++writes; if (writes==fail_write_at || (fail_restore && v==100)) return -1; values[a]=clear_write_bit8?(v&~0x100):v;
  if(auto_response && a>=0x3056 && a<=0x305c && !(a&1)) {
      const u16 target[]={0x3032,0x3036,0x3034,0x3038};values[target[(a-0x3056)/2]]=(u16)(((v>>9)&7)*256);
  }
  return 0; }
Result CAMU_WriteMcuVariableI2c(u32 s,u16 a,u16 v)
{ return CAMU_WriteRegisterI2c(s,a,v); }
Result CAMU_IsAutoExposure(bool *v,u32 s) { *v=ae; return 0; }
Result CAMU_IsAutoWhiteBalance(bool *v,u32 s) { *v=awb; return 0; }
Result CAMU_SetAutoExposure(u32 s,bool on) { ae=on;values[0xa11d]=on;return 0; }
Result CAMU_SetAutoWhiteBalance(u32 s,bool on) { awb=on;values[0xa11f]=on;return 0; }
Result CAMU_SetFrameRate(u32 s,CAMU_FrameRate r) {++rate_sets;values[0x300a]=r==8?518:r==5?2114:704;return 0;}
Result CAMU_SetPhotoMode(u32 s,CAMU_PhotoMode m) {return 0;}
Result CAMU_GetSuitableY2rStandardCoefficient(Y2RU_StandardCoefficient *m) {*m=1;return 0;}
Result CAMU_SetExposure(u32 s,s8 e) {values[0x3012]=100;return 0;}
Result GSPGPU_FlushDataCache(const void *b,u32 n) { return 0; }
Result GSPGPU_InvalidateDataCache(const void *b,u32 n) { return 0; }
Result CAMU_SetReceiving(Handle *e,void *b,u32 p,u32 n,s16 t)
{ ++receives; if(receives==drift_at_receive)++values[0x3012];
  if(receives==isp_drift_at)++values[0x32d6];
  if(receives==isp_settle_at)++values[0x32d4];
  *e=7; dma=b; memset(dma,100,n);
  if(auto_response)for(unsigned i=0;i<n;i+=2){dma[i]=(u8)fmin(255,values[0x3012]);dma[i+1]=128;}
  return 0; }
Result CAMU_StartCapture(u32 p) { return 0; }
Result CAMU_StopCapture(u32 p) { ++stops; return fail_stop ? -1 : 0; }
Result CAMU_ClearBuffer(u32 p) { return 0; }
Result CAMU_IsBusy(bool *v,u32 p) { *v=stay_busy; return 0; }
Result svcSleepThread(s64 t) { return 0; }
Result CAMU_IsFinishedReceiving(bool *v,u32 p) { *v=!(fail_second_receive && receives==2); return 0; }
Result svcWaitSynchronization(Handle e,s64 t) { return positive_timeout ? (Result)0x09401bfe : timeout_capture ? -1 : 0; }
Result svcCloseHandle(Handle e) { ++closed_events; return 0; }

static Probe setup(void)
{
    Probe p={.sensor=1,.select=SELECT_OUT1,.port=PORT_CAM1,.transfer=5120,
             .map_known=true,.capture_ready=true};
    p.calls=fopen("build/probe-calls.tmp","w+"); p.reads=fopen("build/probe-reads.tmp","w+");
    p.frames=fopen("build/probe-frames.tmp","w+"); p.meta=fopen("build/probe-meta.tmp","w+");
    assert(p.calls && p.reads && p.frames && p.meta);
    fputs("sensor,phase,label,operation,addr,value,result,time_ms\n",p.calls);
    fputs("sensor,phase,label,frame,when,space,name,addr,value,result,time_ms\n",p.reads);
    fputs("sensor,phase,label,frame,measured,ok,result,received,y_mean,y_p50,endpoint_low,endpoint_high,crc32,start_ms,end_ms\n",p.frames);
    p.pixels=malloc(BYTES); assert(p.pixels);
    strcpy(p.directory,".");
    values[0x3012]=100;
    values[0xa11d]=values[0xa11f]=0;ae=awb=false;
    writes=receives=closed_events=stops=fail_write_at=0;
    drift_at_receive=rate_sets=0;
    cancel_after_write=timeout_capture=fail_restore=deny_read=fail_stop=fail_second_receive=positive_timeout=stay_busy=false;
    clear_write_bit8=reject_ffff_read=false;
    auto_response=false;isp_drift_at=isp_settle_at=0;
    return p;
}
static void cleanup(Probe *p)
{ fclose(p->calls); fclose(p->reads); fclose(p->frames); fclose(p->meta);
  if(p->calibration)fclose(p->calibration);
  if(p->burst)fclose(p->burst);
  if(p->roi)fclose(p->roi);
  if(p->noise)fclose(p->noise);
  if(p->execution)fclose(p->execution);
  free(p->pixels); }

static void save_fixture(Probe *p)
{
    flush_logs(p);
    const char *names[]={"calls.csv","reads.csv","frames.csv"};FILE *files[]={p->calls,p->reads,p->frames};
    for(unsigned i=0;i<3;++i) {
        char path[240];snprintf(path,sizeof path,"%s/%s",p->directory,names[i]);
        FILE *out=fopen(path,"wb");assert(out);rewind(files[i]);int ch;
        while((ch=fgetc(files[i]))!=EOF)assert(fputc(ch,out)!=EOF);
        assert(!fclose(out));
    }
}

int main(void)
{
    Probe p;
    bool auto_only=getenv("EXPOSURE_AUTO_ONLY")!=NULL;
    if(auto_only)goto auto_contract_tests;
    const u16 addr=0x3012, base=100;
    p=setup();
    assert(!write_reg(&p,0x304a,1,"forbidden_otp"));
    assert(!writes);
    cancel_after_write=true;
    assert(!direct_trial(&p,"integration",&addr,&base,1,false));
    assert(p.aborted && values[addr]==base && writes==2);
    cleanup(&p);

    p=setup(); timeout_capture=true;
    assert(!direct_trial(&p,"integration",&addr,&base,1,false));
    assert(values[addr]==base && writes==2 && closed_events==1 && stops==1);
    assert(p.failures==1 && !p.phase_success);
    cleanup(&p);

    p=setup(); timeout_capture=fail_stop=true;
    assert(!direct_trial(&p,"integration",&addr,&base,1,false));
    assert(values[addr]==base && !p.capture_ready && p.streaming);
    assert(!capture(&p,"must_not_reuse_dma",0,false,false) && receives==1);
    cleanup(&p);

    p=setup(); fail_restore=true; cancel_after_write=true;
    assert(!direct_trial(&p,"integration",&addr,&base,1,false));
    assert(p.aborted && values[addr]!=base);
    cleanup(&p);

    p=setup(); p.map_known=false;
    assert(direct_probe(&p) && !writes && !receives);
    cleanup(&p);

    p=setup(); p.capture_ready=false;
    assert(direct_probe(&p) && !writes);
    cleanup(&p);

    p=setup(); deny_read=true;
    u16 value=123;
    assert(R_FAILED(read_reg(&p,false,addr,&value,"denied",-1,"initial","coarse")));
    assert(value==0 && !writes);
    cleanup(&p);
    p=setup();
    assert(capture(&p,"good_frame",8,true,false));
    assert(p.captures==1 && !p.failures && receives==1 && closed_events==1);
    cleanup(&p);
    p=setup();
    assert(acquisition_preflight(&p) && p.one_shot && receives==8 && p.captures==8);
    cleanup(&p);
    remove("s1-p001-preflight_continuous.pgm");
    remove("s1-p002-preflight_single.pgm");
    p=setup();p.map_known=false;
    assert(acquisition_preflight(&p) && !p.one_shot && receives==4);
    cleanup(&p);
    remove("s1-p001-preflight_continuous.pgm");
    p=setup();fail_second_receive=true;
    assert(acquisition_preflight(&p) && p.one_shot && receives==6 && p.captures==5 && p.failures==1);
    cleanup(&p);
    remove("s1-p002-preflight_single.pgm");
    p=setup();timeout_capture=true;
    assert(!acquisition_preflight(&p) && p.failures==2 && receives==2);
    cleanup(&p);
    p=setup();positive_timeout=true;
    assert(!capture(&p,"positive_wait_timeout",0,false,false));
    assert(!p.captures && p.failures==1);
    cleanup(&p);
    p=setup();stay_busy=true;p.streaming=true;
    stop(&p,"cannot_reuse_busy_dma");
    assert(!p.capture_ready && p.streaming);
    assert(!capture(&p,"busy_dma_forbidden",0,false,false));
    cleanup(&p);
    assert(probe_crc32((const u8 *)"123456789",9)==UINT32_C(0xcbf43926));
    p=setup();p.first_phase=34;p.phase=33;
    assert(sample(&p,"replayed_setup") && !receives && !p.samples && !p.phase_success);
    p.phase=34;
    assert(sample(&p,"resumed_measurement") && receives==20 && p.samples==1 && p.successful_samples==1);
    cleanup(&p);remove("s1-p034-resumed_measurement.pgm");
    p=setup();writes=1;cancel_after_write=true;
    assert(!sample(&p,"aborted_before_first_frame") && !p.phase_success && !p.successful_samples && !receives);
    cleanup(&p);
    u16 encoded=0;
    assert(cal_encode_analog(0x220,2,&encoded) && fabs(cal_analog(encoded)-2)<1e-6 && (encoded&0xe00)==0x200);
    assert(!cal_encode_analog(0x220,16,&encoded));
    assert(!cal_encode_analog(0xf220,2,&encoded));
    assert(cal_digital(0x2ab,4,&encoded) && (encoded&0x1ff)==(0x2ab&0x1ff) && ((encoded>>9)&7)==4);
    assert(!cal_digital(0x220,0,&encoded) && !cal_digital(0x220,8,&encoded));
    assert(!cal_rule("unknown",1) && !cal_rule("analog",1));
    p=setup();
    u8 *screen=malloc(320*240*3+32);assert(screen);memset(screen,0xa5,320*240*3+32);
    assert(!cal_preview_draw(screen,320*240*2,240,320,p.pixels));
    assert(cal_preview_draw(screen,320*240*3,240,320,p.pixels));
    for(unsigned i=320*240*3;i<320*240*3+32;++i)assert(screen[i]==0xa5);
    free(screen);cleanup(&p);
    p=setup();p.calibration=fopen("build/calibration-plan.tmp","w+");
    const u16 cal_base[]={100,0,0,0,0},cal_target[]={50,0,0,0,0};
    cancel_after_write=true;
    assert(!cal_pair(&p,"abort_cal","integration",cal_target,cal_base,1,false));
    assert(values[0x3012]==100 && !receives && p.aborted);
    cleanup(&p);
    p=setup();p.calibration=fopen("build/calibration-plan.tmp","w+");timeout_capture=true;
    assert(!cal_pair(&p,"timeout_cal","integration_long",cal_target,cal_base,1,true));
    assert(values[0x3012]==100 && p.wait_ns==INT64_C(30000000000));cleanup(&p);
    p=setup();p.one_shot=true;p.smoke=true;p.phase=2;
    mkdir("build/calibration-smoke",0777);strcpy(p.directory,"build/calibration-smoke");
    assert(calibration_probe(&p));assert(p.phase==3 && p.successful_samples==1 && !writes);
    cleanup(&p);
    for(unsigned part=CAL_GAIN;part<=CAL_LIMITS_FFFE;++part) {
        p=setup();p.one_shot=true;p.phase=2;p.calibration_part=part;p.frame_log_flush=true;
        for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
        snprintf(p.directory,sizeof p.directory,"build/calibration-%s",cal_scope_name(part));mkdir(p.directory,0777);
        assert(calibration_probe(&p));
        assert(p.phase==cal_scope_stages(part)+2 && p.samples==cal_scope_stages(part) && p.successful_samples==cal_scope_stages(part));
        if(part==CAL_LIMITS || part==CAL_LIMITS_FFFE)assert(rate_sets==2);
        assert(values[0x3012]==100 && values[0x3056]==0x220);
        save_fixture(&p);cleanup(&p);
    }
    p=setup();p.one_shot=true;p.phase=2;p.calibration_part=CAL_CODES_BOOST;p.frame_log_flush=true;clear_write_bit8=true;
    for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
    strcpy(p.directory,"build/calibration-codes-boost-rejected");mkdir(p.directory,0777);
    assert(!calibration_probe(&p));
    assert(p.phase==16 && p.successful_samples==12 && !p.aborted && !p.io_error);
    assert(values[0x3012]==100 && values[0x3056]==0x220);save_fixture(&p);cleanup(&p);
    p=setup();fclose(p.calls);fclose(p.reads);fclose(p.frames);fclose(p.meta);assert(ram_begin());
    p.calls=probe_fopen("calls.csv","w");p.reads=probe_fopen("reads.csv","w");p.frames=probe_fopen("frames.csv","w");p.meta=probe_fopen("manifest.txt","w");
    assert(p.calls && p.reads && p.frames && p.meta);
    fputs("sensor,phase,label,operation,addr,value,result,time_ms\n",p.calls);
    fputs("sensor,phase,label,frame,when,space,name,addr,value,result,time_ms\n",p.reads);
    fputs("sensor,phase,label,frame,measured,ok,result,received,y_mean,y_p50,endpoint_low,endpoint_high,crc32,start_ms,end_ms\n",p.frames);
    p.phase=2;p.one_shot=true;p.frame_log_flush=true;p.calibration_part=CAL_DIGITAL_HEADROOM;
    for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
    strcpy(p.directory,"ram:/actual-c-fixture");
    assert(calibration_probe(&p) && p.successful_samples==6);
    fputs("REQUEST only_sensor=1 first_phase=3 last_phase=8\nCALIBRATION contract=2 expected_stages=6 scope=digital-headroom\nSENSOR_END sensor=1 complete=1\nMEASUREMENTS expected=6 complete=6 all_complete=1\nEND complete=1 aborted=0 io_error=0\n",p.meta);
    cleanup(&p);assert(ram_freeze());mkdir("build/calibration-ram-fixture",0777);
    for(unsigned i=0;i<ram_log.count;++i) {
        RamLogFile *r=&ram_log.files[i];char path[256];snprintf(path,sizeof path,"build/calibration-ram-fixture/%s",r->name);
        FILE *out=fopen(path,"wb");assert(out);assert(fwrite(r->data,1,r->size,out)==r->size);assert(!fclose(out));
    }
    ram_release();
    for(unsigned i=0;i<2;++i) {
        p=setup();p.one_shot=true;p.phase=2;p.calibration_part=i?CAL_LIMITS_FFFE:CAL_LIMITS;reject_ffff_read=true;
        for(unsigned j=1;j<5;++j)values[cal_addresses[j]]=0x220;
        snprintf(p.directory,sizeof p.directory,"build/calibration-sentinel-%u",i);mkdir(p.directory,0777);
        bool ok=calibration_probe(&p);assert(ok==(i!=0));
        assert(p.phase==10 && p.successful_samples==(i?8:6) && values[0x3012]==100);
        cleanup(&p);
    }
    p=setup();p.one_shot=true;p.phase=2;
    for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
    const u16 locked_base[]={100,0x220,0x220,0x220,0x220},locked_target[]={50,0x220,0x220,0x220,0x220};
    p.calibration=fopen("build/calibration-drift.tmp","w+");assert(p.calibration);drift_at_receive=8;
    assert(!cal_stage(&p,"late_drift","integration",locked_target,1,0,false,false));
    assert(p.control_drift && !p.phase_success && receives==8);
    drift_at_receive=0;
    assert(cal_stage(&p,"verified_restore","restore",locked_base,1,0,false,true));
    assert(!p.control_drift && p.phase_success && values[0x3012]==100);cleanup(&p);
    p=setup();p.one_shot=true;p.phase=2;p.calibration_part=CAL_LIMITS;
    for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x260;
    strcpy(p.directory,"build/calibration-limits-failed");mkdir(p.directory,0777);fail_write_at=1;
    assert(!calibration_probe(&p));
    for(unsigned i=1;i<5;++i)assert(values[cal_addresses[i]]==0x260);
    assert(values[0x3012]==100 && writes==9);cleanup(&p);
    p=setup();fclose(p.calls);p.calls=fopen("/dev/full","w");assert(p.calls);p.frame_log_flush=true;assert(buffer_log(&p,p.calls));
    record_call(&p,"test","write_failure",0,0,0);flush_logs(&p);assert(p.io_error && !running(&p));cleanup(&p);
    p=setup();p.one_shot=true;
    for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
    mkdir("build/calibration-test",0777);strcpy(p.directory,"build/calibration-test");
    p.phase=2;
    assert(calibration_probe(&p));
    assert(p.phase==CAL_LAST_PHASE && p.samples==CAL_STAGE_COUNT && p.successful_samples==CAL_STAGE_COUNT);
    assert(values[0x3012]==100 && values[0x3056]==0x220);
    cleanup(&p);
    assert(!rename("build/probe-calls.tmp","build/full-probe-calls.tmp"));
    assert(!rename("build/probe-reads.tmp","build/full-probe-reads.tmp"));
    assert(!rename("build/probe-frames.tmp","build/full-probe-frames.tmp"));
auto_contract_tests:;
    /* Curve fixtures validate mathematics, not hardware calibration claims. */
    FieldLabel curve[]={{1,.125},{2,.5},{4,2},{8,8},{16,32}};
    FieldModel fit=field_fit(curve,5);
    assert(fit.fitted && fit.validated && fabs(fit.slope-2)<1e-10);
    assert(fabs(field_predict(&fit,4)-2)<1e-10 && field_in_domain(&fit,4) && !field_in_domain(&fit,32));
    FieldModel one=field_fit(curve,1);
    assert(one.fitted && !one.validated && one.slope==1 && !field_in_domain(&one,1));
    curve[2].seconds=8;
    assert(!field_fit(curve,5).validated);
    assert(fabs(field_predict(&fit,4)-2)<1e-10); /* previous model is frozen */
    FieldLabel bad[]={{0,.25}};assert(!field_fit(bad,1).fitted);
    u8 *color=calloc(320*240,3),*source=malloc(BYTES);assert(color && source);
    for(unsigned i=0;i<BYTES;i+=4){source[i]=source[i+2]=120;source[i+1]=128;source[i+3]=180;}
    assert(field_preview_draw(color,320*240*3,240,320,source,1));
    assert(color[2]>color[0]); /* actual RGB, not repeated Y */
    assert(!field_preview_draw(color,320*240*3,240,320,source,5));free(color);free(source);
    for(unsigned fault=0;fault<3;++fault) {
        p=setup();p.one_shot=true;p.phase=2;p.calibration_part=CAL_EXPOSURE_FIELD;p.automation_fps=5;p.frame_log_flush=true;p.raw=true;
        auto_response=true;values[0x302a]=8;values[0x300c]=842;values[0x3014]=633;
        for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
        for(unsigned i=0;i<4;++i){values[auto_isp_addresses[i]]=128;values[0x3032+2*i]=256;}
        snprintf(p.directory,sizeof p.directory,"build/exposure-field-%u",fault);mkdir(p.directory,0777);
        if(fault==1)isp_drift_at=22;
        if(fault==2)fail_write_at=1;
        FieldRun run={.prior=one};snprintf(run.directory,sizeof run.directory,"%s",p.directory);
        assert(field_capture(&p,&run)==(fault==0));
        assert(p.successful_samples<=FIELD_MAX_STAGES && values[0x3012]==100 && values[0x3056]==0x220);
        if(!fault) {
            assert(run.complete && run.converged && run.images[0].coarse<run.images[1].coarse && run.images[2].coarse>run.images[1].coarse);
            assert(run.images[1].prediction>0 && run.prior.count==1 && !run.reference_saved);
            assert(field_save_prior(&run) && field_save_summary(&run));
            /* Actual filesystem round trip, not a mocked persistence API. */
            mkdir("sdmc:",0777);mkdir("sdmc:/3ds",0777);mkdir(FIELD_ROOT,0777);
            remove(FIELD_LABELS);remove("build/exposure-field-0/reference-A.csv");
            double frozen_prediction=run.images[1].prediction;
            assert(field_save_reference(&run,.25) && run.reference_saved && run.reference_seconds==.25);
            assert(run.prior.count==1 && run.images[1].prediction==frozen_prediction);
            FieldLabel roundtrip[FIELD_MAX_LABELS];unsigned saved_count;
            assert(field_load_labels(FIELD_LABELS,roundtrip,&saved_count) && saved_count==1);
            assert(fabs(roundtrip[0].dose-run.images[1].dose)<1e-10 && roundtrip[0].seconds==.25);
            assert(!field_save_reference(&run,.5)); /* no duplicate label */
            remove(FIELD_LABELS);
        } else assert(!run.complete);
        if(fault)assert(!field_save_reference(&run,.25)); /* no failed-run training */
        FILE *manifest=fopen("build/exposure-field-manifest.tmp","w");assert(manifest);
        rewind(p.meta);int ch;while((ch=fgetc(p.meta))!=EOF)assert(fputc(ch,manifest)!=EOF);assert(!fclose(manifest));
        char destination[240];snprintf(destination,sizeof destination,"%s/manifest.txt",p.directory);
        assert(!rename("build/exposure-field-manifest.tmp",destination));
        save_fixture(&p);cleanup(&p);
    }
    FILE *invalid=fopen("build/field-invalid-labels.csv","w");assert(invalid);
    fputs("dose,reference_seconds,run\n1,0,invalid\n",invalid);assert(!fclose(invalid));
    FieldLabel loaded[FIELD_MAX_LABELS];unsigned loaded_count;
    assert(!field_load_labels("build/field-invalid-labels.csv",loaded,&loaded_count));
    assert(auto_next_coarse(100,.18)==100 && auto_next_coarse(528,0)==528);
    assert(auto_next_coarse(1,1)==1 && !auto_next_coarse(100,NAN));
    assert(fabs(auto_luma(118,0,0,1)-.181164244)<1e-6);
    for(unsigned fault=0;fault<3;++fault) {
        p=setup();p.one_shot=true;p.phase=2;p.calibration_part=CAL_EXPOSURE_AUTO;p.frame_log_flush=true;p.raw=true;
        auto_response=true;
        values[0x302a]=8;
        for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
        for(unsigned i=0;i<4;++i){values[auto_isp_addresses[i]]=128;values[0x3032+2*i]=256;}
        snprintf(p.directory,sizeof p.directory,"build/exposure-auto-%u",fault);mkdir(p.directory,0777);
        if(fault==1)isp_drift_at=22;
        if(!fault)isp_settle_at=16;
        if(fault==2)fail_write_at=1;
        bool ok=calibration_probe(&p);
        assert(ok==(fault==0));
        assert(p.successful_samples<=AUTO_MAX_STAGES && values[0x3012]==100 && values[0x3056]==0x220);
        if(fault==1)assert(p.control_drift && receives==22);
        if(!fault)assert(p.expected_isp[0]==129 && !p.isp_pin_pending);
        if(!fault)assert(fabs(log2(.18/auto_linear(119.0/255)))<.125);
        save_fixture(&p);cleanup(&p);
    }
    assert(ram_begin());assert(!probe_fopen("s1-p004-raw.yuyv","wb"));ram_release();
    assert(ram_begin_profile(true));
    FILE *raw=probe_fopen("s1-p004-raw.yuyv","wb");assert(raw);
    u8 row[1280]={0};for(unsigned i=0;i<480;++i)assert(fwrite(row,1,sizeof row,raw)==sizeof row);
    assert(!fclose(raw) && ram_log.limit==RAM_AUTO_BYTES && ram_freeze());ram_release();
    assert(ram_begin_profile(true));
    const char *logs[]={"calls.csv","reads.csv","frames.csv","manifest.txt","ipc.csv","lifecycle.txt",
        "calibration_plan.csv","frame_burst.csv","roi.csv","noise.csv","execution.csv"};
    for(unsigned i=0;i<sizeof logs/sizeof *logs;++i){FILE *f=probe_fopen(logs[i],"w");assert(f);assert(!fclose(f));}
    for(unsigned i=0;i<19;++i){char name[80];snprintf(name,sizeof name,"s1-p%03u-budget.%s",i,i<2?"pgm":"yuyv");
        FILE *f=probe_fopen(name,"wb");assert(f);assert(!fclose(f));}
    assert(ram_log.used<=RAM_AUTO_BYTES && !ram_log.failed && ram_freeze());ram_release();
    /* Preserve the legacy 678-frame fixture consumed by calibration_report.py. */
    if(!auto_only) {
        assert(!rename("build/full-probe-calls.tmp","build/probe-calls.tmp"));
        assert(!rename("build/full-probe-reads.tmp","build/probe-reads.tmp"));
        assert(!rename("build/full-probe-frames.tmp","build/probe-frames.tmp"));
    }
    puts("PASS actual probe driver and calibration: registered auto feedback, ISP drift/write failure stop and restoration, raw RAM profile, legacy scopes");
    return 0;
}
