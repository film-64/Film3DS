#define _GNU_SOURCE 1
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
#include <sys/statvfs.h>
#include <errno.h>
#include <time.h>
#include "remote_upload.h"
#include "deep_ipc.h"
#include "crash_collect.h"
#include "probe_plan.h"
#include "ram_log.h"
#include "calibration_contract.h"
#include "fault_capture.h"

#ifndef PROBE_REVISION
#define PROBE_REVISION "unknown"
#endif
enum { W = 640, H = 480, BYTES = W * H * 2, SETTLE = 8, SAMPLES = 12 };
typedef struct { u16 addr; const char *name; bool mcu; } Register;
static const Register registers[] = {
    {0x0000,"chip_id",false}, {0x0010,"pll_dividers",false},
    {0x0012,"pll_p_dividers",false}, {0x0014,"pll_control",false},
    {0x300a,"frame_lines",false}, {0x300c,"line_pck",false},
    {0x3002,"window_y_start",false}, {0x3004,"window_x_start",false},
    {0x3006,"window_y_end",false}, {0x3008,"window_x_end",false},
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
    FILE *calls, *reads, *frames, *meta, *calibration, *burst, *roi, *noise,*execution;
    char directory[160];
    unsigned sensor, phase, transfer;
    u32 select, port;
    u8 *pixels;
    bool aborted, io_error, map_known, capture_ready, driver, streaming, raw, phase_success, one_shot;
    unsigned captures, failures, samples, successful_samples, first_phase;
    Handle error_event;
    unsigned settle_frames, measured_frames;
    s64 wait_ns;
    bool smoke, frame_log_flush;
    unsigned calibration_part;
} Probe;
#include "calibration_image.h"

static void flush_logs(Probe *p)
{
    FILE *files[] = {p->calls,p->reads,p->frames,p->meta,p->calibration,p->burst,p->roi,p->noise,p->execution
#ifdef __3DS__
        ,deep_ipc_log
#endif
    };
    for (unsigned i=0; i<sizeof files/sizeof *files; ++i)
        if (files[i] && (fflush(files[i]) || ferror(files[i]))) {
            if(!p->io_error)printf("\n%s_LOG_ERROR stream=%u errno=%d phase=%u\n",ram_log.active?"RAM":"SD",i,errno,p->phase);
            p->io_error = true;
        }
    if(ram_log.failed)p->io_error=true;
}

static bool buffer_log(Probe *p,FILE *file)
{
    if(!file)return false;
    if(p->frame_log_flush && setvbuf(file,NULL,_IOFBF,32768)) {
        p->io_error=true;printf("%s_LOG_ERROR operation=setvbuf\n",ram_log.active?"RAM":"SD");return false;
    }
    return true;
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
    if(!p->frame_log_flush)flush_logs(p);
    return rc;
}

static Result read_reg(Probe *p, bool mcu, u16 addr, u16 *value,
                        const char *label, int frame, const char *when, const char *name)
{
    *value = 0; /* failed reads are logged with a result, never treated as zero measurements */
    Result rc = PROBE_READ(value,p->select,addr,mcu);
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
    Result rc=PROBE_AE(&enabled,p->select);
    record_call(p,label,"IsAutoExposure",0,enabled,rc);
    enabled=false;
    rc=PROBE_AWB(&enabled,p->select);
    record_call(p,label,"IsAutoWhiteBalance",0,enabled,rc);
    if(!p->frame_log_flush)flush_logs(p);
}

