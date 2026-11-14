#pragma once
#include "sweep_contract.h"
/* Registered diagnostic scopes. Local phase numbers restart at 3 per run.
 * No skipped stages are reported as measured; every part seeds its own camera.
 * Parts use frame-boundary stdio flushing, retain all original evidence, and
 * stop on any stream error. No filesystem retry or automatic rerun is allowed.
 */
enum { CAL_FULL, CAL_GAIN, CAL_SLOW, CAL_LIMITS, CAL_DIGITAL, CAL_CODES_FINE, CAL_CODES_BOOST, CAL_DIGITAL_HEADROOM, CAL_LIMITS_FFFE, CAL_EXPOSURE_AUTO, CAL_EXPOSURE_FIELD, CAL_EXPOSURE_SWEEP, CAL_EXPOSURE_SWEEP_CHECK, CAL_EXPOSURE_SWEEP_REFRESH, CAL_EXPOSURE_SWEEP_MANUAL, CAL_EXPOSURE_ISP_UPDATE, CAL_NO_RECEIVE_UPDATE, CAL_CAMERA_WRITER_WINDOW, CAL_CAMERA_WRITER_OBSERVE, CAL_HG_ISOLATION, CAL_CCM_NORMALIZATION, CAL_CCM_BYTES, CAL_CCM_BOOT, CAL_GAIN_MATRIX, CAL_EXPOSURE_REBUILD, CAL_EXPOSURE_DENSE };
static inline bool cal_sweep_refresh(unsigned part)
{return part==CAL_EXPOSURE_SWEEP_REFRESH || part==CAL_EXPOSURE_SWEEP_MANUAL || part==CAL_EXPOSURE_ISP_UPDATE || part==CAL_NO_RECEIVE_UPDATE || part==CAL_CAMERA_WRITER_WINDOW || part==CAL_CAMERA_WRITER_OBSERVE || part==CAL_HG_ISOLATION || part==CAL_CCM_NORMALIZATION || part==CAL_CCM_BYTES || part==CAL_CCM_BOOT || part==CAL_GAIN_MATRIX || part==CAL_EXPOSURE_REBUILD || part==CAL_EXPOSURE_DENSE;}
static inline bool cal_sweep_diagnostic(unsigned part)
{return part==CAL_EXPOSURE_SWEEP_CHECK || cal_sweep_refresh(part);}
static inline bool cal_sweep(unsigned part)
{return part==CAL_EXPOSURE_SWEEP || cal_sweep_diagnostic(part);}
static inline bool cal_auto_meter(unsigned part)
{return part==CAL_EXPOSURE_AUTO || part==CAL_EXPOSURE_FIELD || cal_sweep(part);}
static inline const char *cal_scope_name(unsigned part)
{
    static const char *names[]={"full","gain","slow","limits","digital","codes-fine","codes-boost","digital-headroom","limits-fffe","exposure-auto","exposure-field","exposure-sweep","exposure-sweep-check","exposure-sweep-awb-refresh","exposure-sweep-post-refresh-manual","exposure-isp-update","no-receive-frame-update","camera-writer-window","camera-writer-window-v2","camera-hg-isolation","camera-ccm-normalization","camera-ccm-normalization-v2","camera-ccm-normalization-v3","camera-gain-matrix-v4","camera-exposure-rebuild-v2","camera-exposure-dense-v1"};
    return part<26?names[part]:"invalid";
}
static inline unsigned cal_scope_stages(unsigned part)
{
    static const unsigned counts[]={58,14,14,8,6,14,14,6,8,18,8,SWEEP_STAGES,5,5,5,6,4,3,3,5,5,5,5,10,76,76};
    return part<26?counts[part]:0;
}
static inline bool cal_scope_parse(const char *name,unsigned *part)
{
    for(unsigned i=1;i<26;++i)if(!strcmp(name,cal_scope_name(i))){*part=i;return true;}
    return false;
}

static inline bool cal_matrix_scope(unsigned part) {return part==CAL_GAIN_MATRIX || part==CAL_EXPOSURE_REBUILD || part==CAL_EXPOSURE_DENSE;}
