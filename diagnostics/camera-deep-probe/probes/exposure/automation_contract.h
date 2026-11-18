#pragma once
/* Registered hardware diagnostic source: exposure-auto/v6.
 * Sensor1, registered 15fps/5fps profiles; max18 stages, <=3 normal decisions.
 * Coarse-only normal decisions settle2 frames; gain/color changes settle8.
 * Four measured frames; a locked stage with >1/8EV sample
 * brightness span is rejected, not retried or averaged into a stable result.
 * ISP lock is established ONCE after the settling frames, immediately before
 * the first locked measured frame. It cannot be re-established after drift.
 * Normal work preserves native analogue color ratios, forces core digital 1x, pins all four ISP
 * digital settings, and changes ONLY coarse integration within 1..528.
 * Target is nominal decoded-sRGB center linear luma .18, tolerance 1/8 EV.
 * Canonical high coarse = clamp(round(normal_coarse*actual_analog_Gr/8)),
 * using a measured native code only to choose safe exploration coverage.
 * Canonical references cover 1/2,3/4,1 of high coarse. Gains .5/.75/1 use
 * high coarse; higher gains use high/2; digital2/4 uses high/4. Thus planned
 * effective doses stay within the reference window instead of dark noise.
 * Native-WB start/mid/end references bracket natural light.
 * no untested high bits, PLL, frame-length, OTP or arbitrary-address writes.
 * No repeated normal setting, retry, background queue or failed-stage bypass.
 * Original five controls are restored on every exit; successful sessions sample
 * the restoration. Capture/readback/ISP drift or log failure stops acquisition.
 * ram-evidence-2 explicitly allocates 28MiB for at most 17 locked YUYV images
 * plus seed/preflight PGM. Capacity failure is fatal; evidence is never dropped.
 * Dynamic stage count is declared as a budget, not fabricated completed stages.
 */
enum { AUTO_CONTRACT_VERSION=6, AUTO_MAX_STAGES=18, AUTO_NORMAL_STEPS=3, AUTO_MIN_COARSE=1, AUTO_MAX_COARSE=528,
       AUTO_SETTLE_COARSE=2, AUTO_SETTLE_GAIN=8, AUTO_MEASURED_FRAMES=4 };
typedef struct {unsigned fps,cam_rate,frame_lines,max_coarse;} AutoProfile;
static const AutoProfile auto_profiles[]={{15,0,704,528},{5,5,2114,1585}};
static const AutoProfile *auto_profile(unsigned fps)
{for(unsigned i=0;i<2;++i)if(auto_profiles[i].fps==fps)return auto_profiles+i;return NULL;}
static const u16 auto_isp_addresses[]={0x32d4,0x32d6,0x32d8,0x32da};
/* User comparison view holds the latest normal native-WB image. Exploration
 * still captures/logs/saves every frame; it does not replace that view. */
static bool auto_preview_normal(const char *label)
{return !strcmp(label,"auto_normal_base") || !strcmp(label,"auto_normal_0") || !strcmp(label,"auto_normal_1") || !strcmp(label,"auto_normal_2");}
static double auto_linear(double x)
{ x=fmax(0,fmin(1,x));return x<=.04045?x/12.92:pow((x+.055)/1.055,2.4); }
static void auto_rgb(unsigned y,int cb,int cr,unsigned matrix,double *rgb)
{
    bool limited=matrix>=2,hd=(matrix&1)!=0;
    double yy=(y-(limited?16.0:0))/(limited?219.0:255.0);
    double u=cb/(limited?224.0:255.0),v=cr/(limited?224.0:255.0);
    rgb[0]=fmax(0,fmin(1,yy+(hd?1.5748:1.402)*v));
    rgb[1]=fmax(0,fmin(1,yy-(hd?.187324:.344136)*u-(hd?.468124:.714136)*v));
    rgb[2]=fmax(0,fmin(1,yy+(hd?1.8556:1.772)*u));
}
static double auto_luma(unsigned y,int cb,int cr,unsigned matrix)
{
    double rgb[3];auto_rgb(y,cb,cr,matrix,rgb);
    return .2126*auto_linear(rgb[0])+.7152*auto_linear(rgb[1])+.0722*auto_linear(rgb[2]);
}
static u16 auto_next_limited(u16 current,double luma,unsigned upper)
{
    if(!isfinite(luma) || luma<0)return 0;
    double error=log2(.18/fmax(luma,1e-4));
    if(fabs(error)<=.125)return current;
    /* Explicit damped feedback; this is a new setting, never a capture retry. */
    long next=lround(current*exp2(fmax(-2,fmin(2,error))*.5));
    if(next<AUTO_MIN_COARSE)next=AUTO_MIN_COARSE;
    if(next>(long)upper)next=upper;
    return (u16)next;
}
static inline u16 auto_next_coarse(u16 current,double luma)
{return auto_next_limited(current,luma,AUTO_MAX_COARSE);}
