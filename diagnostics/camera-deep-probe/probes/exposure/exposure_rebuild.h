#pragma once
/* camera-exposure-rebuild/v1: registered points, two explicit pixel slots and
 * exactly one pending Receive. No acquisition retry, skipped point or queue. */
typedef struct {
    Probe *p;FILE *points,*frames,*bands;u8 *buffers[2],*original;Handle pending;
    unsigned pending_slot,pending_ordinal,armed,point,block,fps,gate;
    u16 coarse,core,isp[4],line,fine,dgain;bool baseline,observe_isp,dense;
    u64 write_begin,write_end;
} Rebuild;
static bool rebuild_state(Rebuild *r,const char *label,int frame,const char *when)
{
    Probe *p=r->p;
    for(unsigned i=0;i<sizeof rebuild_state_registers/sizeof *rebuild_state_registers;++i) {
        u16 a=(u16)rebuild_state_registers[i],v=0;bool mcu=(a&0x8000) || a==0x221f;
        bool ok=read_reg(p,mcu,a,&v,label,frame,when,"rebuild_state")==0;
        if(a==0xa102)ok=ok && v==p->sweep_seq_target && !(v&13);
        if(a==0xa103)ok=ok && !v;
        if(a==0xa104)ok=ok && v==3;
        if(a==0xa355)ok=ok && (v&96)==32;
        if(a==0xa34e || a==0xa34f || a==0xa350)ok=ok && v==128;
        if(a==0x3012)ok=ok && (r->gate?(v>=1 && v<=(r->fps==5?1585:528)):v==r->coarse);
        if(a>=0x3056 && a<=0x305c)ok=ok && v==r->core;
        if(a>=0x32d4 && a<=0x32da)ok=ok && v>=1 && v<=512 && (r->observe_isp || v==r->isp[(a-0x32d4)/2]);
        if(a==0x300a)ok=ok && v==(r->fps==5?2114:704);
        if(a==0x300c){if(!r->baseline)r->line=v;ok=ok && v && v==r->line;}
        if(a==0x3014){if(!r->baseline)r->fine=v;ok=ok && v==r->fine;}
        if(a==0x221f){if(!r->baseline)r->dgain=v;ok=ok && v>=1 && v<=512 && v==r->dgain;}
        if(!ok){fprintf(p->meta,"REBUILD_REJECT point=%u frame=%d when=%s addr=%04x value=%u\n",r->point,frame,when,a,v);return false;}
    }
    r->baseline=true;return true;
}
static bool rebuild_arm(Rebuild *r,unsigned slot)
{
    if(r->pending || slot>1)return false;
    char label[48];snprintf(label,sizeof label,"rb_dma_%04u",r->armed);
    Result rc=GSPGPU_FlushDataCache(r->buffers[slot],BYTES);record_call(r->p,label,"FlushDataCache",0,slot,rc);if(rc!=0)return false;
    rc=CAMU_SetReceiving(&r->pending,r->buffers[slot],r->p->port,BYTES,(s16)r->p->transfer);
    record_call(r->p,label,"SetReceiving",0,slot,rc);
    if(rc!=0 || !r->pending)return false;
    r->pending_slot=slot;r->pending_ordinal=r->armed++;return true;
}
static bool rebuild_capture(Rebuild *r,const char *label,unsigned frame,unsigned count,bool last,double *mean_out)
{
    Probe *p=r->p;u64 before=osGetTime();
    if(!rebuild_state(r,label,(int)frame,"before"))return false;
    unsigned slot=r->pending_slot,ordinal=r->pending_ordinal;Handle event=r->pending;
    if(!event)return false;
    char dma_label[48];snprintf(dma_label,sizeof dma_label,"rb_dma_%04u",ordinal);
    Result wait=svcWaitSynchronization(event,INT64_C(2000000000));u64 waited=osGetTime();
    record_call(p,dma_label,"WaitFrame",0,event,wait);bool received=false,ok=wait==0;
    if(ok){Result rc=PROBE_FINISHED(&received,p->port);record_call(p,dma_label,"IsFinishedReceiving",0,received,rc);ok=rc==0 && received;}
    Result closed=svcCloseHandle(event);record_call(p,dma_label,"CloseDMAEvent",0,event,closed);r->pending=0;ok=ok && closed==0;
    if(ok){Result rc=GSPGPU_InvalidateDataCache(r->buffers[slot],BYTES);record_call(p,dma_label,"InvalidateDataCache",0,slot,rc);ok=rc==0;}
    /* The completed slot is immutable while the other slot receives. A single
     * prefetched frame can straddle the next point's writes; first images are
     * retained as latency evidence, never asserted to be already settled. */
    if(ok && !last)ok=rebuild_arm(r,slot^1u);
    p->pixels=r->buffers[slot];
    s64 latest=0;
    if(ok){Result rc=CAMU_GetLatestVsyncTiming(&latest,p->port,1);record_call(p,dma_label,"GetLatestVsyncTiming",0,1,rc);ok=rc==0 && latest>0;}
    if(ok)ok=rebuild_state(r,label,(int)frame,"after");
    u64 after=osGetTime();
    if(!ok)return false;
    uint64_t sums[16]={0},squares=0,total=0;unsigned low=0,high=0;
    static u8 crop[128*96*2];
    for(unsigned y=0;y<96;++y) {
        const u8 *row=p->pixels+((y+192)*W+256)*2;memcpy(crop+y*256,row,256);
        for(unsigned x=0;x<128;++x){unsigned v=row[x*2];sums[y/6]+=v;total+=v;squares+=(uint64_t)v*v;low+=v<=4;high+=v>=250;}
    }
    double mean=(double)total/(128*96),variance=(double)squares/(128*96)-mean*mean;
    uint32_t crc=probe_crc32(crop,sizeof crop);bool saved=frame<2 || frame>=count-2;
    char image_label[64];snprintf(image_label,sizeof image_label,"rb_p%03u_f%03u",r->point,frame);
    if(saved && !save_frame(p,image_label))return false;
    fprintf(r->frames,"%u,%u,%u,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRId64 ",%.9f,%.9f,%u,%u,%08" PRIx32 ",%u\n",r->point,p->phase,frame,ordinal,slot,(unsigned)event,(uint64_t)r->write_begin,(uint64_t)r->write_end,(uint64_t)before,(uint64_t)waited,(uint64_t)after,(int64_t)latest,mean,variance<0?0:variance,low,high,crc,saved);
    fprintf(r->bands,"%u,%u,%u,%u",r->point,p->phase,frame,ordinal);
    for(unsigned band=0;band<16;++band)fprintf(r->bands,",%.9f",(double)sums[band]/(128*6));
    fputc('\n',r->bands);
    fprintf(p->frames,"%u,%u,%s,%u,0,1,00000000,1,%.9f,0,%u,%u,%08" PRIx32 ",%" PRIu64 ",%" PRIu64 "\n",p->sensor,p->phase,label,frame,mean,low,high,crc,(uint64_t)before,(uint64_t)after);
    ++p->captures;*mean_out=mean;flush_logs(p);
    return !ferror(r->frames) && !ferror(r->bands) && running(p);
}
static bool rebuild_point(Rebuild *r,const char *kind,unsigned coarse,unsigned core,const unsigned isp[4],unsigned frames,unsigned gate,int anchor,bool coverage,bool last,double *score)
{
    Probe *p=r->p;char label[64];snprintf(label,sizeof label,"rb_p%03u",r->point);
    ++p->phase;++p->samples;r->coarse=(u16)coarse;r->core=(u16)core;r->observe_isp=!strncmp(kind,"FD_",3);
    for(unsigned i=0;i<4;++i)r->isp[i]=(u16)isp[i];
    fprintf(p->meta,"REBUILD_POINT index=%u phase=%u block=%u kind=%s fps=%u gate=%u coarse=%u core=%u isp=%u,%u,%u,%u frames=%u anchor=%d coverage=%u\n",r->point,p->phase,r->block,kind,r->fps,gate,coarse,core,isp[0],isp[1],isp[2],isp[3],frames,anchor,coverage);
    r->write_begin=osGetTime();bool ok=true;
    if(gate!=r->gate){p->sweep_seq_target=(u16)((p->sweep_seq_saved&~15u)|gate);ok=write_reg(p,0xa102,p->sweep_seq_target,"rb_gate");r->gate=gate;}
    if(ok)ok=write_reg(p,0x3012,(u16)coarse,label);
    for(unsigned i=1;i<5 && ok;++i)ok=write_reg(p,cal_addresses[i],(u16)core,label);
    for(unsigned i=0;i<4 && ok;++i)ok=write_reg(p,auto_isp_addresses[i],(u16)isp[i],label);
    r->write_end=osGetTime();
    if(ok)ok=rebuild_state(r,label,-1,"configured");
    if(ok && !p->streaming) {
        ok=!r->pending && rebuild_arm(r,0);
        if(ok){char start[40];snprintf(start,sizeof start,"rb_start_b%u",r->block);Result rc=CAMU_StartCapture(p->port);record_call(p,start,"StartCapture",0,0,rc);ok=rc==0;p->streaming=ok;}
    }
    double means[2]={0};
    for(unsigned frame=0;frame<frames && ok && running(p);++frame) {
        if(!strcmp(kind,"FD_off_after") && frame==8) {
            u64 begin=osGetTime();char intervention[48];snprintf(intervention,sizeof intervention,"rb_reassert_p%03u",r->point);
            for(unsigned i=0;i<4 && ok;++i)ok=write_reg(p,auto_isp_addresses[i],128,intervention);
            fprintf(p->meta,"REBUILD_REASSERT point=%u phase=%u frame=8 begin_ms=%" PRIu64 " end_ms=%" PRIu64 " complete=%u\n",r->point,p->phase,(uint64_t)begin,(uint64_t)osGetTime(),ok);
        }
        double value=0;if(ok)ok=rebuild_capture(r,label,frame,frames,last && frame+1==frames,&value);means[frame&1]=value;
    }
    ok=ok && running(p);double result=(means[0]+means[1])/2;if(score)*score=result;
    fprintf(r->points,"%u,%u,%u,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%" PRIu64 ",%" PRIu64 ",%.9f,%u\n",r->point,p->phase,r->block,kind,r->fps,gate,coarse,core,isp[0],isp[1],isp[2],isp[3],frames,anchor,coverage,(uint64_t)r->write_begin,(uint64_t)r->write_end,result,ok);
    fprintf(p->meta,"REBUILD_POINT_END index=%u complete=%u\n",r->point,ok);
    if(ok){++p->successful_samples;++r->point;}return ok && !ferror(r->points);
}
static bool rebuild_snapshot(Rebuild *r,GainMatrixNode *node,const char *label,const char *when)
{
    if(!gain_matrix_snapshot(r->p,node,label,when))return false;
    for(unsigned i=0;i<sizeof rebuild_timing_registers/sizeof *rebuild_timing_registers;++i){u16 v=0;if(read_reg(r->p,false,(u16)rebuild_timing_registers[i],&v,label,-1,when,"rebuild_timing")!=0)return false;}
    fprintf(r->p->meta,"REBUILD_TIMING block=%u when=%s complete=1\n",r->block,when);return true;
}
static bool rebuild_block(Rebuild *r,unsigned fps)
{
    Probe *p=r->p;r->fps=fps;r->gate=0;r->baseline=false;
    fprintf(p->meta,"REBUILD_BLOCK index=%u fps=%u first_point=%u\n",r->block,fps,r->point);
    bool ok=boot_mark(13,r->block,0);
    if(ok && fps==15)ok=gain_matrix_profile(p,15,"rb_profile15");
    if(ok)ok=isp_update_prepare_arm(p,128,"rb_prepare");
    p->sweep_seq_target=(u16)(p->sweep_seq_saved&~15u);
    if(ok)ok=write_reg(p,0xa102,p->sweep_seq_target,"rb_gate");
    GainMatrixNode node={.index=r->block,.fps=fps};char snapshot_label[40];snprintf(snapshot_label,sizeof snapshot_label,"rb_block%u",r->block);
    if(ok)ok=rebuild_snapshot(r,&node,snapshot_label,"before_stream") && boot_mark(14,r->block,0);
    const unsigned unity[4]={128,128,128,128};unsigned n=r->dense?(fps==5?19:15):(fps==5?10:8);const unsigned *grid=r->dense?(fps==5?dense_coarse5:dense_coarse15):sweep_coarse;double scores[19]={0};
    if(ok)ok=rebuild_point(r,"reference_start",128,528,unity,6,0,-1,true,false,NULL);
    for(unsigned i=0;i<n && ok;++i)ok=rebuild_point(r,"coarse",grid[i],528,unity,6,0,(int)i,true,false,&scores[i]);
    unsigned selected=5;bool coverage=false;double distance=1e9;
    for(unsigned i=0;i<n;++i)if(scores[i]>=16 && scores[i]<=224 && fabs(scores[i]-(r->dense?80:128))<distance){distance=fabs(scores[i]-(r->dense?80:128));selected=i;coverage=true;}
    unsigned anchors[2]={selected,selected>2?selected-2:0};
    if(r->dense) {
        bool second=false;distance=1e9;double fine=r->line?(double)r->fine/r->line:0,first=grid[selected]+fine;
        for(unsigned i=0;i<n;++i)if(scores[i]>=16 && scores[i]<=224 && (grid[i]+fine)*(grid[i]+fine)>=2*first*first && fabs(scores[i]-120)<distance){distance=fabs(scores[i]-120);anchors[1]=i;second=true;}
        coverage=coverage && second;if(!coverage){anchors[0]=5;anchors[1]=9;}
    }
    fprintf(p->meta,"REBUILD_ANCHOR block=%u primary=%u secondary=%u coverage=%u\n",r->block,anchors[0],anchors[1],coverage);
    for(unsigned a=0;a<2 && ok;++a)for(unsigned c=0;c<3 && ok;++c)for(unsigned d=0;d<3 && ok;++d){unsigned v=rebuild_ISP_targets[d],gains[4]={v,v,v,v};ok=rebuild_point(r,"gain",grid[anchors[a]],rebuild_core_codes[c],gains,6,0,(int)anchors[a],coverage,false,NULL);}
    for(unsigned i=0;i<4 && ok && !r->dense;++i)ok=rebuild_point(r,"color",grid[selected],528,rebuild_colors[i],6,0,(int)selected,coverage,false,NULL);
    if(ok && !r->dense)ok=rebuild_point(r,"hold",grid[selected],528,unity,128,0,(int)selected,coverage,false,NULL);
    if(ok)ok=rebuild_point(r,"reference_end",128,528,unity,6,0,-1,true,r->dense,NULL);
    for(unsigned i=0;i<3 && ok && !r->dense;++i)ok=rebuild_point(r,i==1?"FD_on":i==0?"FD_off_before":"FD_off_after",4,528,unity,16,i==1?2:0,-1,true,i==2,NULL);
    char stop_label[40];snprintf(stop_label,sizeof stop_label,"rb_stop_b%u",r->block);if(p->streaming || !ok)stop(p,stop_label);
    if(r->pending){char label[48];snprintf(label,sizeof label,"rb_dma_%04u",r->pending_ordinal);Result rc=svcCloseHandle(r->pending);record_call(p,label,"CancelPendingDMA",0,r->pending,rc);r->pending=0;ok=false;}
    ok=ok && !p->streaming && p->capture_ready;
    if(ok)ok=rebuild_snapshot(r,&node,snapshot_label,"after_stop");
    fprintf(p->meta,"REBUILD_BLOCK_END index=%u complete=%u\n",r->block,ok);return ok;
}
static bool exposure_rebuild_probe(Probe *p)
{
    if((p->calibration_part!=CAL_EXPOSURE_REBUILD && p->calibration_part!=CAL_EXPOSURE_DENSE) || p->sensor!=1 || !p->raw || !ram_log.exposure_sweep || p->automation_fps!=5)return false;
    bool dense=p->calibration_part==CAL_EXPOSURE_DENSE;
    fprintf(p->meta,"REBUILD_CONTRACT source=%s blocks=2 points=74 DMA=%u images=296 calibration=0 optical_time=0\n",dense?EXPOSURE_DENSE_SOURCE:EXPOSURE_REBUILD_SOURCE,dense?444:748);
    u16 saved[5]={0},isp[4]={0};bool saved_ok=false;
    bool ok=isp_update_baseline(p,saved,isp,&saved_ok);if(!saved_ok)return false;
    Y2RU_StandardCoefficient matrix=(Y2RU_StandardCoefficient)0;Result matrix_rc=CAMU_GetSuitableY2rStandardCoefficient(&matrix);
    record_call(p,"rb_color_matrix","GetSuitableY2rStandardCoefficient",0,(unsigned)matrix,matrix_rc);
    ok=ok && matrix_rc==0 && (unsigned)matrix<=3;
    if(ok)p->auto_matrix=(unsigned)matrix;
    fprintf(p->meta,"REBUILD_COLOR_MATRIX value=%u verified=%u\n",(unsigned)matrix,matrix_rc==0 && (unsigned)matrix<=3);
    Rebuild r={.p=p,.original=p->pixels,.dense=dense};r.buffers[0]=p->pixels;
#ifdef PROBE_REBUILD_ALLOC
    r.buffers[1]=PROBE_REBUILD_ALLOC(BYTES,0x80);
#elif defined(__3DS__)
    r.buffers[1]=linearMemAlign(BYTES,0x80);
#else
    r.buffers[1]=malloc(BYTES);
#endif
    fprintf(p->meta,"REBUILD_BUFFER extra_bytes=%u slots=2 max_pending=1 allocated=%u crc_scratch=24576\n",BYTES,r.buffers[1]!=NULL);ok=ok && r.buffers[1];
    p->sweep_awb_target=(u16)((p->sweep_awb_saved|32u)&~64u);u16 mode=0;
    if(ok)ok=write_reg(p,0xa355,p->sweep_awb_target,"rb_normalization") && read_reg(p,true,0xa355,&mode,"rb_normalization",-1,"verified","rb_normalization")==0 && (mode&96)==32;
    r.points=probe_fopen("rebuild_points.csv","w");r.frames=probe_fopen("rebuild_frames.csv","w");r.bands=probe_fopen("rebuild_bands.csv","w");ok=ok && r.points && r.frames && r.bands;
    if(r.points)fputs("point,phase,block,kind,fps,gate,coarse,core,isp_r,isp_gr,isp_gb,isp_b,frames,anchor,coverage,write_begin_ms,write_end_ms,score_y,complete\n",r.points);
    if(r.frames)fputs("point,phase,frame,ordinal,slot,event,write_begin_ms,write_end_ms,before_ms,wait_ms,after_ms,vsync_raw,y_mean,y_variance,low_count,high_count,crc32,saved\n",r.frames);
    if(r.bands){fputs("point,phase,frame,ordinal",r.bands);for(unsigned i=0;i<16;++i)fprintf(r.bands,",b%02u",i);fputc('\n',r.bands);}
    if(ok)ok=rebuild_block(&r,5);
    if(ok){r.block=1;ok=rebuild_block(&r,15);}
    if(p->streaming)stop(p,"rb_cleanup");
    if(r.pending){Result rc=svcCloseHandle(r.pending);record_call(p,"rb_cleanup","CancelPendingDMA",0,r.pending,rc);r.pending=0;ok=false;}
    p->pixels=r.original;
#ifdef __3DS__
    linearFree(r.buffers[1]);
#else
    free(r.buffers[1]);
#endif
    bool restored=gain_matrix_profile(p,5,"rb_profile_restore5");
    if(p->sweep_refresh_active){bool mcu=sweep_refresh_restore(p);restored=restored && mcu;}
    bool controls=cal_restore_controls(p,saved,"sweep_restore_controls"),gains=sweep_isp(p,isp,"sweep_restore_isp");restored=restored && controls && gains;
    if(r.points && fclose(r.points))ok=false;
    if(r.frames && fclose(r.frames))ok=false;
    if(r.bands && fclose(r.bands))ok=false;
    bool done=ok && restored && r.point==74 && running(p);
    fprintf(p->meta,"REBUILD_END complete=%u points=%u DMA=%u restored=%u\n",done,r.point,r.armed,restored);p->check_isp=false;p->isp_pin_pending=false;flush_logs(p);return done;
}
