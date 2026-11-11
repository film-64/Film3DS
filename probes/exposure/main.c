/* Standalone real-hardware experiment: intentionally bypass film_camera_open(),
 * which rejects the device if its preferred AE initialization fails.
 * Register map: GBATEK, DSi Aptina Camera CORE/SYSCTL/AE sections.
 * Never write OTP, PLL, reset, reserved registers, or arbitrary user addresses.
 */
#include <3ds.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef PROBE_REVISION
#define PROBE_REVISION "unknown"
#endif
enum { W = 640, H = 480, BYTES = W * H * 2, SETTLE = 8, SAMPLES = 12 };
typedef struct { u16 addr; const char *name; bool mcu; } Register;
static const Register registers[] = {
    {0x0000,"chip_id",false}, {0x0010,"pll_dividers",false},
    {0x0012,"pll_p_dividers",false}, {0x0014,"pll_control",false},
    {0x300a,"frame_lines",false}, {0x300c,"line_pck",false},
    {0x3012,"coarse",false}, {0x3014,"fine",false}, {0x3016,"row_speed",false},
    {0x3028,"analog_global_code",false}, {0x302a,"analog_gr_code",false},
    {0x302c,"analog_r_code",false}, {0x302e,"analog_b_code",false},
    {0x3030,"analog_gb_code",false}, {0x3032,"digital_gr",false},
    {0x3034,"digital_r",false}, {0x3036,"digital_b",false}, {0x3038,"digital_gb",false},
    {0x3056,"gain_gr",false}, {0x3058,"gain_b",false},
    {0x305a,"gain_r",false}, {0x305c,"gain_gb",false},
    {0x32d4,"ifp_gain_r",false}, {0x32d6,"ifp_gain_gr",false},
    {0x32d8,"ifp_gain_gb",false}, {0x32da,"ifp_gain_b",false},
    /* bit15 marks byte access in MCU logical addresses; do not lose it. */
    {0xa206,"ae_target",true}, {0xa20b,"ae_min_index",true},
    {0xa20c,"ae_max_index",true}, {0xa20d,"ae_min_virtgain",true},
    {0xa20e,"ae_max_virtgain",true}, {0xa217,"ae_status",true},
    {0xa218,"ae_current_y",true}, {0x2219,"ae_shutter_delay",true},
    {0xa21b,"ae_index",true}, {0xa21c,"ae_virtgain",true},
    {0x221f,"ae_dgain",true}, {0x2222,"ae_integration",true},
    {0x222d,"ae_integration_step",true}, {0xa24f,"ae_base_target",true},
    {0xa11d,"seq_preview_ae",true}, {0xa11f,"seq_preview_awb",true}
};
typedef struct {
    FILE *calls, *reads, *frames, *meta;
    char directory[160];
    unsigned sensor, phase, transfer;
    u32 select, port;
    u8 *pixels;
    bool aborted, io_error, map_known, capture_ready, driver, streaming, raw, phase_success;
    unsigned captures, failures;
} Probe;

static void flush_logs(Probe *p)
{
    FILE *files[] = {p->calls,p->reads,p->frames,p->meta};
    for (unsigned i=0; i<sizeof files/sizeof *files; ++i)
        if (files[i] && (fflush(files[i]) || ferror(files[i]))) p->io_error = true;
}

static bool running(Probe *p)
{
    if (p->aborted || p->io_error) return false;
    hidScanInput();
    if (!aptMainLoop() || (hidKeysHeld() & KEY_B)) p->aborted = true;
    return !p->aborted;
}

static Result record_call(Probe *p, const char *label, const char *op,
                          unsigned addr, unsigned value, Result rc)
{
    fprintf(p->calls,"%u,%u,%s,%s,%04x,%u,%08" PRIx32 ",%" PRIu64 "\n",
            p->sensor,p->phase,label,op,addr,value,(uint32_t)rc,(uint64_t)osGetTime());
    flush_logs(p);
    return rc;
}

