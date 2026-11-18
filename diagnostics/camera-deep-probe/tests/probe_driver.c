/* Execute the actual production trial/cleanup functions against injected failures. */
#include <stddef.h>
static void *rebuild_test_allocate(size_t size,size_t alignment);
#define PROBE_REBUILD_ALLOC rebuild_test_allocate
#define main filmprobe_app_main
#include "../probes/exposure/main.c"
#undef main
#include <assert.h>
#include <sys/wait.h>
#include <signal.h>

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
static bool fail_seq_disable,fail_seq_restore,simulate_live_isp;
static bool fail_awb_force,fail_awb_restore;
static unsigned refresh_commands,fail_refresh_at;
static bool refresh_never_completes,fail_preview_enable,refresh_low_bits_change;
static bool refresh_unit_awb,fail_manual_disable;
static Probe *update_probe_active;
static unsigned update_pattern,update_frames_seen;
static bool update_fail_mode;
static Probe *no_receive_active;
static unsigned no_receive_pattern,vsync_closed;
static s64 vsync_clock;
static bool simulate_debug_read_delay;
static u8 *dma;
static Probe *hg_active;
static unsigned hg_pattern,hg_red_reads;
static Probe *gain_matrix_active;
static unsigned gain_matrix_pattern,gain_matrix_waits,gain_matrix_dmas;
static Probe *rebuild_active;
static unsigned rebuild_pattern,rebuild_waits,rebuild_epoch_frames,rebuild_previous_coarse;
static u64 rebuild_epoch;
static u8 *rebuild_completed_buffer;
static bool rebuild_fd_tail;
static void *rebuild_test_allocate(size_t size,size_t alignment) {(void)alignment;return rebuild_active && rebuild_pattern==12?NULL:malloc(size);}

void hidScanInput(void) {}
u32 hidKeysHeld(void) { return cancel_after_write && writes ? KEY_B : 0; }
bool aptMainLoop(void) { return true; }
static u64 simulated_clock;
u64 osGetTime(void) { return ++simulated_clock; }
Result CAMU_ReadRegisterI2cExclusive(u16 *v,u32 s,u16 a)
{ if (deny_read || (update_fail_mode && a==0x2703)) return (Result)0xd900182f;
  if(simulate_debug_read_delay && no_receive_active && no_receive_active->streaming && a>=0x32d4 && a<=0x32da && values[a]==128)values[a]=133;
  if(hg_active && hg_active->calibration_part==CAL_CCM_NORMALIZATION && hg_pattern==11 && a==0x233e && (values[0xa355]&64))return (Result)0xc9602fef;
  if(hg_active && (hg_active->calibration_part==CAL_CCM_BYTES || hg_active->calibration_part==CAL_CCM_BOOT) && hg_pattern==14 && a==0x2332)return (Result)0xc9602fef;
  if(gain_matrix_active && gain_matrix_pattern==6 && a==0xa33e)return (Result)0xc9602fef;
  if(hg_active && hg_pattern==3 && a==0xab17)return -1;
  if(hg_active && hg_active->streaming && a==0x32d4 && ++hg_red_reads>=3 &&
     (hg_pattern==1 || (hg_pattern!=2 && ((hg_active->calibration_part==CAL_CCM_NORMALIZATION || (hg_active->calibration_part==CAL_CCM_BYTES || hg_active->calibration_part==CAL_CCM_BOOT))?
        (hg_pattern!=10 || !(values[0xa355]&64)):(values[0xa102]&8)))))
      for(unsigned i=0;i<4;++i)values[auto_isp_addresses[i]]=
          (hg_active->calibration_part==CAL_CCM_NORMALIZATION || (hg_active->calibration_part==CAL_CCM_BYTES || hg_active->calibration_part==CAL_CCM_BOOT)) && hg_pattern==0 && (values[0xa355]&64)?128:133;
  if(gain_matrix_active && (gain_matrix_pattern==11 || gain_matrix_pattern==12) && gain_matrix_active->sweep_manual_locked && !gain_matrix_active->streaming && values[0xa102]==2 && a==0x32d6)values[a]=gain_matrix_pattern==11?133:0;
  *v=values[a];
  if(rebuild_active && rebuild_pattern==4 && rebuild_active->streaming && a==0x3012)++*v;
  return reject_ffff_read && *v==0xffff?(Result)0xc9602fef:0; }
Result CAMU_ReadMcuVariableI2cExclusive(u16 *v,u32 s,u16 a)
{
  if(((hg_active && (hg_active->calibration_part==CAL_CCM_BYTES || hg_active->calibration_part==CAL_CCM_BOOT)) || gain_matrix_active) && a>=0xa332 && a<=0xa347) {
      if(rebuild_active && rebuild_pattern==5 && a==0xa33e)return (Result)0xc9602fef;
      if(gain_matrix_active && gain_matrix_pattern==6 && a==0xa33e)return (Result)0xc9602fef;
      if(hg_pattern==12 && a==0xa33e)return (Result)0xc9602fef;
      u16 word=values[(a&0x7fffu)&~1u];*v=(u16)((a&1)?(word&255):(word>>8));
      if(hg_pattern==11 && a==0xa332)*v^=1;
      if(hg_pattern==13 && a==0xa333)*v=300;
      if(gain_matrix_active && gain_matrix_pattern==9 && a==0xa333)*v-=2;
      return 0;
  }
  if(gain_matrix_active && (a==0xa703 || a==0xa704)){*v=a==0xa703?2:128;if((gain_matrix_pattern==10 || (rebuild_active && rebuild_pattern==6)) && a==0xa704)*v=129;return 0;}
  return CAMU_ReadRegisterI2cExclusive(v,s,a);
}
Result CAMU_WriteRegisterI2c(u32 s,u16 a,u16 v)
{ if(hg_active && a==0x32d4)hg_red_reads=0;
  ++writes; if (writes==fail_write_at || (fail_restore && v==100)) return -1; values[a]=clear_write_bit8?(v&~0x100):v;
  if(auto_response && a>=0x3056 && a<=0x305c && !(a&1)) {
      const u16 target[]={0x3032,0x3036,0x3034,0x3038};values[target[(a-0x3056)/2]]=(u16)(((v>>9)&7)*256);
  }
  return 0; }