static void stop(Probe *p, const char *label)
{
    Result stopped=record_call(p,label,"StopCapture",0,0,CAMU_StopCapture(p->port));
    bool busy=true;
    Result state=stopped;
    u64 deadline=osGetTime()+(p->wait_ns>INT64_C(2000000000)?(u64)(p->wait_ns/1000000):2000);
    /* Explicit bounded stop completion: do not restart or reuse DMA while busy. */
    while(R_SUCCEEDED(state) && busy && osGetTime()<deadline) {
        state=PROBE_BUSY(&busy,p->port);
        record_call(p,label,"IsBusyAfterStop",0,busy,state);
        if(R_SUCCEEDED(state) && busy)svcSleepThread(INT64_C(10000000));
    }
    bool quiescent=R_SUCCEEDED(stopped) && R_SUCCEEDED(state) && !busy;
    Result cleared=quiescent?CAMU_ClearBuffer(p->port):state;
    record_call(p,label,quiescent?"ClearBuffer":"ClearBufferSkippedBusy",0,quiescent,cleared);
    p->streaming=!quiescent;
    if(!quiescent || R_FAILED(cleared))p->capture_ready=false;
}

static uint32_t probe_crc32(const u8 *bytes, unsigned count)
{
    static uint32_t table[256];static bool ready;
    if(!ready) {
        for(unsigned i=0;i<256;++i) {
            uint32_t c=i;
            for(unsigned b=0;b<8;++b)c=(c>>1)^(0xedb88320u&(0u-(c&1)));
            table[i]=c;
        }
        ready=true;
    }
    uint32_t crc=UINT32_MAX;
    for(unsigned i=0;i<count;++i)crc=(crc>>8)^table[(crc^bytes[i])&255];
    return ~crc;
}

static bool save_frame(Probe *p, const char *label)
{
    char path[240];
    snprintf(path,sizeof path,"%s/s%u-p%03u-%s.%s",p->directory,p->sensor,p->phase,label,
             p->raw?"yuyv":"pgm");
    FILE *f=probe_fopen(path,"wb");
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
    if (R_SUCCEEDED(rc) && !p->streaming) {
        rc=CAMU_ClearBuffer(p->port);
        record_call(p,label,"ClearBeforeStart",0,0,rc);
#ifdef __3DS__
        if(p->error_event) {
            Result signaled=svcWaitSynchronization(p->error_event,0);
            record_call(p,label,"BufferErrorBeforeArm",0,signaled==0,signaled);
            if(signaled==0)record_call(p,label,"ClearBufferErrorEvent",0,0,svcClearEvent(p->error_event));
        }
#endif
    }
    /* Arm DMA before starting the producer, so no frame begins without a sink. */
    if (R_SUCCEEDED(rc)) {
        rc=CAMU_SetReceiving(&event,p->pixels,p->port,BYTES,(s16)p->transfer);
        record_call(p,label,"SetReceiving",0,BYTES,rc);
    }
    if (R_SUCCEEDED(rc) && !p->streaming) {
        rc=CAMU_StartCapture(p->port);
        record_call(p,label,"StartCapture",0,0,rc);
        p->streaming=R_SUCCEEDED(rc);
    }
    if (R_SUCCEEDED(rc)) rc=svcWaitSynchronization(event,p->wait_ns?p->wait_ns:INT64_C(2000000000));
    record_call(p,label,"WaitFrame",0,event,rc);
    bool event_complete=rc==0; /* Positive RD_TIMEOUT is not a completed wait. */
    bool received=false;
    if (event_complete) {
        rc=PROBE_FINISHED(&received,p->port);
        record_call(p,label,"IsFinishedReceiving",0,received,rc);
    }
    if (R_SUCCEEDED(rc) && received) {
        rc=GSPGPU_InvalidateDataCache(p->pixels,BYTES);
        record_call(p,label,"InvalidateDataCache",0,0,rc);
    }
    bool ok=event_complete && R_SUCCEEDED(rc) && received;
    if (!ok) { ++p->failures; stop(p,label); }
    else if (p->one_shot) stop(p,label);
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
            ok?probe_crc32(p->pixels,BYTES):0,(uint64_t)start,(uint64_t)osGetTime());
    if(ok && measured && p->roi)cal_image(p,label,frame);
    if (ok && measured) snapshot(p,label,frame,"after");
    if (ok && save) save_frame(p,label);
    flush_logs(p);
    return ok && running(p);
}