static Result read_reg(Probe *p, bool mcu, u16 addr, u16 *value,
                        const char *label, int frame, const char *when, const char *name)
{
    *value = 0; /* failed reads are logged with a result, never treated as zero measurements */
    Result rc = mcu ? CAMU_ReadMcuVariableI2cExclusive(value,p->select,addr)
                    : CAMU_ReadRegisterI2cExclusive(value,p->select,addr);
    fprintf(p->reads,"%u,%u,%s,%d,%s,%s,%s,%04x,%u,%08" PRIx32 ",%" PRIu64 "\n",
            p->sensor,p->phase,label,frame,when,mcu?"mcu":"reg",name,addr,*value,
            (uint32_t)rc,(uint64_t)osGetTime());
    return rc;
}

static void snapshot(Probe *p, const char *label, int frame, const char *when)
{
    for (unsigned i=0; i<sizeof registers/sizeof *registers && running(p); ++i) {
        const Register *r=&registers[i];
        if (!p->map_known && r->addr != 0) continue;
        u16 value;
        read_reg(p,r->mcu,r->addr,&value,label,frame,when,r->name);
    }
    bool enabled=false;
    Result rc=CAMU_IsAutoExposure(&enabled,p->select);
    record_call(p,label,"IsAutoExposure",0,enabled,rc);
    enabled=false;
    rc=CAMU_IsAutoWhiteBalance(&enabled,p->select);
    record_call(p,label,"IsAutoWhiteBalance",0,enabled,rc);
    flush_logs(p);
}

static void stop(Probe *p, const char *label)
{
    Result stopped=record_call(p,label,"StopCapture",0,0,CAMU_StopCapture(p->port));
    Result cleared=record_call(p,label,"ClearBuffer",0,0,CAMU_ClearBuffer(p->port));
    p->streaming=R_FAILED(stopped);
    /* Do not reuse a DMA buffer after an unsuccessful stop/clear. The remaining
     * interface queries can continue, then close_camera finalizes the driver. */
    if (R_FAILED(stopped) || R_FAILED(cleared)) p->capture_ready=false;
}

static uint32_t crc32(const u8 *bytes, unsigned count)
{
    uint32_t crc=UINT32_MAX;
    for (unsigned i=0; i<count; ++i) {
        crc ^= bytes[i];
        for (unsigned b=0; b<8; ++b) crc=(crc>>1) ^ (0xedb88320u & (0u-(crc&1)));
    }
    return ~crc;
}

static bool save_frame(Probe *p, const char *label)
{
    char path[240];
    snprintf(path,sizeof path,"%s/s%u-p%03u-%s.%s",p->directory,p->sensor,p->phase,label,
             p->raw?"yuyv":"pgm");
    FILE *f=fopen(path,"wb");
    if (!f) { p->io_error=true; return false; }
    if (p->raw) {
        if (fwrite(p->pixels,1,BYTES,f)!=BYTES) p->io_error=true;
    } else {
        /* Full-resolution luma, exact code values; no gamma or LUT. */
        fprintf(f,"P5\n640 480\n255\n");
        u8 row[W];
        for (unsigned y=0; y<H; ++y) {
            for (unsigned x=0; x<W; ++x) row[x]=p->pixels[(y*W+x)*2];
            if (fwrite(row,1,W,f)!=W) { p->io_error=true; break; }
        }
    }
    if (fclose(f)) p->io_error=true;
    return !p->io_error;
}

