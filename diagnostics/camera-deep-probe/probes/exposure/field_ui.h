#pragma once
#ifdef __3DS__
static void field_show_image(const FieldRun *run,unsigned slot,u8 *pixels)
{
    const FieldImage *v=run->images+slot;if(!v->measured)return;
    char path[256];snprintf(path,sizeof path,"%s/s1-p%03u-%s.yuyv",run->directory,v->phase,v->label);
    FILE *file=fopen(path,"rb");if(!file)return;
    bool ok=fread(pixels,1,BYTES,file)==BYTES;if(fclose(file))ok=false;
    if(!ok)return;
    u16 w,h;u8 *screen=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,&w,&h);
    if(gspGetBytesPerPixel(gfxGetScreenFormat(GFX_BOTTOM))==3 && field_preview_draw(screen,(size_t)w*h*3,w,h,pixels,run->matrix))gfxFlushBuffers();
}
static void field_screen(const FieldRun *run,unsigned slot,bool have_run)
{
    printf("\x1b[2J\x1b[1;1HFilmExposureField v1 | OFFLINE SD\n");
    puts("A: NEW capture  X: enter A-meter shutter\nL/R: darker / normal / brighter photo\nY: choose saved run for network recovery\nSTART: exit; B stops capture\n");
    puts("Reference display: ISO100 f/2.8\nModel predicts reference-A shutter\nMeter: boxed center 4%, native color\n");
    if(!have_run){puts("Move to light, aim at SAME meter target.\nPress A to measure first.\nNo new reference is assumed.\n");return;}
    const FieldImage *v=run->images+slot;
    printf("Capture: %s  target: %s\n",run->complete?"proof PASS":"INCOMPLETE",run->converged?"reached":"not reached");
    printf("Prior labels %u; model %s\n",run->prior.count,run->prior.validated?"LOO pass":"PROVISIONAL");
    printf("View: %s | %s\n",slot==0?"DARK":slot==1?"NORMAL":"BRIGHT",v->measured?"measured":"clamped/unavailable");
    if(v->measured) {
        printf("Sensor shutter SIM %.6fs (C%u)\n",v->integration,v->coarse);
        if(isfinite(v->prediction)) {
            printf("A shutter PRED %.5fs ~1/%.1f\n",v->prediction,1/v->prediction);
            printf("Pred EV100 %.3f | %s\n",log2(7.84/v->prediction),field_in_domain(&run->prior,v->dose)?"inside fit range":"EXTRAPOLATED");
        } else puts("A shutter prediction: needs label");
    }
    if(run->reference_saved) {
        printf("A reference %.5fs EV100 %.3f\n",run->reference_seconds,log2(7.84/run->reference_seconds));
        printf("Normal prediction error %+.3f EV\n",log2(run->images[1].prediction/run->reference_seconds));
    } else puts("A reference: NOT ENTERED (X after run)");
    printf("Normal-check drift %+.3f EV\n",run->drift_ev);
    puts("Prediction frozen BEFORE your reference.\nNew label affects NEXT capture only.\nSD: /3ds/Film3DS/probes/run-...\nSensor seconds/gain model are provisional.");
}
static bool field_reference_edit(FieldRun *run)
{
    if(!run->complete || !run->converged || run->reference_saved)return true;
    /* Start with a neutral editor value, never copy the predicted shutter. */
    double tv=0;
    while(aptMainLoop()) {
        double seconds=exp2(-tv);
        printf("\x1b[2J\x1b[1;1HENTER ACTUAL A METER\nISO100 f/2.8 fixed reference\n\nShutter %.5fs ~1/%.2f\nEV100 %.3f\n\nUp/down: shutter 1/6 stop\nLeft/right: shutter 1 stop\nA: save actual reading\nB: cancel\n\nDo not enter prediction as a label.\n",seconds,1/seconds,log2(7.84/seconds));
        u32 keys=0;
        while(aptMainLoop() && !keys){hidScanInput();keys=hidKeysDown();gspWaitForVBlank();}
        if(keys&(KEY_B|KEY_START))return true;
        if(keys&KEY_A)return field_save_reference(run,seconds);
        if(keys&KEY_UP)tv+=1.0/6;
        if(keys&KEY_DOWN)tv-=1.0/6;
        if(keys&KEY_RIGHT)tv+=1;
        if(keys&KEY_LEFT)tv-=1;
        tv=fmax(-5,fmin(16,tv));
    }
    return true;
}
static int field_export(void)
{
    char names[128][65];unsigned n=0,selected=0;
    DIR *dir=opendir("sdmc:/3ds/Film3DS/probes");if(!dir)return -1;
    struct dirent *entry;
    while((entry=readdir(dir)) && n<128) {
        size_t length=strlen(entry->d_name);
        if(length>64 || !run_name(entry->d_name))continue;
        char path[512];snprintf(path,sizeof path,"sdmc:/3ds/Film3DS/probes/%s/exposure.csv",entry->d_name);
        struct stat st;if(stat(path,&st) || !S_ISREG(st.st_mode))continue;
        memcpy(names[n++],entry->d_name,length+1);
    }
    closedir(dir);if(!n)return -1;
    while(aptMainLoop()) {
        printf("\x1b[2J\x1b[1;1HREAD-ONLY SD RECOVERY\n\nRun %u / %u\n%s\n\nUp/down: choose saved run\nA: pack and serve HTTP5000\nB: cancel\n\nNo camera work; SD originals retained.\nSTART in server exits to launcher.\n",selected+1,n,names[selected]);
        u32 keys=0;while(aptMainLoop() && !keys){hidScanInput();keys=hidKeysDown();gspWaitForVBlank();}
        if(keys&(KEY_B|KEY_START))return -1;
        if(keys&KEY_UP)selected=(selected+n-1)%n;
        if(keys&KEY_DOWN)selected=(selected+1)%n;
        if(keys&KEY_A) {
            char option[80],archive_path[256];snprintf(option,sizeof option,"--run=%s",names[selected]);
            snprintf(archive_path,sizeof archive_path,"sdmc:/3ds/Film3DS/recovery/%s.tar.gz",names[selected]);
            struct stat st;bool existing=!stat(archive_path,&st);
            gfxExit();char *args[]={"FilmRecovery",option,"--serve-existing"};
            return film_recovery_entry(existing?3:2,args);
        }
    }
    return -1;
}
static int field_entry(void)
{
    mkdir("sdmc:/3ds",0777);mkdir(FIELD_ROOT,0777);
    FieldRun run={0};bool have_run=false;unsigned slot=1;
    u8 *view=linearMemAlign(BYTES,0x80);if(!view){puts("Memory allocation failed");return 1;}
    field_screen(&run,slot,have_run);
    while(aptMainLoop()) {
        hidScanInput();u32 keys=hidKeysDown();
        if(keys&KEY_START)break;
        if(keys&KEY_A) {
            FieldLabel labels[FIELD_MAX_LABELS];unsigned n;
            if(!field_load_labels(FIELD_LABELS,labels,&n)){puts("STOP: model label file invalid/unreadable");break;}
            run=(FieldRun){.prior=field_fit(labels,n)};
            Probe p={.calibration_part=CAL_EXPOSURE_FIELD,.automation_fps=5,.raw=true,.frame_log_flush=true,.first_phase=3,.pixels=view};
            if(open_logs(&p)) {
                snprintf(run.directory,sizeof run.directory,"%s",p.directory);
                bool saved=field_save_prior(&run);
                if(saved && open_camera(&p,1)) {
                    if(acquisition_preflight(&p))field_capture(&p,&run);
                    close_camera(&p);
                }
                fprintf(p.meta,"FIELD_SESSION complete=%u converged=%u aborted=%u io_error=%u\n",run.complete,run.converged,p.aborted,p.io_error);
                flush_logs(&p);
                FILE *files[]={p.calls,p.reads,p.frames,p.meta,p.calibration,p.burst,p.roi,p.noise,p.execution};
                bool closed=true;for(unsigned i=0;i<sizeof files/sizeof *files;++i)if(files[i] && fclose(files[i]))closed=false;
                if(!saved || !closed || p.io_error || !field_save_summary(&run)) {
                    run.complete=false;puts("STOP: SD write error, partial evidence kept");break;
                }
            } else {puts("STOP: cannot create SD run");break;}
            have_run=true;slot=1;field_show_image(&run,slot,view);field_screen(&run,slot,have_run);
        } else if(keys&KEY_X) {
            if(have_run && !field_reference_edit(&run)){puts("STOP: reference SD write failed");break;}
            field_screen(&run,slot,have_run);
        } else if(have_run && (keys&(KEY_L|KEY_R))) {
            slot=(slot+((keys&KEY_R)?1:2))%3;
            field_show_image(&run,slot,view);field_screen(&run,slot,have_run);
        } else if(keys&KEY_Y) {
            int exported=field_export();
            if(exported>=0){linearFree(view);return exported;}
            field_screen(&run,slot,have_run);
        }
        gspWaitForVBlank();
    }
    linearFree(view);gfxExit();return 0;
}
#endif