static bool sample(Probe *p, const char *label)
{
    p->phase_success=false;
    if(p->phase<p->first_phase) {
        fprintf(p->meta,"SETUP_REPLAY sensor=%u phase=%u label=%s measured=0\n",p->sensor,p->phase,label);
        return running(p);
    }
    ++p->samples;
    printf("\x1b[12;0HSensor %u phase %u: %-28s\nB: stop after current operation\n",p->sensor,p->phase,label);
    snapshot(p,label,-1,"immediate");
    if (!p->capture_ready) return running(p);
    unsigned settle=p->settle_frames?p->settle_frames:SETTLE;
    unsigned samples=p->measured_frames?p->measured_frames:SAMPLES;
    cal_noise_reset();
    for (unsigned i=0; i<settle+samples; ++i)
        if (!capture(p,label,(int)i,i>=settle,i==settle+samples-1)) {
            fprintf(p->meta,"SAMPLE_FAILED sensor=%u phase=%u label=%s\n",p->sensor,p->phase,label);
            /* Stop this sensor sweep; preserve evidence and continue next sensor. */
            return false;
        }
    p->phase_success=true;
    if(p->noise)cal_noise_write(p,label);
    ++p->successful_samples;
    return running(p);
}

static void close_camera(Probe *p)
{
    if (!p->driver) return;
    stop(p,"cleanup");
    record_call(p,"cleanup","ActivateNone",0,0,CAMU_Activate(SELECT_NONE));
    if(p->error_event) {svcCloseHandle(p->error_event);p->error_event=0;}
    camExit();
#ifdef __3DS__
    deep_ports_close();
#endif
    p->driver=p->capture_ready=false;
}

static bool open_camera(Probe *p, unsigned sensor)
{
    static const u32 selects[]={SELECT_OUT1,SELECT_OUT2,SELECT_IN1};
    p->sensor=sensor; p->select=selects[sensor-1];
#ifdef __3DS__
    deep_sensor=sensor;
#endif
    p->port=sensor==2?PORT_CAM2:PORT_CAM1;
    p->map_known=p->capture_ready=p->streaming=false;
    Result rc=record_call(p,"open","camInit",0,0,camInit());
    if (R_FAILED(rc)) return false;
    p->driver=true;
    rc=record_call(p,"open","Activate",0,p->select,CAMU_Activate(p->select));
    if (R_FAILED(rc)) return false;
    u16 id;
#ifdef __3DS__
    deep_ports(p->meta,p->select);
#endif
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
#ifdef __3DS__
    rc=CAMU_GetBufferErrorInterruptEvent(&p->error_event,p->port);
    record_call(p,"open","GetBufferErrorEvent",0,p->error_event,rc);
    if(R_FAILED(rc))p->error_event=0;
#endif
    Y2RU_StandardCoefficient coefficient=0;
    rc=CAMU_GetSuitableY2rStandardCoefficient(&coefficient);
    record_call(p,"open","GetY2rCoefficient",0,coefficient,rc);
    u32 active=0;
    rc=CAMU_GetActivatedCamera(&active);
    record_call(p,"open","GetActivatedCamera",0,active,rc);
    u32 sleep_camera=0;
    rc=CAMU_GetSleepCamera(&sleep_camera);
    record_call(p,"open","GetSleepCamera",0,sleep_camera,rc);
#ifdef __3DS__
    fputs("INIT_STEP stage=calibration_queries_begin\n",p->meta);flush_logs(p);
    deep_calibration_reply(p->meta,0x32,(sizeof(CAMU_ImageQualityCalibrationData)+3)/4);
    deep_calibration_reply(p->meta,0x2b,16);
    fputs("INIT_STEP stage=calibration_queries_end\n",p->meta);flush_logs(p);
#else
    CAMU_ImageQualityCalibrationData quality={0};
    rc=CAMU_GetImageQualityCalibrationData(&quality);
    record_call(p,"open","GetImageQualityCalibrationData",0,(unsigned)(u16)quality.aeBaseTarget,rc);
    CAMU_StereoCameraCalibrationData stereo={0};
    rc=CAMU_GetStereoCameraCalibrationData(&stereo);
    record_call(p,"open","GetStereoCameraCalibrationData",0,0,rc);
    fprintf(p->meta,"STEREO sensor=%u result=%08" PRIx32 " fov_right=%.6f fov_left=%.6f distance=%.6f\n",
            p->sensor,(uint32_t)rc,stereo.angleOfViewRight,stereo.angleOfViewLeft,stereo.distanceCameras);
#endif
    fputs("INIT_STEP stage=initial_snapshot_begin\n",p->meta);flush_logs(p);
    snapshot(p,"initial",-1,"initial");
    fputs("INIT_STEP stage=initial_snapshot_end\n",p->meta);flush_logs(p);
    return running(p);
}

