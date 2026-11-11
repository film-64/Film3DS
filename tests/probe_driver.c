/* Execute the actual production trial/cleanup functions against injected failures. */
#define main filmprobe_app_main
#include "../probes/exposure/main.c"
#undef main
#include <assert.h>

static u16 values[65536];
static unsigned writes, receives, closed_events, stops;
static bool cancel_after_write, timeout_capture, fail_restore, deny_read;
static bool ae, awb;
static u8 *dma;
void hidScanInput(void) {}
u32 hidKeysHeld(void) { return cancel_after_write && writes ? KEY_B : 0; }
bool aptMainLoop(void) { return true; }
u64 osGetTime(void) { static u64 now; return ++now; }
Result CAMU_ReadRegisterI2cExclusive(u16 *v,u32 s,u16 a)
{ if (deny_read) return (Result)0xd900182f; *v=values[a]; return 0; }
Result CAMU_ReadMcuVariableI2cExclusive(u16 *v,u32 s,u16 a)
{ return CAMU_ReadRegisterI2cExclusive(v,s,a); }
Result CAMU_WriteRegisterI2c(u32 s,u16 a,u16 v)
{ ++writes; if (fail_restore && v==100) return -1; values[a]=v; return 0; }
Result CAMU_WriteMcuVariableI2c(u32 s,u16 a,u16 v)
{ return CAMU_WriteRegisterI2c(s,a,v); }
Result CAMU_IsAutoExposure(bool *v,u32 s) { *v=ae; return 0; }
Result CAMU_IsAutoWhiteBalance(bool *v,u32 s) { *v=awb; return 0; }
Result CAMU_SetAutoExposure(u32 s,bool on) { ae=on; return 0; }
Result CAMU_SetAutoWhiteBalance(u32 s,bool on) { awb=on; return 0; }
Result GSPGPU_FlushDataCache(const void *b,u32 n) { return 0; }
Result GSPGPU_InvalidateDataCache(const void *b,u32 n) { return 0; }
Result CAMU_SetReceiving(Handle *e,void *b,u32 p,u32 n,s16 t)
{ ++receives; *e=7; dma=b; memset(dma,100,n); return 0; }
Result CAMU_StartCapture(u32 p) { return 0; }
Result CAMU_StopCapture(u32 p) { ++stops; return 0; }
Result CAMU_ClearBuffer(u32 p) { return 0; }
Result CAMU_IsFinishedReceiving(bool *v,u32 p) { *v=true; return 0; }
Result svcWaitSynchronization(Handle e,s64 t) { return timeout_capture ? -1 : 0; }
Result svcCloseHandle(Handle e) { ++closed_events; return 0; }

static Probe setup(void)
{
    Probe p={.sensor=1,.select=SELECT_OUT1,.port=PORT_CAM1,.transfer=5120,
             .map_known=true,.capture_ready=true};
    p.calls=fopen("build/probe-calls.tmp","w+"); p.reads=fopen("build/probe-reads.tmp","w+");
    p.frames=fopen("build/probe-frames.tmp","w+"); p.meta=fopen("build/probe-meta.tmp","w+");
    assert(p.calls && p.reads && p.frames && p.meta);
    p.pixels=malloc(BYTES); assert(p.pixels);
    strcpy(p.directory,".");
    values[0x3012]=100;
    writes=receives=closed_events=stops=0;
    cancel_after_write=timeout_capture=fail_restore=deny_read=false;
    return p;
}
static void cleanup(Probe *p)
{ fclose(p->calls); fclose(p->reads); fclose(p->frames); fclose(p->meta); free(p->pixels); }

int main(void)
{
    const u16 addr=0x3012, base=100;
    Probe p=setup();
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
    puts("PASS actual probe driver: cancellation, timeout cleanup, restore failure, unknown sensor, denied reads, OTP guard");
    return 0;
}