static bool capture(Probe *p, const char *label, int frame, bool measured, bool save)
{
    if (!p->capture_ready || !running(p)) return false;
    if (measured) snapshot(p,label,frame,"before");
    if (!running(p)) return false;
    Handle event=0;
    u64 start=osGetTime();
    Result rc=GSPGPU_FlushDataCache(p->pixels,BYTES);
    record_call(p,label,"FlushDataCache",0,0,rc);
    if (R_SUCCEEDED(rc)) {
        rc=CAMU_SetReceiving(&event,p->pixels,p->port,BYTES,(s16)p->transfer);
        record_call(p,label,"SetReceiving",0,BYTES,rc);
    }
    if (R_SUCCEEDED(rc) && !p->streaming) {
        rc=CAMU_StartCapture(p->port);
        record_call(p,label,"StartCapture",0,0,rc);
        p->streaming=R_SUCCEEDED(rc);
    }
    if (R_SUCCEEDED(rc)) rc=svcWaitSynchronization(event,INT64_C(2000000000));
    bool received=false;
    if (R_SUCCEEDED(rc)) {
        rc=CAMU_IsFinishedReceiving(&received,p->port);
        record_call(p,label,"IsFinishedReceiving",0,received,rc);
    }
    if (R_SUCCEEDED(rc) && received) {
        rc=GSPGPU_InvalidateDataCache(p->pixels,BYTES);
        record_call(p,label,"InvalidateDataCache",0,0,rc);
    }
    bool ok=R_SUCCEEDED(rc) && received;
    if (!ok) { ++p->failures; stop(p,label); }
    if (event) svcCloseHandle(event);
    unsigned hist[256]={0};
    uint64_t sum=0;
    unsigned p50=0, count=0;
    if (ok) {
        ++p->captures;
        for (unsigned i=0; i<BYTES; i+=2) { ++hist[p->pixels[i]]; sum+=p->pixels[i]; }
        for (unsigned i=0; i<256; ++i) {
            count+=hist[i];
            if (count >= W*H/2) { p50=i; break; }
        }
    }
    fprintf(p->frames,"%u,%u,%s,%d,%u,%u,%08" PRIx32 ",%u,%.6f,%u,%u,%u,%08" PRIx32 ",%" PRIu64 ",%" PRIu64 "\n",
            p->sensor,p->phase,label,frame,measured,ok,(uint32_t)rc,received,
            ok?sum/(double)(W*H):0,p50,hist[0]+hist[16],hist[235]+hist[255],
            ok?crc32(p->pixels,BYTES):0,(uint64_t)start,(uint64_t)osGetTime());
    if (ok && measured) snapshot(p,label,frame,"after");
    if (ok && save) save_frame(p,label);
    flush_logs(p);
    return ok && running(p);
}

static bool sample(Probe *p, const char *label)
{
    p->phase_success=false;
    printf("\x1b[12;0HSensor %u phase %u: %-28s\nB: stop after current operation\n",p->sensor,p->phase,label);
    snapshot(p,label,-1,"immediate");
    if (!p->capture_ready) return running(p);
    for (int i=0; i<SETTLE+SAMPLES && running(p); ++i)
        if (!capture(p,label,i,i>=SETTLE,i==SETTLE+SAMPLES-1)) {
            fprintf(p->meta,"SAMPLE_FAILED sensor=%u phase=%u label=%s\n",p->sensor,p->phase,label);
            /* Failed imaging must not hide the remaining API capabilities. */
            return running(p);
        }
    p->phase_success=true;
    return running(p);
}

static void close_camera(Probe *p)
{
    if (!p->driver) return;
    stop(p,"cleanup");
    record_call(p,"cleanup","ActivateNone",0,0,CAMU_Activate(SELECT_NONE));
    camExit();
    p->driver=p->capture_ready=false;
}