static bool set_ae(Probe *p, bool on, const char *label)
{
    Result a=record_call(p,label,"SetAutoExposure",0,on,CAMU_SetAutoExposure(p->select,on));
    bool value=!on;
    Result b=PROBE_AE(&value,p->select);
    record_call(p,label,"IsAutoExposure",0,value,b);
    return R_SUCCEEDED(a) && R_SUCCEEDED(b) && value==on;
}

/* Explicit acquisition comparison. A failed continuous attempt is retained;
 * it is never relabeled a successful lock test or silently retried. */
static bool acquisition_preflight(Probe *p)
{
    ++p->phase;
    bool continuous=true;
    for(int i=0;i<4 && running(p);++i)
        if(!capture(p,"preflight_continuous",i,false,i==3)){continuous=false;break;}
    if(p->streaming)stop(p,"preflight_continuous_end");
    fprintf(p->meta,"ACQUISITION sensor=%u mode=continuous complete=%u\n",p->sensor,continuous);
    /* Register bracketing uses exclusive I2C reads and can exceed a frame period.
     * Measure with the producer stopped between frames even when fast streaming works. */
    if(continuous && !p->map_known)return true;
    if(!p->capture_ready || !running(p))return false;
    p->one_shot=true;
    fprintf(p->meta,"ACQUISITION_SELECTED sensor=%u mode=single_frame_restart reason=register_brackets_or_stream_failure\n",p->sensor);
    ++p->phase;
    bool single=true;
    for(int i=0;i<4 && running(p);++i)
        if(!capture(p,"preflight_single",i,false,i==3)){single=false;break;}
    fprintf(p->meta,"ACQUISITION sensor=%u mode=single_frame_restart complete=%u lock_scope=per_capture_only\n",p->sensor,single);
    return single && running(p);
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
                                       PROBE_WRITE(p->select,addr,value,true)));
    if (addr!=0x3012 && addr!=0x3056 && addr!=0x3058 && addr!=0x305a && addr!=0x305c)
        return false;
    return R_SUCCEEDED(record_call(p,label,"WriteRegisterI2c",addr,value,
                                   PROBE_WRITE(p->select,addr,value,false)));
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
    Result b=PROBE_AWB(&awb,p->select);
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

#include "calibration.h"
#ifdef __3DS__
#define main film_recovery_entry
#include "../recovery/main.c"
#undef main
#endif
#include "ram_http.h"

