#pragma once
/* CONTRACT_MATRIX: exposure-field/v1, local hardware diagnostic only.
 * Entry: field_capture, explicitly requested by A; never automatic/retried.
 * Compute: bounded Sensor1 capture, not Vision RegionTask/LaneAdmission.
 * Proof: SD reads.csv before/after every measured frame, manifest + field.csv.
 * Profile: 5fps, coarse 1..1585; native analog color preserved, core digital1x.
 * ISP pinned once after initial settling, never accepted again after drift.
 * Stages: seed, normal_base, <=3 normal decisions, low/high +/-1EV candidates,
 * normal_check. Maximum8; clamped duplicate bounds recorded, not fabricated.
 * Four measured frames; settle8 for gain/color,2 for integration. Span and
 * same-setting end/start drift >.125EV fail; restore original controls on exit.
 * SD stream/readback/capture failure stops, no skip/retry/fallback/queue.
 * Each A starts a NEW run directory. No network needed for measurement.
 * Reference: user A-meter ISO100/f2.8 shutter, captured AFTER frozen prediction.
 * Model: log2(reference seconds)=a+b*log2(sensor dose). Parameters computed
 * from saved labels; n=1 uses EXPLICIT provisional unit slope, n>=2 fits slope.
 * Prior model/prediction is immutable for this run; new label affects NEXT run.
 * Three photos are candidates, not certified correct exposure boundaries.
 * Side effects: SD evidence + local model labels; no portable metadata export.
 * Y opens an explicit SD-run picker then the existing read-only recovery
 * archive/HTTP boundary. It creates no camera work; exits to launcher afterward.
 */
enum { FIELD_VERSION=1, FIELD_MAX_STAGES=8, FIELD_MAX_LABELS=128 };
typedef struct {double dose,seconds;} FieldLabel;
typedef struct {unsigned count;double intercept,slope,lo,hi,holdout_ev;bool fitted,validated;} FieldModel;
static FieldModel field_fit(const FieldLabel *labels,unsigned n)
{
    FieldModel m={.count=n,.slope=1,.lo=INFINITY,.hi=-INFINITY,.holdout_ev=INFINITY};
    if(!n || n>FIELD_MAX_LABELS)return m;
    double sx=0,sy=0,sxx=0,sxy=0;
    for(unsigned i=0;i<n;++i) {
        if(!isfinite(labels[i].dose)||!isfinite(labels[i].seconds)||labels[i].dose<=0||labels[i].seconds<=0)return m;
        double x=log2(labels[i].dose),y=log2(labels[i].seconds);
        sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;m.lo=fmin(m.lo,x);m.hi=fmax(m.hi,x);
    }
    double denom=n*sxx-sx*sx;
    if(n>=2 && denom>1e-9)m.slope=(n*sxy-sx*sy)/denom;
    m.intercept=(sy-m.slope*sx)/n;
    m.fitted=isfinite(m.intercept)&&isfinite(m.slope)&&m.slope>0;
    if(n>=4 && m.fitted) {
        double worst=0;
        for(unsigned i=0;i<n;++i) {
            double x=log2(labels[i].dose),y=log2(labels[i].seconds);
            double ax=sx-x,ay=sy-y,axx=sxx-x*x,axy=sxy-x*y;
            double d=(n-1)*axx-ax*ax;
            if(d<=1e-9){worst=INFINITY;break;}
            double b=((n-1)*axy-ax*ay)/d,a=(ay-b*ax)/(n-1);
            if(!isfinite(b)||b<=0){worst=INFINITY;break;}
            worst=fmax(worst,fabs(a+b*x-y));
        }
        m.holdout_ev=worst;m.validated=worst<=.25;
    }
    return m;
}
static double field_predict(const FieldModel *m,double dose)
{return m->fitted && isfinite(dose) && dose>0?exp2(m->intercept+m->slope*log2(dose)):NAN;}
static bool field_in_domain(const FieldModel *m,double dose)
{double x=log2(dose);return m->count>=2 && x>=m->lo && x<=m->hi;}