Result CAMU_WriteMcuVariableI2c(u32 s,u16 a,u16 v)
{ if(rebuild_active && rebuild_pattern>=14 && rebuild_active->streaming && a==0xa102 && values[a]==2 && !(v&2))rebuild_fd_tail=true;
  if(hg_active && (hg_active->calibration_part==CAL_CCM_NORMALIZATION || (hg_active->calibration_part==CAL_CCM_BYTES || hg_active->calibration_part==CAL_CCM_BOOT)) && a==0xa355 && (v&64)) {
      if(hg_pattern==8)return -1;
      if(hg_pattern==9)v&=~64u;
  }
  if(hg_active && hg_pattern==4 && a==0xa102 && v==2)return -1;
  if(a==0xa102 && ((fail_seq_disable && v==10) || (fail_seq_restore && v==15)))return -1;
  if(a==0xa102 && v==10 && fail_manual_disable)return -1;
  if(a==0xa355 && ((fail_awb_force && v==33) || (fail_awb_restore && v==1)))return -1;
  if(a==0xa11f && v==1 && fail_preview_enable)return -1;
  if(a==0xa103) {
      if(rebuild_active)rebuild_fd_tail=false;
      if(update_probe_active)update_frames_seen=0;
      ++refresh_commands;if(refresh_commands==fail_refresh_at)return -1;
      Result rc=CAMU_WriteRegisterI2c(s,a,v);
      if(!refresh_never_completes)values[a]=0;
      if(refresh_low_bits_change)values[0xa355]|=refresh_commands==1?8:24;
      if(refresh_commands==1 && refresh_unit_awb)values[0xa34e]=values[0xa34f]=values[0xa350]=128;
      return rc;
  }
  return CAMU_WriteRegisterI2c(s,a,v); }
Result CAMU_IsAutoExposure(bool *v,u32 s) { *v=ae; return 0; }
Result CAMU_IsAutoWhiteBalance(bool *v,u32 s) { *v=awb; return 0; }
Result CAMU_SetAutoExposure(u32 s,bool on) { ae=on;values[0xa11d]=on;return 0; }
Result CAMU_SetAutoWhiteBalance(u32 s,bool on) { awb=on;values[0xa11f]=on;return 0; }
Result CAMU_SetFrameRate(u32 s,CAMU_FrameRate r) {++rate_sets;
 if(rebuild_active && rebuild_pattern==8 && r==5 && rate_sets>=3)return -1;
 if(gain_matrix_active && gain_matrix_pattern==8 && r==5 && rate_sets>=3)return -1;
 values[0x300a]=r==8?518:r==5?2114:704;
 if(rebuild_active && rebuild_pattern==7 && r==0)values[0x300a]=705;
 if(gain_matrix_active && gain_matrix_pattern==4 && r==0)values[0x300a]=705;
 return 0;}
Result CAMU_SetPhotoMode(u32 s,CAMU_PhotoMode m) {return 0;}
Result CAMU_GetSuitableY2rStandardCoefficient(Y2RU_StandardCoefficient *m) {*m=1;return 0;}
Result CAMU_SetExposure(u32 s,s8 e) {values[0x3012]=100;return 0;}
Result GSPGPU_FlushDataCache(const void *b,u32 n) { return 0; }
Result GSPGPU_InvalidateDataCache(const void *b,u32 n) { return 0; }
Result CAMU_SetReceiving(Handle *e,void *b,u32 p,u32 n,s16 t)
{ if(ram_log.exposure_sweep && !(rebuild_active && rebuild_active->sweep_manual_locked))simulated_clock+=199;
  if(rebuild_active && rebuild_active->sweep_manual_locked)assert(b!=rebuild_completed_buffer);
  ++receives; if(receives==drift_at_receive)++values[0x3012];
  if(receives==isp_drift_at)++values[0x32d6];
  if(receives==isp_settle_at)++values[0x32d4];
  *e=7; dma=b; memset(dma,100,n);
  if(auto_response)for(unsigned i=0;i<n;i+=2){dma[i]=(u8)fmin(255,values[0x3012]);dma[i+1]=128;}
  return 0; }
Result CAMU_StartCapture(u32 p) {
  if(rebuild_active && rebuild_active->sweep_manual_locked){rebuild_epoch=simulated_clock;rebuild_epoch_frames=0;}
  if(simulate_live_isp && !(values[0xa355]&32) && values[0x32d4]==128)
      for(unsigned i=0;i<4;++i)values[auto_isp_addresses[i]]=160;
  return 0; }