static bool open_logs(Probe *p)
{
    if(ram_log.active)snprintf(p->directory,sizeof p->directory,"ram:/run-%lld",(long long)time(NULL));
    else {
    mkdir("sdmc:/3ds",0777); mkdir("sdmc:/3ds/Film3DS",0777);
    mkdir("sdmc:/3ds/Film3DS/probes",0777);
    for (unsigned suffix=0; suffix<1000; ++suffix) {
        snprintf(p->directory,sizeof p->directory,"sdmc:/3ds/Film3DS/probes/run-%lld-%03u",
                 (long long)time(NULL),suffix);
        if (mkdir(p->directory,0777)==0) break;
        if (suffix==999) return false;
    }
    }
    const char *names[]={"calls.csv","reads.csv","frames.csv","manifest.txt"};
#ifdef __3DS__
    snprintf(probe_fault_path,sizeof probe_fault_path,"%s/probe-fault.dmp",p->directory);
    snprintf(probe_lifecycle_path,sizeof probe_lifecycle_path,"%s/lifecycle.txt",p->directory);
#endif
    FILE **files[]={&p->calls,&p->reads,&p->frames,&p->meta};
    for (unsigned i=0; i<4; ++i) {
        char path[200]; snprintf(path,sizeof path,"%s/%s",p->directory,names[i]);
        *files[i]=probe_fopen(path,"w"); if (!buffer_log(p,*files[i])) return false;
    }
    fputs("sensor,phase,label,operation,addr,value,result,time_ms\n",p->calls);
    fputs("sensor,phase,label,frame,when,space,name,addr,value,result,time_ms\n",p->reads);
    fputs("sensor,phase,label,frame,measured,ok,result,received,y_mean,y_p50,endpoint_low,endpoint_high,crc32,start_ms,end_ms\n",p->frames);
    bool newer=false; Result rc=APT_CheckNew3DS(&newer);
    fprintf(p->meta,"FILMPROBE schema=1 revision=%s new3ds=%u model_result=%08" PRIx32
            " kernel=%08" PRIx32 " settle_frames=%u measured_frames=%u\n",
            PROBE_REVISION,newer,(uint32_t)rc,(uint32_t)osGetKernelVersion(),SETTLE,SAMPLES);
    struct statvfs space;
    if(!ram_log.active && !statvfs("sdmc:/",&space))fprintf(p->meta,"STORAGE block_bytes=%llu available_blocks=%llu free_blocks=%llu\n",
        (unsigned long long)space.f_frsize,(unsigned long long)space.f_bavail,(unsigned long long)space.f_bfree);
    flush_logs(p); return !p->io_error;
}

