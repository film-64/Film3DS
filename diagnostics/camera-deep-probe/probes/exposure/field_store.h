#pragma once
#define FIELD_ROOT "sdmc:/3ds/FilmExposureField"
#define FIELD_LABELS FIELD_ROOT "/labels.csv"
static bool field_load_labels(const char *path,FieldLabel *labels,unsigned *count)
{
    *count=0;FILE *file=fopen(path,"r");
    if(!file)return errno==ENOENT;
    char line[512];bool ok=fgets(line,sizeof line,file) && !strcmp(line,"dose,reference_seconds,run\n");
    while(ok && fgets(line,sizeof line,file)) {
        double dose,seconds;int end=0;
        if(*count==FIELD_MAX_LABELS || sscanf(line,"%lf,%lf,%n",&dose,&seconds,&end)!=2 || !end ||
           !strchr(line+end,'\n') || !isfinite(dose) || dose<=0 || !isfinite(seconds) || seconds<=0){ok=false;break;}
        labels[(*count)++]=(FieldLabel){dose,seconds};
    }
    if(ferror(file))ok=false;
    if(fclose(file))ok=false;
    return ok;
}
static bool field_save_prior(const FieldRun *run)
{
    char path[240];snprintf(path,sizeof path,"%s/model-before.txt",run->directory);
    FILE *file=fopen(path,"w");if(!file)return false;
    const FieldModel *m=&run->prior;
    fprintf(file,"exposure-field/v1\nlog2(reference_A_seconds)=a+b*log2(dose)\nlabels=%u\na=%.15g\nb=%.15g\nholdout_max_ev=%.10g\nvalidated=%u\nunit_slope_assumed=%u\nlog2_domain=%.10g,%.10g\nreference_iso=100\nreference_f_number=2.8\nabsolute_iso_calibrated=0\n",
        m->count,m->intercept,m->slope,m->holdout_ev,m->validated,m->count==1,m->lo,m->hi);
    bool ok=!ferror(file);if(fclose(file))ok=false;return ok;
}
static bool field_save_summary(const FieldRun *run)
{
    char path[240];snprintf(path,sizeof path,"%s/exposure.csv",run->directory);
    FILE *file=fopen(path,"w");if(!file)return false;
    fputs("slot,phase,label,coarse,integration_est_s,dose,predicted_A_s,predicted_ev100,extrapolated,linear_luma,measured\n",file);
    for(unsigned i=0;i<3;++i) {
        const FieldImage *v=run->images+i;
        fprintf(file,"%u,%u,%s,%u,%.12g,%.12g,%.12g,%.12g,%u,%.12g,%u\n",
            i,v->phase,v->label,v->coarse,v->integration,v->dose,v->prediction,
            log2(2.8*2.8/v->prediction),!field_in_domain(&run->prior,v->dose),v->luma,v->measured);
    }
    bool ok=!ferror(file);if(fclose(file))ok=false;return ok;
}
static bool field_save_reference(FieldRun *run,double seconds)
{
    if(!run->complete || !run->converged || run->reference_saved || !isfinite(seconds) || seconds<=0)return false;
    const FieldImage *v=run->images+1;
    if(!v->measured || !isfinite(v->dose) || v->dose<=0)return false;
    FieldLabel labels[FIELD_MAX_LABELS];unsigned n;
    if(!field_load_labels(FIELD_LABELS,labels,&n) || n>=FIELD_MAX_LABELS)return false;
    char path[240];snprintf(path,sizeof path,"%s/reference-A.csv",run->directory);
    FILE *file=fopen(path,"wx");if(!file)return false;
    fputs("meter,iso,f_number,reference_s,reference_ev100,predicted_s,error_ev,precision_ev,recorded_ms\n",file);
    fprintf(file,"A,100,2.8,%.12g,%.12g,%.12g,%.12g,0.1666667,%" PRIu64 "\n",
        seconds,log2(2.8*2.8/seconds),v->prediction,log2(v->prediction/seconds),(uint64_t)osGetTime());
    bool ok=!ferror(file);if(fclose(file))ok=false;if(!ok)return false;
    file=fopen(FIELD_LABELS,n?"a":"w");if(!file)return false;
    if(!n)fputs("dose,reference_seconds,run\n",file);
    fprintf(file,"%.15g,%.15g,%s\n",v->dose,seconds,run->directory);
    ok=!ferror(file);if(fclose(file))ok=false;
    if(ok){run->reference_saved=true;run->reference_seconds=seconds;}
    return ok;
}