static bool open_camera(Probe *p, unsigned sensor)
{
    static const u32 selects[]={SELECT_OUT1,SELECT_OUT2,SELECT_IN1};
    p->sensor=sensor; p->select=selects[sensor-1];
    p->port=sensor==2?PORT_CAM2:PORT_CAM1;
    p->map_known=p->capture_ready=p->streaming=false;
    Result rc=record_call(p,"open","camInit",0,0,camInit());
    if (R_FAILED(rc)) return false;
    p->driver=true;
    rc=record_call(p,"open","Activate",0,p->select,CAMU_Activate(p->select));
    if (R_FAILED(rc)) return false;
    u16 id;
    rc=read_reg(p,false,0,&id,"identity",-1,"initial","chip_id");
    p->map_known=R_SUCCEEDED(rc) && id==0x2280;
    /* Keep testing official interfaces even when low-level reads are denied. */
    bool setup=true;
    rc=record_call(p,"open","SetSizeVGA",0,0,CAMU_SetSize(p->select,SIZE_VGA,CONTEXT_A));
    setup &= R_SUCCEEDED(rc);
    rc=record_call(p,"open","SetOutputYUV",0,0,CAMU_SetOutputFormat(p->select,OUTPUT_YUV_422,CONTEXT_A));
    setup &= R_SUCCEEDED(rc);
    rc=record_call(p,"open","SwitchContextA",0,0,CAMU_SwitchContext(p->select,CONTEXT_A));
    setup &= R_SUCCEEDED(rc);
    rc=record_call(p,"open","SetTrimmingOff",0,0,CAMU_SetTrimming(p->port,false));
    setup &= R_SUCCEEDED(rc);
    u32 transfer=0;
    rc=record_call(p,"open","GetMaxBytes",0,0,CAMU_GetMaxBytes(&transfer,W,H));
    setup &= R_SUCCEEDED(rc) && transfer && transfer<=INT16_MAX && BYTES%transfer==0;
    if (setup) {
        rc=record_call(p,"open","SetTransferBytes",0,transfer,CAMU_SetTransferBytes(p->port,transfer,W,H));
        setup &= R_SUCCEEDED(rc);
    }
    p->transfer=transfer; p->capture_ready=setup;
    Y2RU_StandardCoefficient coefficient=0;
    rc=CAMU_GetSuitableY2rStandardCoefficient(&coefficient);
    record_call(p,"open","GetY2rCoefficient",0,coefficient,rc);
    u32 active=0;
    rc=CAMU_GetActivatedCamera(&active);
    record_call(p,"open","GetActivatedCamera",0,active,rc);
    u32 sleep_camera=0;
    rc=CAMU_GetSleepCamera(&sleep_camera);
    record_call(p,"open","GetSleepCamera",0,sleep_camera,rc);
    CAMU_ImageQualityCalibrationData quality={0};
    rc=CAMU_GetImageQualityCalibrationData(&quality);
    record_call(p,"open","GetImageQualityCalibrationData",0,(unsigned)(u16)quality.aeBaseTarget,rc);
    CAMU_StereoCameraCalibrationData stereo={0};
    rc=CAMU_GetStereoCameraCalibrationData(&stereo);
    record_call(p,"open","GetStereoCameraCalibrationData",0,0,rc);
    fprintf(p->meta,"STEREO sensor=%u result=%08" PRIx32 " fov_right=%.6f fov_left=%.6f distance=%.6f\n",
            p->sensor,(uint32_t)rc,stereo.angleOfViewRight,stereo.angleOfViewLeft,stereo.distanceCameras);
    snapshot(p,"initial",-1,"initial");
    return running(p);
}

static bool set_ae(Probe *p, bool on, const char *label)
{
    Result a=record_call(p,label,"SetAutoExposure",0,on,CAMU_SetAutoExposure(p->select,on));
    bool value=!on;
    Result b=CAMU_IsAutoExposure(&value,p->select);
    record_call(p,label,"IsAutoExposure",0,value,b);
    return R_SUCCEEDED(a) && R_SUCCEEDED(b) && value==on;
}

