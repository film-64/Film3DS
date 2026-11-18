#pragma once
/* Explicit diagnostic contract. These are experiments, never ISO or seconds. */
/* Locked trials require exact control and AE/AWB readback on both sides of
 * every measured frame. Drift rejects the trial; only verified sampled restore
 * may permit an independent next point. Prepare the frame rate before seeding,
 * never repeat its setter after a scoped boundary has locked its baseline. */
enum { CAL_STAGE_COUNT=58, CAL_FIRST_PHASE=3, CAL_LAST_PHASE=60,
       CAL_BURST_FRAMES=24, CAL_ROI_W=128, CAL_ROI_H=96 };
static const unsigned cal_rates[]={8,0,5}; /* fixed 30,15,5 fps */
static const u16 cal_addresses[]={0x3012,0x3056,0x3058,0x305a,0x305c};
/* Empirical raw-code matrix, not a table of verified gain multipliers. */
static const u16 cal_fine_codes[]={0x210,0x218,0x220,0x228,0x230,0x23c};
static const u16 cal_boost_codes[]={0x240,0x260,0x2a0,0x2bf,0x300,0x320};
/* Driver read handler rejects the value FFFF as a sentinel. A distinct scope
 * tests adjacent FFFE, retaining the original failed FFFF evidence unchanged. */
static const u16 cal_nonsentinel_coarse=0xfffe;
static bool cal_rule(const char *kind,unsigned mask)
{
    if(!strcmp(kind,"auto_gain") || !strcmp(kind,"auto_digital"))return mask==31;
    if(!strcmp(kind,"seed"))return mask==0;
    /* Raw code scans and digital-headroom explicitly shorten integration;
     * the raw scans make no gain multiplier claim and use equal channel codes.
     * A rejected target may advance only after a fully verified sampled restore;
     * it remains an incomplete measurement, never a success or a retry.
     */
    if(!strcmp(kind,"code_baseline"))return mask==31;
    if(!strcmp(kind,"code"))return mask==30;
    if(!strcmp(kind,"normalize") || !strcmp(kind,"analog") || !strcmp(kind,"digital"))return mask==30;
    if(!strcmp(kind,"integration") || !strcmp(kind,"integration_long"))return mask==1;
    if(!strcmp(kind,"restore"))return mask==1 || mask==30;
    return false;
}
static double cal_analog(u16 value)
{
    return (value&127)/32.0 * (1+((value>>7)&1)) * (1+((value>>8)&1));
}
static bool cal_encode_analog(u16 base,double gain,u16 *out)
{
    if((base&0xf000) || gain<1.0/32 || gain>127.0/8)return false;
    double error=1e9;unsigned best=0;
    for(unsigned stage=0;stage<4;++stage) {
        unsigned multiplier=(1+(stage&1))*(1+((stage>>1)&1));
        unsigned initial=(unsigned)(gain*32/multiplier+.5);
        if(initial<1 || initial>127)continue;
        double difference=fabs(initial*multiplier/32.0-gain);
        if(difference<error){error=difference;best=initial|(stage<<7);}
    }
    if(error==1e9)return false;
    *out=(u16)((base&~0x1ffu)|best);return true;
}
static bool cal_digital(u16 base,unsigned multiplier,u16 *out)
{
    if((base&0xf000) || multiplier<1 || multiplier>7)return false;
    *out=(u16)((base&~0xe00u)|(multiplier<<9));return true;
}