Result CAMU_StopCapture(u32 p) { ++stops; return fail_stop ? -1 : 0; }
Result CAMU_ClearBuffer(u32 p) { return 0; }
Result CAMU_IsBusy(bool *v,u32 p) { *v=stay_busy; return 0; }
void svcSleepThread(s64 t) { if(hg_active || gain_matrix_active)simulated_clock+=(u64)(t/1000000); }
Result CAMU_IsFinishedReceiving(bool *v,u32 p) { if(gain_matrix_active && gain_matrix_pattern==7 && gain_matrix_dmas==2){*v=false;return 0;}*v=!(fail_second_receive && receives==2); return 0; }
Result CAMU_GetVsyncInterruptEvent(Handle *e,u32 p)
{ if(no_receive_pattern==5)return -1;*e=99;return 0; }
Result CAMU_GetLatestVsyncTiming(s64 *timing,u32 p,u32 past)
{ assert(past==1);if(no_receive_pattern==6)return -1;*timing=vsync_clock;return 0; }
Result svcClearEvent(Handle e) { assert(e==99);return no_receive_pattern==7?-1:0; }
Result svcWaitSynchronization(Handle e,s64 t) {
    if(rebuild_active && rebuild_active->sweep_manual_locked) {
        unsigned period=values[0x300a]==704?67:200;
        u64 target=rebuild_epoch+(u64)(++rebuild_epoch_frames)*period;if(simulated_clock<target)simulated_clock=target;
        vsync_clock+=period;++rebuild_waits;
        if(rebuild_pattern==3 && rebuild_waits==17)return (Result)0x09401bfe;
        if(rebuild_fd_tail){for(unsigned i=0;i<4;++i)values[auto_isp_addresses[i]]=133;if(rebuild_pattern==14)rebuild_fd_tail=false;}
        if(rebuild_pattern==2 || (rebuild_pattern!=1 && (values[0xa102]&2)))for(unsigned i=0;i<4;++i)values[auto_isp_addresses[i]]=133;
        if(rebuild_pattern==13 && (values[0xa102]&2))values[0x3012]=8;
        rebuild_completed_buffer=dma;
        unsigned coarse=values[0x3012];if(rebuild_pattern==9){unsigned prior=rebuild_previous_coarse;rebuild_previous_coarse=coarse;if(prior)coarse=prior;}
        double brightness=coarse*.8*(values[0x3056]&127)/16*((values[0x3056]>>9)&7)*values[0x32d6]/128;
        unsigned y=rebuild_pattern==10?0:rebuild_pattern==11?255:(unsigned)fmin(255,brightness);
        for(unsigned i=0;i<BYTES;i+=2){dma[i]=(u8)y;dma[i+1]=128;}
        return 0;
    }

    if(gain_matrix_active && gain_matrix_active->sweep_manual_locked) {
        unsigned period=values[0x300a]==704?67:200;
        vsync_clock+=period;simulated_clock+=period;
        if(e==99) {
            ++gain_matrix_waits;
            if(gain_matrix_pattern==3 && gain_matrix_waits==7)return (Result)0x09401bfe;
            if(gain_matrix_pattern==5 && gain_matrix_waits==5)++values[0x3012];
        } else ++gain_matrix_dmas;
        if(gain_matrix_pattern!=2 && !(gain_matrix_pattern==1 && !(values[0xa102]&2)))
            for(unsigned i=0;i<4;++i)values[auto_isp_addresses[i]]=133;
    }
    if(hg_active && hg_active->sweep_manual_locked && e==99) {
        if(hg_pattern==5)return (Result)0x09401bfe;
        vsync_clock+=200;
        if(hg_pattern==7)++values[0x3012];
    }
    if(no_receive_active && no_receive_active->sweep_manual_locked) {
        if(e==99) {
            if(no_receive_pattern==3)return (Result)0x09401bfe;
            if(no_receive_pattern!=4)vsync_clock+=200;
            if(no_receive_pattern==0)for(unsigned i=0;i<4;++i)values[auto_isp_addresses[i]]=133;
        } else if(no_receive_pattern!=2) {
            for(unsigned i=0;i<4;++i)values[auto_isp_addresses[i]]=133;
        }
        if(no_receive_pattern==8 && e==99)++values[0x3012];
    }
    if(update_probe_active && update_probe_active->sweep_manual_locked) {
        ++update_frames_seen;
        if(update_pattern==3 && update_probe_active->phase==8 && update_frames_seen==2)return (Result)0x09401bfe;
        if(update_pattern==4)++values[0x3012];
        if(update_pattern==0 || (update_pattern==1 && update_frames_seen==1))
            for(unsigned i=0;i<4;++i)values[auto_isp_addresses[i]]=133;
    }
    return positive_timeout ? (Result)0x09401bfe : timeout_capture ? -1 : 0;
}
Result svcCloseHandle(Handle e) { ++closed_events;if(e==99)++vsync_closed;return 0; }

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
    values[0xa102]=15;fail_seq_disable=fail_seq_restore=simulate_live_isp=false;
    values[0xa355]=1;values[0xa104]=3;fail_awb_force=fail_awb_restore=false;
    values[0xa103]=0;refresh_commands=fail_refresh_at=0;refresh_never_completes=fail_preview_enable=false;
    refresh_low_bits_change=false;
    refresh_unit_awb=fail_manual_disable=false;
    values[0xa34e]=132;values[0xa34f]=128;values[0xa350]=105;values[0x221f]=128;
    update_probe_active=NULL;update_pattern=update_frames_seen=0;update_fail_mode=false;
    no_receive_active=NULL;no_receive_pattern=vsync_closed=0;vsync_clock=1000;
    memset(&boot_journal,0,sizeof boot_journal);boot_host_file=NULL;
    gain_matrix_active=NULL;gain_matrix_pattern=gain_matrix_waits=gain_matrix_dmas=0;
    rebuild_active=NULL;rebuild_pattern=rebuild_waits=rebuild_epoch_frames=rebuild_previous_coarse=0;rebuild_epoch=0;rebuild_completed_buffer=NULL;rebuild_fd_tail=false;
    simulate_debug_read_delay=false;hg_active=NULL;hg_pattern=hg_red_reads=0;
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