static bool api_sweep(Probe *p)
{
    /* Return codes and images survive even if the AE boolean cannot be verified. */
    record_call(p,"setup","SetFrameRate15",0,FRAME_RATE_15,CAMU_SetFrameRate(p->select,FRAME_RATE_15));
    record_call(p,"setup","SetPhotoModeNormal",0,0,CAMU_SetPhotoMode(p->select,PHOTO_MODE_NORMAL));
    record_call(p,"setup","SetAutoWhiteBalance",0,0,CAMU_SetAutoWhiteBalance(p->select,false));
    for (unsigned ae=0; ae<2 && running(p); ++ae) {
        char label[64];
        snprintf(label,sizeof label,"ae%u_hold",ae);
        ++p->phase; set_ae(p,ae,label);
        if (!sample(p,label)) return false;
        /* Interleave zero references to distinguish drift from a step response. */
        static const int steps[]={0,-5,0,-4,0,-3,0,-2,0,-1,0,1,0,2,0,3,0,4,0,5,0};
        for (unsigned i=0; i<sizeof steps/sizeof *steps && running(p); ++i) {
            int step=steps[i];
            ++p->phase;
            snprintf(label,sizeof label,"ae%u_exp_%s%d",ae,step<0?"m":"p",abs(step));
            record_call(p,label,"SetExposure",0,(unsigned)(int32_t)step,CAMU_SetExposure(p->select,(s8)step));
            if (!sample(p,label)) return false;
        }
    }
    /* Test interactions which can alter timing/gain even without SetISO. */
    set_ae(p,true,"modes");
    static const CAMU_FrameRate rates[]={FRAME_RATE_15,FRAME_RATE_30,FRAME_RATE_15_TO_5,FRAME_RATE_15};
    for (unsigned i=0; i<sizeof rates/sizeof *rates && running(p); ++i) {
        char label[32]; snprintf(label,sizeof label,"rate_%u",(unsigned)rates[i]);
        ++p->phase;
        record_call(p,label,"SetFrameRate",0,rates[i],CAMU_SetFrameRate(p->select,rates[i]));
        if (!sample(p,label)) return false;
    }
    for (unsigned i=0; i<5 && running(p); ++i) {
        char label[32]; snprintf(label,sizeof label,"photo_%u",i);
        ++p->phase;
        record_call(p,label,"SetPhotoMode",0,i,CAMU_SetPhotoMode(p->select,(CAMU_PhotoMode)i));
        if (!sample(p,label)) return false;
    }
    record_call(p,"modes","SetPhotoModeNormal",0,0,CAMU_SetPhotoMode(p->select,PHOTO_MODE_NORMAL));
    for (unsigned i=0; i<3 && running(p); ++i) {
        char label[32]; snprintf(label,sizeof label,"ae_window_%u",i);
        ++p->phase;
        s16 x=i==1?W/4:0,y=i==1?H/4:0,w=i==1?W/2:W,h=i==1?H/2:H;
        record_call(p,label,"SetAutoExposureWindow",0,i,CAMU_SetAutoExposureWindow(p->select,x,y,w,h));
        if (!sample(p,label)) return false;
    }
    return running(p);
}

static bool write_reg(Probe *p, u16 addr, u16 value, const char *label)
{
    /* This function has deliberately no arbitrary-address path. */
    if (addr==0xa206)
        return R_SUCCEEDED(record_call(p,label,"WriteMcuVariableI2c",addr,value,
                                       CAMU_WriteMcuVariableI2c(p->select,addr,value)));
    if (addr!=0x3012 && addr!=0x3056 && addr!=0x3058 && addr!=0x305a && addr!=0x305c)
        return false;
    return R_SUCCEEDED(record_call(p,label,"WriteRegisterI2c",addr,value,
                                   CAMU_WriteRegisterI2c(p->select,addr,value)));
}

static bool direct_trial(Probe *p, const char *kind, const u16 *addresses,
                          const u16 *base, unsigned count, bool high)
{
    char label[40]; snprintf(label,sizeof label,"direct_%s_%s",kind,high?"high":"low");
    ++p->phase;
    bool wrote=true;
    for (unsigned i=0; i<count && wrote && running(p); ++i) {
        unsigned v;
        if (addresses[i]==0xa206) {
            v=base[i]*3/4;
            if (v<1) v=1;
        } else if (addresses[i]==0x3012) {
            /* Shorter exposure only: no frame-length/clock writes. */
            v=high?base[i]:base[i]/2;
            if (!v) v=1;
        } else {
            unsigned initial=base[i]&0x7f;
            unsigned next=high?initial:initial*3/4;
            if (!next) next=1;
            v=(base[i]&~0x7fu)|next; /* preserve digital gain and analog stage bits */
        }
        wrote=write_reg(p,addresses[i],(u16)v,label);
    }
    bool sampled=false;
    if (wrote && running(p)) sampled=sample(p,label) && p->phase_success;
    /* Cleanup runs even when B is pressed, capture fails or SD fills up. */
    ++p->phase;
    bool restored=true;
    for (unsigned i=0; i<count; ++i) {
        if (!write_reg(p,addresses[i],base[i],"restore")) restored=false;
        u16 got;
        Result rc=read_reg(p,addresses[i]==0xa206,addresses[i],&got,"restore",-1,"immediate","restore_value");
        if (R_FAILED(rc) || got!=base[i]) restored=false;
    }
    fprintf(p->meta,"RESTORE sensor=%u phase=%u kind=%s immediate_ok=%u\n",p->sensor,p->phase,kind,restored);
    flush_logs(p);
    if (!restored) { p->aborted=true; return false; }
    if (sampled && running(p)) return sample(p,"restore");
    return false;
}