int main(int argc, char **argv)
{
#ifdef __3DS__
    probe_fault_install();
#endif
    gfxInitDefault(); consoleInit(GFX_TOP,NULL);
#ifdef __3DS__
    gfxSetDoubleBuffering(GFX_BOTTOM,false);
#endif
    ProbeRequest request;
    if(!probe_parse_request(argc,argv,&request)) {
        puts("Invalid request: --calibrate needs one sensor, no resume/direct; --calibration-part=gain|slow|limits|digital requires --offline, no smoke");
        gfxExit();return 2;
    }
#ifdef PROBE_OFFLINE_DEFAULT
    if(argc<=1)probe_offline_default(&request);
#endif
    Probe p={0};bool direct=request.direct,launch=request.auto_start;
    p.smoke=request.smoke;p.calibration_part=request.calibration_part;
    p.frame_log_flush=request.ram || request.calibration_part!=CAL_FULL;
#ifdef __3DS__
    probe_ram_mode=request.ram;
#endif
    p.raw=request.raw;p.first_phase=request.first_phase;
    if(request.ram && !request.calibration_part)printf("FilmCalibration %s\nRAM validation: 20 frames only\nSensor %u; no SD writes\nA/X: start registered short test\nAfter capture: keep HTTP open.\nSTART exits and discards RAM.\n",PROBE_REVISION,request.sensor);
    else if(request.calibration_part)printf("FilmCalibration %s\nPart %s: %u stages\nSensor %u; %s\nKeep target and light fixed.\nA/X: start selected part\nB: stop after current operation\nSTART: exit\n",PROBE_REVISION,cal_scope_name(request.calibration_part),cal_scope_stages(request.calibration_part),request.sensor,request.ram?"RAM only; no SD writes":"SD-only");
    else if(request.calibrate)printf("FilmCalibration %s\n\nSensor %u; SD-only=%u\nKeep target, position and light fixed.\n\n"
           "A: short 20-frame check\nX: full 58-stage calibration\nB: stop after current operation\nSTART: exit\n\n"
           "%s\n",PROBE_REVISION,request.sensor,request.offline,
           request.offline?"SD only: no compression or upload.":"After measurements: gzip and HTTP recovery.");
    else printf("FilmProbe %s\n\nFix console on a stand. Use steady light.\n"
           "Keep the same scene through the run.\n\nA: API sweep + register reads\n"
           "X: also test integration / analog writes\nY: toggle full YUYV (default: luma PGM)\n"
           "START: exit   B: abort a running test\n\nRequested camera scope; progress below.\n",PROBE_REVISION);
    printf("Requested sensor: %u (0 = all); first measured phase: %u; calibration=%u\n",request.sensor,request.first_phase,request.calibrate);
    if(request.calibration_part)printf("PART %s: %u stages; frame-boundary log flush; no rerun.\n",cal_scope_name(request.calibration_part),cal_scope_stages(request.calibration_part));
    while (!launch && aptMainLoop()) {
        hidScanInput(); u32 keys=hidKeysDown();
        if (keys&KEY_START) { gfxExit(); return 0; }
        if ((keys&KEY_Y) && !request.calibrate) { p.raw=!p.raw; printf("Full YUYV: %s\n",p.raw?"ON":"OFF"); }
        if (keys&(KEY_A|KEY_X)) {
            if(request.calibrate){probe_menu_choice(&request,(keys&KEY_X)!=0);p.smoke=request.smoke;}
            else direct=(keys&KEY_X)!=0;
            launch=true;
        }
        gspWaitForVBlank();
    }
    if (!launch) { gfxExit(); return 1; }
    if(request.ram && !ram_begin()) {
        puts("RAM allocation failed before camera access. START exits.");
        while(aptMainLoop()){hidScanInput();if(hidKeysDown()&KEY_START)break;gspWaitForVBlank();}
        gfxExit();return 1;
    }
    bool completed=false;
    if (open_logs(&p)) {
        fprintf(p.meta,"OPTIONS direct=%u raw=%u acquisition=continuous deterministic_ipc=1 transfer=%s\n",direct,p.raw,
            request.ram?"ram_http":request.offline?"sd_only":request.calibrate?"gzip_http":"final_only");
        if(request.ram)fputs("RAM_EVIDENCE contract=1 bytes=20971520 sd_writes=0 freeze_after_camera=1 no_compression=1 retry=none\n",p.meta);
        fprintf(p.meta,"REQUEST only_sensor=%u first_phase=%u last_phase=%u\n",request.sensor,request.first_phase,request.smoke?3:request.calibrate?cal_scope_stages(request.calibration_part)+2:direct?PROBE_DIRECT_LAST_PHASE:PROBE_SWEEP_LAST_PHASE);
        if(request.calibrate)fprintf(p.meta,"CALIBRATION contract=%u expected_stages=%u target=stable_scene lock_scope=per_capture units=raw_codes scope=%s\n",request.calibration_part?2:1,request.smoke?1:cal_scope_stages(request.calibration_part),request.smoke?"smoke":cal_scope_name(request.calibration_part));
        if(p.frame_log_flush)fputs("LOG_POLICY frame_boundary buffers=32768 all_streams=1 retry=none\n",p.meta);
#ifdef __3DS__
        char ipc_path[240];snprintf(ipc_path,sizeof ipc_path,"%s/ipc.csv",p.directory);
        deep_ipc_log=probe_fopen(ipc_path,"w");
        if(buffer_log(&p,deep_ipc_log)) fputs("sensor,service,command,transport_result,service_result,start_ms,request_first8,response64\n",deep_ipc_log);
        else p.io_error=true;
#endif
        if(request.ram) {
            puts("RAM evidence: no SD dump collection or process mappings.");
            fputs("CRASH_COLLECTION skipped_existing_dump_already_recovered ram_mode=1\n",p.meta);
#ifdef __3DS__
            probe_ram_lifecycle=probe_fopen("lifecycle.txt","w");
            if(!probe_ram_lifecycle)p.io_error=true;
#endif
        } else {
            printf("Collecting saved crash file; no process mappings...\n");
            probe_collect_crash(p.meta,p.directory);
        }
        flush_logs(&p);
#ifdef __3DS__
        if(deep_ipc_log)fflush(deep_ipc_log);
#endif

        p.pixels=linearMemAlign(BYTES,0x80);
        if (p.pixels) {
            bool sensors_complete=true;
            for (unsigned sensor=1; sensor<=3 && running(&p); ++sensor) {
                if(request.sensor && sensor!=request.sensor)continue;
                p.phase=0;p.one_shot=false;
                if (open_camera(&p,sensor)) {
                    bool acquired=acquisition_preflight(&p);
                    bool ok=acquired && (request.calibrate?calibration_probe(&p):api_sweep(&p));
                    if (ok && direct && !request.calibrate) ok=direct_probe(&p);
                    sensors_complete=sensors_complete && ok;
                    fprintf(p.meta,"SENSOR_END sensor=%u complete=%u\n",sensor,ok);
                } else {sensors_complete=false;fprintf(p.meta,"SENSOR_END sensor=%u complete=0\n",sensor);}
                close_camera(&p); flush_logs(&p);
#ifdef __3DS__
                if(deep_ipc_log)fflush(deep_ipc_log);
#endif

            }
            completed=sensors_complete && !p.aborted && !p.io_error && p.sensor==(request.sensor?request.sensor:3);
            fprintf(p.meta,"MEASUREMENTS expected=%u complete=%u all_complete=%u\n",p.samples,p.successful_samples,p.samples>0 && p.samples==p.successful_samples);

            linearFree(p.pixels);
        } else fputs("ERROR allocation_failed\n",p.meta);
        fprintf(p.meta,"END complete=%u aborted=%u io_error=%u captures=%u failures=%u\n",
                completed,p.aborted,p.io_error,p.captures,p.failures);
        flush_logs(&p);
    }
#ifdef __3DS__
    if(deep_ipc_log) {if(fclose(deep_ipc_log))p.io_error=true;deep_ipc_log=NULL;}
#endif
    FILE *files[]={p.calls,p.reads,p.frames,p.meta,p.calibration,p.burst,p.roi,p.noise,p.execution};
    for (unsigned i=0; i<sizeof files/sizeof *files; ++i) if (files[i] && fclose(files[i])) p.io_error=true;
    if(request.ram) {
#ifdef __3DS__
        probe_ram_lifecycle=NULL;
#endif
        bool frozen=ram_freeze();
        printf("\x1b[12;0HSTOPPED Sensor %u phase %u\n",p.sensor,p.phase);
        printf("%s; captures %u failures %u\n",completed&&!p.io_error&&frozen?"Run ended":"INCOMPLETE",p.captures,p.failures);
        ram_http_serve(p.directory,completed&&!p.io_error&&frozen);
        ram_release();gfxExit();return completed&&!p.io_error&&frozen?0:1;
    }
#ifdef __3DS__
    if(request.calibrate && !request.offline && p.directory[0]) {
        char run_option[192];const char *run=strrchr(p.directory,'/');
        snprintf(run_option,sizeof run_option,"--run=%s",run?run+1:p.directory);
        printf("Measurement ended. Packing SD files; then HTTP 5000.\n");
        gfxExit();char *recovery_args[]={"FilmRecovery",run_option};
        int recovered=film_recovery_entry(2,recovery_args);
        if(recovered) {
            printf("\nRECOVERY ERROR status=%d errno=%d\nSD originals retained. START exits.\n",recovered,errno);
            while(aptMainLoop()){hidScanInput();if(hidKeysDown()&KEY_START)break;gspWaitForVBlank();}
            gfxExit();
        }
        return recovered;
    }
#endif
    if(!request.offline)remote_upload_checkpoint(p.directory,"final");
    printf("\x1b[16;0H%s\n%s\nCaptures %u; failures %u\nCopy run folder to PC.\nRun tools/%s FOLDER\nSTART: exit\n",
           completed&&!p.io_error?"Run ended; inspect report":"INCOMPLETE: inspect partial logs",
           p.directory,p.captures,p.failures,request.calibrate?"calibration_report.py":"exposure_report.py");
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown()&KEY_START) break; gspWaitForVBlank(); }
    gfxExit(); return completed&&!p.io_error?0:1;
}