static void sweep_contract_tests(void)
{
    for(unsigned scenario=0;scenario<27;++scenario) {
        unsigned fault=scenario<8?scenario%4:0;
        bool diagnostic=scenario>=4;
        Probe p=setup();p.one_shot=true;p.phase=2;p.calibration_part=CAL_EXPOSURE_SWEEP;
        if(diagnostic)p.calibration_part=CAL_EXPOSURE_SWEEP_CHECK;
        bool refresh=(scenario>=12 && scenario<=17) || scenario>=19;
        if(refresh)p.calibration_part=CAL_EXPOSURE_SWEEP_REFRESH;
        bool manual=scenario>=21;
        if(manual){p.calibration_part=CAL_EXPOSURE_SWEEP_MANUAL;refresh_unit_awb=scenario!=22;}
        simulate_live_isp=diagnostic;
        fail_seq_disable=scenario==8;fail_seq_restore=scenario==9;
        fail_awb_force=scenario==10;fail_awb_restore=scenario==11;
        if(scenario==13)fail_refresh_at=1;
        if(scenario==19)fail_refresh_at=2;
        if(scenario==20)refresh_low_bits_change=true;
        if(scenario==23)fail_manual_disable=true;
        if(scenario==24)drift_at_receive=49;
        if(scenario==25)isp_settle_at=49;
        if(scenario==26)fail_refresh_at=2;
        if(scenario==14)refresh_never_completes=true;
        if(scenario==15)fail_preview_enable=true;
        if(scenario==16)values[0xa104]=33;
        if(scenario==17 || scenario==18){values[0xa102]=10;isp_settle_at=49;}
        p.automation_fps=5;p.frame_log_flush=true;p.raw=true;auto_response=true;
        assert(ram_begin_sweep());
        FILE **streams[]={&p.calls,&p.reads,&p.frames,&p.meta};
        const char *names[]={"calls.csv","reads.csv","frames.csv","manifest.txt"};
        for(unsigned k=0;k<4;++k) {
            FILE *previous=*streams[k];rewind(previous);
            *streams[k]=probe_fopen(names[k],"w");assert(*streams[k]);int ch;
            while((ch=fgetc(previous))!=EOF)assert(fputc(ch,*streams[k])!=EOF);
            fclose(previous);
        }
        values[0x302a]=8;values[0x300c]=842;values[0x3014]=633;
        for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
        for(unsigned i=0;i<4;++i){values[auto_isp_addresses[i]]=160;values[0x3032+2*i]=256;}
        assert(!write_reg(&p,0x304a,1,"forbidden"));
        assert(!write_reg(&p,0x32d4,513,"forbidden_isp"));
        if(fault==1)isp_drift_at=22;
        if(fault==2)fail_write_at=6;
        if(fault==3)fail_write_at=1;
        snprintf(p.directory,sizeof p.directory,"build/exposure-sweep-v2-%u",scenario);mkdir(p.directory,0777);
        bool ok=calibration_probe(&p);assert(ok==((fault==0 && scenario<8) || scenario==12 || scenario==20 || scenario==21));
        assert(values[0xa102]==(scenario==9 || scenario==17 || scenario==18?10:15));
        assert(values[0xa355]==(scenario==11?33:scenario==20?25:1));
        assert(values[0xa11f]==0 && refresh_commands<=2);
        if(scenario==12)assert(refresh_commands==2);
        assert(values[0x3012]==100 && values[0x3056]==0x220);
        for(unsigned i=0;i<4;++i)assert(values[auto_isp_addresses[i]]==160);
        if(ok)assert(p.successful_samples==(diagnostic?5:SWEEP_STAGES) && p.phase==(diagnostic?7:SWEEP_STAGES+2));
        else assert(p.successful_samples<SWEEP_STAGES);
        if(ok) {
            fflush(p.reads);unsigned traced=0;
            const char *needle=",isp_transition,";size_t length=strlen(needle);
            for(unsigned k=0;k<ram_log.count;++k)if(!strcmp(ram_log.files[k].name,"reads.csv")) {
                RamLogFile *f=&ram_log.files[k];
                for(size_t j=0;j+length<=f->size;++j)if(!memcmp(f->data+j,needle,length))++traced;
            }
            assert(traced==(diagnostic?160:0));
            unsigned mcu_traced=0;const char *mcu_needle=",isp_mcu_transition,";
            size_t mcu_length=strlen(mcu_needle);
            for(unsigned k=0;k<ram_log.count;++k)if(!strcmp(ram_log.files[k].name,"reads.csv")) {
                RamLogFile *f=&ram_log.files[k];
                for(size_t j=0;j+mcu_length<=f->size;++j)if(!memcmp(f->data+j,mcu_needle,mcu_length))++mcu_traced;
            }
            assert(mcu_traced==(diagnostic?320:0));
            unsigned exposure_traced=0;const char *exposure_needle=",exposure_transition,";
            for(unsigned k=0;k<ram_log.count;++k)if(!strcmp(ram_log.files[k].name,"reads.csv")) {
                RamLogFile *f=&ram_log.files[k];size_t n=strlen(exposure_needle);
                for(size_t j=0;j+n<=f->size;++j)if(!memcmp(f->data+j,exposure_needle,n))++exposure_traced;
            }
            assert(exposure_traced==(manual?40:0));
        }
        flush_logs(&p);cleanup(&p);assert(ram_freeze());
        for(unsigned k=0;k<ram_log.count;++k) {
            RamLogFile *file=&ram_log.files[k];char path[256];
            snprintf(path,sizeof path,"%s/%s",p.directory,file->name);
            FILE *out=fopen(path,"wb");assert(out);
            assert(fwrite(file->data,1,file->size,out)==file->size && !fclose(out));
        }
        assert(ram_log.used<=RAM_AUTO_BYTES && ram_log.count<=RAM_SWEEP_FILES);
        ram_release();
    }
    Probe p=setup();assert(!write_reg(&p,0x32d4,128,"wrong_scope"));
    assert(!write_reg(&p,0xa102,10,"wrong_scope"));
    assert(!write_reg(&p,0xa355,33,"wrong_scope"));cleanup(&p);
    p=setup();p.calibration_part=CAL_EXPOSURE_SWEEP_CHECK;p.phase=5;
    unsigned before_writes=writes;
    deny_read=true;
    assert(sweep_transition(&p,"outside_frame",8,"before_arm"));
    assert(!p.control_drift);
    assert(!sweep_transition(&p,"failed_read",0,"before_arm"));
    assert(p.control_drift && writes==before_writes);
    cleanup(&p);
    p=setup();p.calibration_part=CAL_EXPOSURE_SWEEP_CHECK;
    assert(!write_reg(&p,0xa103,5,"wrong_scope"));assert(!write_reg(&p,0xa11f,1,"wrong_scope"));cleanup(&p);
    p=setup();p.calibration_part=CAL_EXPOSURE_SWEEP_REFRESH;p.sweep_refresh_active=true;
    assert(!write_reg(&p,0xa103,6,"unregistered_command"));
    assert(!write_reg(&p,0xa11f,2,"unregistered_preview"));
    assert(write_reg(&p,0xa103,5,"apply"));assert(write_reg(&p,0xa103,5,"restore"));
    assert(!write_reg(&p,0xa103,5,"no_third_command"));cleanup(&p);
    p=setup();p.calibration_part=CAL_EXPOSURE_SWEEP_MANUAL;p.sweep_refresh_active=true;p.sweep_manual_locked=true;
    assert(!write_reg(&p,0xa103,5,"no_refresh_after_final_lock"));assert(refresh_commands==0);cleanup(&p);
    p=setup();p.calibration_part=CAL_EXPOSURE_SWEEP_CHECK;
    p.sweep_seq_gate=true;p.sweep_seq_saved=15;p.sweep_seq_target=10;
    assert(!write_reg(&p,0xa102,14,"unregistered_value"));
    assert(!write_reg(&p,0xa102,256,"invalid_byte"));
    snapshot(&p,"live_gate_mismatch",8,"before");assert(p.control_drift);
    assert(write_reg(&p,0xa102,10,"gate"));
    assert(write_reg(&p,0xa102,15,"restore"));assert(writes==2);
    cleanup(&p);
    puts("PASS sweep exact coverage, ISP scope, drift/write failure and restoration");
}
static void isp_update_contract_tests(void)
{
    for(unsigned scenario=0;scenario<9;++scenario) {
        Probe p=setup();p.one_shot=true;p.phase=2;p.calibration_part=CAL_EXPOSURE_ISP_UPDATE;
        p.automation_fps=5;p.frame_log_flush=true;p.raw=true;auto_response=true;
        refresh_unit_awb=scenario!=8;update_pattern=scenario;update_probe_active=&p;
        if(scenario==5)fail_refresh_at=1;
        if(scenario==6)fail_refresh_at=5;
        if(scenario==7)update_fail_mode=true;
        assert(ram_begin_sweep());
        FILE **streams[]={&p.calls,&p.reads,&p.frames,&p.meta};
        const char *names[]={"calls.csv","reads.csv","frames.csv","manifest.txt"};
        for(unsigned k=0;k<4;++k) {
            FILE *old=*streams[k];rewind(old);*streams[k]=probe_fopen(names[k],"w");assert(*streams[k]);
            int ch;while((ch=fgetc(old))!=EOF)assert(fputc(ch,*streams[k])!=EOF);fclose(old);
        }
        values[0x302a]=8;values[0x300c]=842;values[0x3014]=633;
        for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
        for(unsigned i=0;i<4;++i){values[auto_isp_addresses[i]]=160;values[0x3032+2*i]=256;}
        snprintf(p.directory,sizeof p.directory,"build/isp-update-%u",scenario);mkdir(p.directory,0777);
        bool ok=calibration_probe(&p);assert(ok==(scenario<3));
        assert(values[0xa102]==15 && values[0xa355]==1 && !values[0xa11f]);
        assert(values[0x3012]==100 && values[0x3056]==0x220);
        for(unsigned i=0;i<4;++i)assert(values[auto_isp_addresses[i]]==160);
        assert(refresh_commands<=5 && closed_events==receives && !p.streaming);
        if(ok)assert(p.successful_samples==6 && p.phase==8 && refresh_commands==5);
        flush_logs(&p);cleanup(&p);assert(ram_freeze());
        for(unsigned k=0;k<ram_log.count;++k) {
            RamLogFile *file=&ram_log.files[k];char path[256];snprintf(path,sizeof path,"%s/%s",p.directory,file->name);
            FILE *out=fopen(path,"wb");assert(out);assert(fwrite(file->data,1,file->size,out)==file->size && !fclose(out));
        }
        ram_release();update_probe_active=NULL;
    }
    Probe p=setup();assert(!isp_update_probe(&p));cleanup(&p);
    puts("PASS ISP update planned rewrites, repeated/once/held patterns, DMA timeout, control/mode failure and restoration");
}
static void no_receive_contract_tests(void)
{
    for(unsigned scenario=0;scenario<10;++scenario) {
        Probe p=setup();p.one_shot=true;p.phase=2;p.calibration_part=CAL_NO_RECEIVE_UPDATE;
        p.automation_fps=5;p.frame_log_flush=true;p.raw=true;auto_response=true;
        refresh_unit_awb=true;no_receive_pattern=scenario;no_receive_active=&p;
        if(scenario==9)fail_refresh_at=3;
        assert(ram_begin_sweep());
        FILE **streams[]={&p.calls,&p.reads,&p.frames,&p.meta};
        const char *names[]={"calls.csv","reads.csv","frames.csv","manifest.txt"};
        for(unsigned k=0;k<4;++k) {
            FILE *old=*streams[k];rewind(old);*streams[k]=probe_fopen(names[k],"w");assert(*streams[k]);
            int ch;while((ch=fgetc(old))!=EOF)assert(fputc(ch,*streams[k])!=EOF);fclose(old);
        }
        values[0x302a]=8;values[0x300c]=842;values[0x3014]=633;
        for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
        for(unsigned i=0;i<4;++i){values[auto_isp_addresses[i]]=160;values[0x3032+2*i]=256;}
        snprintf(p.directory,sizeof p.directory,"build/no-receive-%u",scenario);mkdir(p.directory,0777);
        bool ok=calibration_probe(&p);assert(ok==(scenario<3));
        assert(values[0xa102]==15 && values[0xa355]==1 && !values[0xa11f]);
        assert(values[0x3012]==100 && values[0x3056]==0x220);
        for(unsigned i=0;i<4;++i)assert(values[auto_isp_addresses[i]]==160);
        assert(refresh_commands==3 && closed_events==receives+vsync_closed && !p.streaming);
        assert(vsync_closed==(scenario==5?0:1));
        if(ok)assert(p.successful_samples==4 && p.phase==6);
        flush_logs(&p);cleanup(&p);assert(ram_freeze());
        for(unsigned k=0;k<ram_log.count;++k) {
            RamLogFile *file=&ram_log.files[k];char path[256];snprintf(path,sizeof path,"%s/%s",p.directory,file->name);
            FILE *out=fopen(path,"wb");assert(out);assert(fwrite(file->data,1,file->size,out)==file->size && !fclose(out));
        }
        ram_release();no_receive_active=NULL;
    }
    Probe p=setup();assert(!no_receive_probe(&p));cleanup(&p);
    puts("PASS no-receive VSYNC/DMA overwrite models, stale/timeout/event/timing/clear/control/restore failures");
}
static void rebuild_contract_tests(bool dense)
{
    simulated_clock=UINT64_C(4003700000000);
    for(unsigned scenario=0;scenario<(dense?12:16);++scenario) {
        Probe p=setup();p.one_shot=true;p.phase=2;p.calibration_part=dense?CAL_EXPOSURE_DENSE:CAL_EXPOSURE_REBUILD;
        p.automation_fps=5;p.frame_log_flush=true;p.raw=true;auto_response=true;refresh_unit_awb=true;
        rebuild_active=&p;const unsigned cases[]={0,9,10,11,3,4,5,6,7,8,12,2};rebuild_pattern=dense?cases[scenario]:scenario;gain_matrix_active=&p;gain_matrix_pattern=1;
        values[0x2703]=640;values[0x2332]=410;values[0x233e]=65535;reject_ffff_read=true;
        assert(ram_begin_sweep());
        FILE **streams[]={&p.calls,&p.reads,&p.frames,&p.meta};const char *names[]={"calls.csv","reads.csv","frames.csv","manifest.txt"};
        for(unsigned i=0;i<4;++i){FILE *old=*streams[i];rewind(old);*streams[i]=probe_fopen(names[i],"w");assert(*streams[i]);int ch;while((ch=fgetc(old))!=EOF)assert(fputc(ch,*streams[i])!=EOF);fclose(old);}
        values[0x302a]=8;values[0x300c]=842;values[0x3014]=633;
        for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
        for(unsigned i=0;i<4;++i){values[auto_isp_addresses[i]]=160;values[0x3032+2*i]=256;}
        snprintf(p.directory,sizeof p.directory,dense?"build/exposure-dense-%u":"build/exposure-rebuild-v2-%u",scenario);mkdir(p.directory,0777);
        boot_host_file=tmpfile();assert(boot_host_file);boot_host_heap=64*1024*1024;boot_host_linear=32*1024*1024;
        boot_journal.active=true;boot_journal.record_limit=1024;boot_journal.run_ms=osGetTime();
        for(unsigned i=1;i<=12;++i)assert(boot_mark(i,i==3?CAL_EXPOSURE_REBUILD:0,0));
        u8 *original=p.pixels;bool ok=calibration_probe(&p);bool expected=dense?scenario<4:(scenario==0 || scenario==1 || scenario==9 || scenario==10 || scenario==11 || scenario>=13);
        if(ok!=expected)fprintf(stderr,"rebuild scenario=%u ok=%u phase=%u waits=%u files=%u used=%lu\n",scenario,ok,p.phase,rebuild_waits,ram_log.count,(unsigned long)ram_log.used);
        assert(ok==expected && p.pixels==original && !p.streaming);
        assert(values[0xa102]==15 && values[0xa355]==1 && !values[0xa11f]);
        assert(values[0x3012]==100 && values[0x3056]==0x220);
        assert(values[0x300a]==(rebuild_pattern==8?704:2114));assert(closed_events==receives && refresh_commands<=3);
        if(ok)assert(p.successful_samples==76 && p.phase==78 && refresh_commands==3 && rate_sets==3 && rebuild_waits==(dense?444:748) && ram_log.count<=313);
        assert(boot_mark(19,1,0) && boot_mark(20,1,0) && boot_mark(21,p.phase,ok?0:(Result)-1) && boot_mark(22,0,0));assert(boot_copy());boot_journal.active=false;fclose(boot_host_file);boot_host_file=NULL;
        flush_logs(&p);cleanup(&p);assert(ram_freeze());
        for(unsigned k=0;k<ram_log.count;++k){RamLogFile *f=&ram_log.files[k];char path[256];snprintf(path,sizeof path,"%s/%s",p.directory,f->name);FILE *out=fopen(path,"wb");assert(out);assert(fwrite(f->data,1,f->size,out)==f->size && !fclose(out));}
        ram_release();rebuild_active=NULL;gain_matrix_active=NULL;
    }
    Probe p=setup();assert(!exposure_rebuild_probe(&p));cleanup(&p);puts("PASS exposure rebuild adaptive images, ping-pong ownership, FD ABA, hold and control/transport/restore/allocation failures");
}
static void gain_matrix_contract_tests(void)
{
    simulated_clock=UINT64_C(4003700000000);
    for(unsigned scenario=0;scenario<13;++scenario) {
        Probe p=setup();p.one_shot=true;p.phase=2;p.calibration_part=CAL_GAIN_MATRIX;
        p.automation_fps=5;p.frame_log_flush=true;p.raw=true;auto_response=true;refresh_unit_awb=true;
        gain_matrix_active=&p;gain_matrix_pattern=scenario;
        values[0x2703]=640;values[0x2332]=410;values[0x233e]=65535;reject_ffff_read=true;
        assert(ram_begin_sweep());
        FILE **streams[]={&p.calls,&p.reads,&p.frames,&p.meta};const char *names[]={"calls.csv","reads.csv","frames.csv","manifest.txt"};
        for(unsigned i=0;i<4;++i){FILE *old=*streams[i];rewind(old);*streams[i]=probe_fopen(names[i],"w");assert(*streams[i]);int ch;while((ch=fgetc(old))!=EOF)assert(fputc(ch,*streams[i])!=EOF);fclose(old);}
        values[0x302a]=8;values[0x300c]=842;values[0x3014]=633;
        for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
        for(unsigned i=0;i<4;++i){values[auto_isp_addresses[i]]=160;values[0x3032+2*i]=256;}
        snprintf(p.directory,sizeof p.directory,"build/gain-matrix-v4-%u",scenario);mkdir(p.directory,0777);
        boot_host_file=tmpfile();assert(boot_host_file);boot_host_heap=64*1024*1024;boot_host_linear=32*1024*1024;
        boot_journal.active=true;boot_journal.record_limit=1024;boot_journal.run_ms=osGetTime();
        for(unsigned i=1;i<=12;++i)assert(boot_mark(i,i==3?CAL_GAIN_MATRIX:0,0));
        bool ok=calibration_probe(&p);
        if(ok!=(scenario<3 || scenario==9 || scenario==11))fprintf(stderr,"matrix scenario=%u ok=%u phase=%u waits=%u\n",scenario,ok,p.phase,gain_matrix_waits);
        assert(ok==(scenario<3 || scenario==9 || scenario==11));
        assert(values[0xa102]==15 && values[0xa355]==1 && !values[0xa11f]);
        assert(values[0x3012]==100 && values[0x3056]==0x220 && !p.streaming);
        assert(values[0x300a]==(scenario==8?704:2114));
        assert(closed_events==receives+vsync_closed && refresh_commands<=9);
        if(ok)assert(p.successful_samples==10 && p.phase==12 && refresh_commands==9 && rate_sets==3 && vsync_closed==8);
        assert(boot_mark(19,1,0) && boot_mark(20,1,0) && boot_mark(21,p.phase,ok?0:(Result)-1) && boot_mark(22,0,0));
        assert(boot_copy());boot_journal.active=false;fclose(boot_host_file);boot_host_file=NULL;
        flush_logs(&p);cleanup(&p);assert(ram_freeze());
        for(unsigned k=0;k<ram_log.count;++k){RamLogFile *f=&ram_log.files[k];char path[256];snprintf(path,sizeof path,"%s/%s",p.directory,f->name);FILE *out=fopen(path,"wb");assert(out);assert(fwrite(f->data,1,f->size,out)==f->size && !fclose(out));}
        ram_release();gain_matrix_active=NULL;
    }
    Probe p=setup();assert(!gain_matrix_probe(&p));cleanup(&p);
    puts("PASS bounded matrix adaptive branches, profile/control/VSYNC/byte/DMA/restore failures");
}
static bool boot_reject_sink(const char *p,size_t n,void *c) {(void)p;(void)n;(*(unsigned *)c)++;return false;}
static void boot_contract_tests(void)
{
    unsigned calls=0;BootJournal j={0};
    assert(boot_emit(&j,1,0,0,1,0,0,boot_reject_sink,&calls) && !calls);
    j.active=true;assert(!boot_emit(&j,1,0,0,1,0,0,boot_reject_sink,&calls) && calls==1 && j.failed && !j.count);
    assert(!boot_emit(&j,1,0,0,1,0,0,boot_reject_sink,&calls) && calls==1);
    for(unsigned kind=0;kind<4;++kind) {
        j=(BootJournal){.active=true,.offset=kind==0?BOOT_TOTAL_CAP+1:kind==1?BOOT_TOTAL_CAP-1:0,.count=kind==2?BOOT_RECORD_CAP:0};calls=0;
        assert(!boot_emit(&j,kind==3?99:1,0,0,1,0,0,boot_reject_sink,&calls) && !calls && j.failed);
    }
    boot_host_file=tmpfile();assert(boot_host_file);fputs("preserved-prefix\n",boot_host_file);fflush(boot_host_file);
    j=(BootJournal){.active=true,.run_ms=123,.offset=17,.run_offset=17};
    assert(boot_emit(&j,1,0,0,456,32,16,boot_sink,&j));rewind(boot_host_file);
    char line[200];assert(fgets(line,sizeof line,boot_host_file) && !strcmp(line,"preserved-prefix\n"));
    assert(fgets(line,sizeof line,boot_host_file) && strstr(line,"run=123 seq=0 stage=1"));fclose(boot_host_file);boot_host_file=NULL;
    char *v3[]={"probe","--calibration-part=camera-ccm-normalization-v3"};assert(boot_requested(2,v3));v3[1]="--calibration-part=camera-ccm-normalization-v2";assert(!boot_requested(2,v3));
    puts("PASS boot journal inactive source, append preservation, sink failure, caps, invalid stage and exact selection");
}
static void hg_contract_tests(bool ccm,bool bytes,bool journal)
{
    /* Exercise the bounded manifest with actual 13-digit device clock width. */
    simulated_clock=UINT64_C(4003600000000);
    for(unsigned scenario=0;scenario<(bytes?15u:ccm?12u:8u);++scenario) {
        Probe p=setup();p.one_shot=true;p.phase=2;p.calibration_part=journal?CAL_CCM_BOOT:bytes?CAL_CCM_BYTES:ccm?CAL_CCM_NORMALIZATION:CAL_HG_ISOLATION;
        p.automation_fps=5;p.frame_log_flush=true;p.raw=true;auto_response=true;
        refresh_unit_awb=true;hg_active=&p;hg_pattern=scenario;
        if(scenario==6)fail_refresh_at=4;
        if(bytes){values[0x2332]=410;values[0x233a]=376;values[0x233e]=65535;reject_ffff_read=true;}
        assert(ram_begin_sweep());
        FILE **streams[]={&p.calls,&p.reads,&p.frames,&p.meta};
        const char *names[]={"calls.csv","reads.csv","frames.csv","manifest.txt"};
        for(unsigned k=0;k<4;++k) {
            FILE *old=*streams[k];rewind(old);*streams[k]=probe_fopen(names[k],"w");assert(*streams[k]);
            int ch;while((ch=fgetc(old))!=EOF)assert(fputc(ch,*streams[k])!=EOF);fclose(old);
        }
        values[0x302a]=8;values[0x300c]=842;values[0x3014]=633;
        for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
        for(unsigned i=0;i<4;++i){values[auto_isp_addresses[i]]=160;values[0x3032+2*i]=256;}
        snprintf(p.directory,sizeof p.directory,journal?"build/ccm-boot-%u":bytes?"build/ccm-bytes-%u":ccm?"build/ccm-normalization-%u":"build/hg-isolation-%u",scenario);mkdir(p.directory,0777);
        if(journal) {
            boot_host_file=tmpfile();assert(boot_host_file);boot_host_heap=64*1024*1024;boot_host_linear=32*1024*1024;
            boot_journal.active=true;boot_journal.run_ms=osGetTime();
            for(unsigned stage=1;stage<=12;++stage)assert(boot_mark(stage,stage==3?CAL_CCM_BOOT:stage==5?RAM_AUTO_BYTES:stage==7 || stage==8?BYTES:stage>=9?1:0,0));
        }
        bool expected=scenario<3 || (ccm && scenario==10);
        bool ok=calibration_probe(&p);if(ok!=expected){fprintf(stderr,"HG scenario=%u unexpected ok=%u phase=%u\n",scenario,ok,p.phase);fflush(p.meta);for(unsigned f=0;f<ram_log.count;++f)if(!strcmp(ram_log.files[f].name,"manifest.txt"))fwrite(ram_log.files[f].data,1,ram_log.files[f].size,stderr);}assert(ok==expected);
        if(journal) {
            assert(boot_mark(19,1,0) && boot_mark(20,1,0) && boot_mark(21,p.phase,ok?0:(Result)-1) && boot_mark(22,0,0));
            assert(boot_copy());boot_journal.active=false;fclose(boot_host_file);boot_host_file=NULL;
        }
        assert(values[0xa102]==15 && values[0xa355]==1 && !values[0xa11f]);
        assert(values[0x3012]==100 && values[0x3056]==0x220);
        for(unsigned i=0;i<4;++i)assert(values[auto_isp_addresses[i]]==160);
        assert(refresh_commands<=4 && closed_events==receives+vsync_closed && !p.streaming);
        if(ok)assert(p.successful_samples==5 && p.phase==7 && refresh_commands==4 && vsync_closed==3);
        flush_logs(&p);cleanup(&p);assert(ram_freeze());
        for(unsigned k=0;k<ram_log.count;++k) {
            RamLogFile *file=&ram_log.files[k];char path[256];snprintf(path,sizeof path,"%s/%s",p.directory,file->name);
            FILE *out=fopen(path,"wb");assert(out);assert(fwrite(file->data,1,file->size,out)==file->size && !fclose(out));
        }
        ram_release();hg_active=NULL;
    }
    Probe p=setup();assert(!hg_isolation_probe(&p));assert(!ccm_normalization_probe(&p));assert(!ccm_bytes_probe(&p));assert(!hg_rewrite(&p,"wrong_scope",0));cleanup(&p);
    puts("PASS HG on/off/on, independent overwrite/held models, snapshot/gate/VSYNC/control/restore failures and wrong scope");
}
static void writer_window_contract_tests(void)
{
    signal(SIGPIPE,SIG_IGN);
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    assert(send(pair[0],"BAD",3,0)==3);assert(!writer_word(pair[1],"GO\n",false,10));
    assert(!writer_word(pair[1],"GO\n",false,5));close(pair[0]);
    assert(!writer_word(pair[1],"GO\n",false,5));close(pair[1]);
    for(unsigned scenario=0;scenario<5;++scenario) {
        Probe p=setup();p.one_shot=true;p.phase=2;p.calibration_part=scenario==4?CAL_CAMERA_WRITER_OBSERVE:CAL_CAMERA_WRITER_WINDOW;
        p.automation_fps=5;p.frame_log_flush=true;p.raw=true;auto_response=true;
        refresh_unit_awb=true;no_receive_active=&p;simulate_debug_read_delay=scenario>=3;
        assert(ram_begin_sweep());
        FILE **streams[]={&p.calls,&p.reads,&p.frames,&p.meta};
        const char *names[]={"calls.csv","reads.csv","frames.csv","manifest.txt"};
        for(unsigned k=0;k<4;++k) {
            FILE *old=*streams[k];rewind(old);*streams[k]=probe_fopen(names[k],"w");assert(*streams[k]);
            int ch;while((ch=fgetc(old))!=EOF)assert(fputc(ch,*streams[k])!=EOF);fclose(old);
        }
        values[0x302a]=8;values[0x300c]=842;values[0x3014]=633;
        for(unsigned i=1;i<5;++i)values[cal_addresses[i]]=0x220;
        for(unsigned i=0;i<4;++i){values[auto_isp_addresses[i]]=160;values[0x3032+2*i]=256;}
        snprintf(p.directory,sizeof p.directory,"build/writer-window-%u",scenario);mkdir(p.directory,0777);
        pid_t child=fork();assert(child>=0);
        if(!child) {
            int fd=-1;
            for(unsigned i=0;i<500;++i) {
                fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
                struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_port=htons(WRITER_GATE_PORT);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
                if(!connect(fd,(struct sockaddr *)&addr,sizeof addr))break;
                close(fd);fd=-1;usleep(10000);
            }
            assert(fd>=0 && writer_word(fd,"FILMTRACE1 READY\n",false,1000));
            assert(writer_word(fd,scenario==1?"NO\n":"GO\n",true,1000));
            if(scenario!=1) {
                assert(writer_word(fd,scenario==3?"FILMTRACE1 DONE ok=0\n":"FILMTRACE1 DONE ok=1\n",false,1000));
                if(scenario!=2)assert(writer_word(fd,"RELEASE\n",true,1000));
            }
            close(fd);_exit(0);
        }
        bool ok=calibration_probe(&p);assert(ok==(scenario==0 || scenario==4));
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
        assert(values[0xa102]==15 && values[0xa355]==1 && !values[0xa11f]);
        assert(values[0x3012]==100 && refresh_commands==2 && !p.streaming);
        assert(closed_events==receives+vsync_closed);
        if(ok)assert(p.successful_samples==3 && p.phase==5 && vsync_closed==1);
        flush_logs(&p);cleanup(&p);assert(ram_freeze());
        for(unsigned k=0;k<ram_log.count;++k) {
            RamLogFile *file=&ram_log.files[k];char path[256];snprintf(path,sizeof path,"%s/%s",p.directory,file->name);
            FILE *out=fopen(path,"wb");assert(out);assert(fwrite(file->data,1,file->size,out)==file->size && !fclose(out));
        }
        ram_release();no_receive_active=NULL;
    }
    puts("PASS writer gate actual sockets, malformed/disconnected/timeout input, one GO window and restoration");
}
int main(void)
{
    if(getenv("EXPOSURE_DENSE_ONLY")){rebuild_contract_tests(true);return 0;}
    if(getenv("EXPOSURE_REBUILD_ONLY")){rebuild_contract_tests(false);return 0;}
    if(getenv("GAIN_MATRIX_ONLY")){gain_matrix_contract_tests();return 0;}
    if(getenv("CCM_BOOT_ONLY")){boot_contract_tests();hg_contract_tests(true,true,true);return 0;}
    if(getenv("CCM_BYTES_ONLY")){hg_contract_tests(true,true,false);return 0;}
    if(getenv("CCM_NORMALIZATION_ONLY")){hg_contract_tests(true,false,false);return 0;}
    if(getenv("HG_ISOLATION_ONLY")){hg_contract_tests(false,false,false);return 0;}
    if(getenv("WRITER_WINDOW_ONLY")){writer_window_contract_tests();return 0;}
    if(getenv("NO_RECEIVE_ONLY")){no_receive_contract_tests();return 0;}
    if(getenv("ISP_UPDATE_ONLY")){isp_update_contract_tests();return 0;}
    if(getenv("EXPOSURE_SWEEP_ONLY")){sweep_contract_tests();return 0;}
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