static bool direct_probe(Probe *p)
{
    if (!p->map_known || !p->capture_ready) {
        fprintf(p->meta,"DIRECT_SKIP sensor=%u reason=unknown_id_or_no_capture\n",p->sensor);
        return true;
    }
    ++p->phase;
    if (!set_ae(p,false,"direct_lock")) {
        fprintf(p->meta,"DIRECT_SKIP sensor=%u reason=ae_not_confirmed_off\n",p->sensor);
        return true;
    }
    bool awb=true;
    Result a=record_call(p,"direct_lock","SetAutoWhiteBalance",0,0,CAMU_SetAutoWhiteBalance(p->select,false));
    Result b=CAMU_IsAutoWhiteBalance(&awb,p->select);
    record_call(p,"direct_lock","IsAutoWhiteBalance",0,awb,b);
    if (R_FAILED(a) || R_FAILED(b) || awb) {
        fprintf(p->meta,"DIRECT_SKIP sensor=%u reason=awb_not_confirmed_off\n",p->sensor);
        return true;
    }
    if (!sample(p,"direct_baseline")) return false;
    if (!p->phase_success) {
        fprintf(p->meta,"DIRECT_SKIP sensor=%u reason=no_baseline_frames\n",p->sensor);
        return true;
    }
    const u16 addresses[]={0x3012,0x3056,0x3058,0x305a,0x305c};
    u16 base[5];
    for (unsigned i=0; i<5; ++i)
        if (R_FAILED(read_reg(p,false,addresses[i],&base[i],"direct_baseline",-1,"saved","saved_value"))) {
            fprintf(p->meta,"DIRECT_SKIP sensor=%u reason=baseline_read_failed\n",p->sensor);
            return true;
        }
    /* Two A-B-A cycles; restores are separately sampled for reversibility. */
    for (unsigned cycle=0; cycle<2 && running(p); ++cycle) {
        if (base[0]>1 && !direct_trial(p,"integration",addresses,base,1,false)) return false;
        bool gain_valid=true;
        for (unsigned i=1; i<5; ++i) if ((base[i]&0x7f)<2 || (base[i]&0xf000)) gain_valid=false;
        if (gain_valid && !direct_trial(p,"analog",addresses+1,base+1,4,false)) return false;
    }
    ++p->phase;
    if (!set_ae(p,true,"direct_ae_target_baseline")) return running(p);
    if (!sample(p,"direct_ae_target_baseline") || !p->phase_success) return false;
    u16 target_addr=0xa206, target;
    if (R_SUCCEEDED(read_reg(p,true,target_addr,&target,"direct_ae_target_baseline",-1,"saved","ae_target"))
            && target>1 && target<=255) {
        for (unsigned cycle=0; cycle<2 && running(p); ++cycle)
            if (!direct_trial(p,"ae_target",&target_addr,&target,1,false)) return false;
    } else fprintf(p->meta,"DIRECT_SKIP sensor=%u reason=ae_target_read_failed_or_out_of_range\n",p->sensor);
    return running(p);
}

