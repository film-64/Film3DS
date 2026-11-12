#pragma once
/* Registered hardware diagnostic source: exposure-auto/v1.
 * One Sensor1/15fps session; max 15 stages, max 3 distinct normal decisions.
 * Normal work locks equal composite 0220 (core digital 1x), pins all four ISP
 * digital settings, and changes ONLY coarse integration within 1..528.
 * Target is nominal decoded-sRGB center linear luma .18, tolerance 1/8 EV.
 * Exploration uses six existing raw fine codes and core digital 2x/4x at ONE shortened coarse;
 * no untested high bits, PLL, frame-length, OTP or arbitrary-address writes.
 * No repeated normal setting, retry, background queue or failed-stage bypass.
 * Original five controls are restored on every exit; successful sessions sample
 * the restoration. Capture/readback/ISP drift or log failure stops acquisition.
 * ram-evidence-2 explicitly allocates 28MiB for at most 14 locked YUYV images
 * plus seed/preflight PGM. Capacity failure is fatal; evidence is never dropped.
 * Dynamic stage count is declared as a budget, not fabricated completed stages.
 */
enum { AUTO_MAX_STAGES=15, AUTO_NORMAL_STEPS=3, AUTO_MIN_COARSE=1, AUTO_MAX_COARSE=528 };
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
static u16 auto_next_coarse(u16 current,double luma)
{
    if(!isfinite(luma) || luma<0)return 0;
    double error=log2(.18/fmax(luma,1e-4));
    if(fabs(error)<=.125)return current;
    /* Explicit damped feedback; this is a new setting, never a capture retry. */
    long next=lround(current*exp2(fmax(-2,fmin(2,error))*.5));
    if(next<AUTO_MIN_COARSE)next=AUTO_MIN_COARSE;
    if(next>AUTO_MAX_COARSE)next=AUTO_MAX_COARSE;
    return (u16)next;
}
