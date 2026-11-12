#pragma once
/* Registered diagnostic scopes. Local phase numbers restart at 3 per run.
 * No skipped stages are reported as measured; every part seeds its own camera.
 * Parts use frame-boundary stdio flushing, retain all original evidence, and
 * stop on any stream error. No filesystem retry or automatic rerun is allowed.
 */
enum { CAL_FULL, CAL_GAIN, CAL_SLOW, CAL_LIMITS };
static inline const char *cal_scope_name(unsigned part)
{
    static const char *names[]={"full","gain","slow","limits"};
    return part<4?names[part]:"invalid";
}
static inline unsigned cal_scope_stages(unsigned part)
{
    static const unsigned counts[]={58,14,14,8};
    return part<4?counts[part]:0;
}
static inline bool cal_scope_parse(const char *name,unsigned *part)
{
    for(unsigned i=1;i<4;++i)if(!strcmp(name,cal_scope_name(i))){*part=i;return true;}
    return false;
}