static bool open_logs(Probe *p)
{
    mkdir("sdmc:/3ds",0777); mkdir("sdmc:/3ds/Film3DS",0777);
    mkdir("sdmc:/3ds/Film3DS/probes",0777);
    for (unsigned suffix=0; suffix<1000; ++suffix) {
        snprintf(p->directory,sizeof p->directory,"sdmc:/3ds/Film3DS/probes/run-%lld-%03u",
                 (long long)time(NULL),suffix);
        if (mkdir(p->directory,0777)==0) break;
        if (suffix==999) return false;
    }
    const char *names[]={"calls.csv","reads.csv","frames.csv","manifest.txt"};
    FILE **files[]={&p->calls,&p->reads,&p->frames,&p->meta};
    for (unsigned i=0; i<4; ++i) {
        char path[200]; snprintf(path,sizeof path,"%s/%s",p->directory,names[i]);
        *files[i]=fopen(path,"w"); if (!*files[i]) return false;
    }
    fputs("sensor,phase,label,operation,addr,value,result,time_ms\n",p->calls);
    fputs("sensor,phase,label,frame,when,space,name,addr,value,result,time_ms\n",p->reads);
    fputs("sensor,phase,label,frame,measured,ok,result,received,y_mean,y_p50,endpoint_low,endpoint_high,crc32,start_ms,end_ms\n",p->frames);
    bool newer=false; Result rc=APT_CheckNew3DS(&newer);
    fprintf(p->meta,"FILMPROBE schema=1 revision=%s new3ds=%u model_result=%08" PRIx32
            " kernel=%08" PRIx32 " settle_frames=%u measured_frames=%u\n",
            PROBE_REVISION,newer,(uint32_t)rc,(uint32_t)osGetKernelVersion(),SETTLE,SAMPLES);
    flush_logs(p); return !p->io_error;
}

int main(int argc, char **argv)
{
    gfxInitDefault(); consoleInit(GFX_TOP,NULL);
    Probe p={0}; bool direct=false, launch=false;
    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i],"--raw")) p.raw=true;
        if (!strcmp(argv[i],"--auto")) launch=true;
        if (!strcmp(argv[i],"--direct")) direct=true;
    }
    printf("FilmProbe %s\n\nFix console on a stand. Use steady light.\n"
           "Keep the same scene through the run.\n\nA: API sweep + register reads\n"
           "X: also test integration / analog writes\nY: toggle full YUYV (default: luma PGM)\n"
           "START: exit   B: abort a running test\n\nAll 3 cameras; allow several minutes.\n",PROBE_REVISION);
    while (!launch && aptMainLoop()) {
        hidScanInput(); u32 keys=hidKeysDown();
        if (keys&KEY_START) { gfxExit(); return 0; }
        if (keys&KEY_Y) { p.raw=!p.raw; printf("Full YUYV: %s\n",p.raw?"ON":"OFF"); }
        if (keys&(KEY_A|KEY_X)) { direct=(keys&KEY_X)!=0; launch=true; }
        gspWaitForVBlank();
    }
    if (!launch) { gfxExit(); return 1; }
    bool completed=false;
    if (open_logs(&p)) {
        fprintf(p.meta,"OPTIONS direct=%u raw=%u\n",direct,p.raw);
        p.pixels=linearMemAlign(BYTES,0x80);
        if (p.pixels) {
            for (unsigned sensor=1; sensor<=3 && running(&p); ++sensor) {
                p.phase=0;
                if (open_camera(&p,sensor)) {
                    bool ok=api_sweep(&p);
                    if (ok && direct) ok=direct_probe(&p);
                    fprintf(p.meta,"SENSOR_END sensor=%u complete=%u\n",sensor,ok);
                } else fprintf(p.meta,"SENSOR_END sensor=%u complete=0\n",sensor);
                close_camera(&p); flush_logs(&p);
            }
            completed=!p.aborted && !p.io_error && p.sensor==3;
            linearFree(p.pixels);
        } else fputs("ERROR allocation_failed\n",p.meta);
        fprintf(p.meta,"END complete=%u aborted=%u io_error=%u captures=%u failures=%u\n",
                completed,p.aborted,p.io_error,p.captures,p.failures);
        flush_logs(&p);
    }
    FILE *files[]={p.calls,p.reads,p.frames,p.meta};
    for (unsigned i=0; i<4; ++i) if (files[i] && fclose(files[i])) p.io_error=true;
    printf("\x1b[16;0H%s\n%s\nCaptures %u; failures %u\nCopy run folder to PC.\nRun tools/exposure_report.py FOLDER\nSTART: exit\n",
           completed&&!p.io_error?"Run ended; inspect report":"INCOMPLETE: inspect partial logs",
           p.directory,p.captures,p.failures);
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown()&KEY_START) break; gspWaitForVBlank(); }
    gfxExit(); return completed&&!p.io_error?0:1;
}
