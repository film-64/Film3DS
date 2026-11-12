#pragma once
/* Registered hardware diagnostic source: exposure-auto/v4.
 * Sensor1, registered 15fps/5fps profiles; max18 stages, <=3 normal decisions.
 * Two settling and four measured frames; a locked stage with >1/8EV sample
 * brightness span is rejected, not retried or averaged into a stable result.
 * ISP lock is established ONCE after the settling frames, immediately before
 * the first locked measured frame. It cannot be re-established after drift.
 * Normal work preserves native analogue color ratios, forces core digital 1x, pins all four ISP
 * digital settings, and changes ONLY coarse integration within 1..528.
 * Target is nominal decoded-sRGB center linear luma .18, tolerance 1/8 EV.
 * Canonical high coarse = clamp(round(normal_coarse*actual_analog_Gr/8)),
 * using a measured native code only to choose safe exploration coverage.
 * Canonical exploration uses equal-channel fine codes; code0220 is a high
 * response reference. Other gains use normal_coarse/2, digital uses /4, and
 * a short reference uses /5. Native-WB start/mid/end references bracket light.
 * no untested high bits, PLL, frame-length, OTP or arbitrary-address writes.
 * No repeated normal setting, retry, background queue or failed-stage bypass.
 * Original five controls are restored on every exit; successful sessions sample
 * the restoration. Capture/readback/ISP drift or log failure stops acquisition.
 * ram-evidence-2 explicitly allocates 28MiB for at most 17 locked YUYV images
 * plus seed/preflight PGM. Capacity failure is fatal; evidence is never dropped.
 * Dynamic stage count is declared as a budget, not fabricated completed stages.
 */
enum { AUTO_CONTRACT_VERSION=4, AUTO_MAX_STAGES=18, AUTO_NORMAL_STEPS=3, AUTO_MIN_COARSE=1, AUTO_MAX_COARSE=528,
       AUTO_SETTLE_FRAMES=2, AUTO_MEASURED_FRAMES=4 };
typedef struct {unsigned fps,cam_rate,frame_lines,max_coarse;} AutoProfile;
static const AutoProfile auto_profiles[]={{15,0,704,528},{5,5,2114,1585}};
static const AutoProfile *auto_profile(unsigned fps)
{for(unsigned i=0;i<2;++i)if(auto_profiles[i].fps==fps)return auto_profiles+i;return NULL;}
static const u16 auto_isp_addresses[]={0x32d4,0x32d6,0x32d8,0x32da};
static double auto_linear(double x)
{ x=fmax(0,fmin(1,x));return x<=.04045?x/12.92:pow((x+.055)/1.055,2.4); }
static double auto_luma(unsigned y,int cb,int cr,unsigned matrix)
{
    bool limited=matrix>=2,hd=(matrix&1)!=0;
    double yy=(y-(limited?16.0:0))/(limited?219.0:255.0);
    double u=cb/(limited?224.0:255.0),v=cr/(limited?224.0:255.0);
    double r=yy+(hd?1.5748:1.402)*v;
    double g=yy-(hd?.187324:.344136)*u-(hd?.468124:.714136)*v;
    double b=yy+(hd?1.8556:1.772)*u;
    return .2126*auto_linear(r)+.7152*auto_linear(g)+.0722*auto_linear(b);
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
