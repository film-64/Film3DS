#pragma once
static uint32_t cal_sum[CAL_ROI_W*CAL_ROI_H],cal_square[CAL_ROI_W*CAL_ROI_H];
static unsigned cal_noise_count;
static void cal_checkpoint(Probe *p,const char *label,int frame,const char *step)
{
    if(p->execution){fprintf(p->execution,"%u,%u,%s,%d,%s\n",p->sensor,p->phase,label,frame,step);if(!p->frame_log_flush)fflush(p->execution);}
}
static bool cal_preview_draw(u8 *screen,size_t bytes,unsigned width,unsigned height,const u8 *pixels)
{
    if(!screen || !pixels || width!=240 || height!=320 || bytes<320*240*3)return false;
    for(unsigned x=0;x<320;++x)for(unsigned y=0;y<240;++y) {
        unsigned v=pixels[((y*2)*W+x*2)*2],at=3*(x*240+239-y);
        screen[at]=screen[at+1]=screen[at+2]=(u8)v;
    }
    return true;
}
static void cal_noise_reset(void)
{
    memset(cal_sum,0,sizeof cal_sum);memset(cal_square,0,sizeof cal_square);cal_noise_count=0;
}
static void cal_image(Probe *p,const char *label,int frame)
{
    cal_checkpoint(p,label,frame,"roi_begin");
    for(unsigned tile=0;tile<10;++tile) {
        unsigned x=256,y=192,w=CAL_ROI_W,h=CAL_ROI_H;
        if(tile) {
            unsigned col=(tile-1)%3,row=(tile-1)/3;
            x=col*W/3;y=row*H/3;w=(col+1)*W/3-x;h=(row+1)*H/3-y;
        }
        uint64_t sum=0,square=0;unsigned low=0,high=0;
        for(unsigned yy=0;yy<h;++yy)for(unsigned xx=0;xx<w;++xx) {
            unsigned v=p->pixels[((y+yy)*W+x+xx)*2];sum+=v;square+=v*v;
            low+=v<=16;high+=v>=235;
            if(!tile){unsigned i=yy*w+xx;cal_sum[i]+=v;cal_square[i]+=v*v;}
        }
        double mean=sum/(double)(w*h),variance=square/(double)(w*h)-mean*mean;
        fprintf(p->roi,"%u,%u,%s,%d,%u,%u,%u,%u,%u,%.8f,%.8f,%u,%u\n",
            p->sensor,p->phase,label,frame,tile,x,y,w,h,mean,variance,low,high);
    }
    ++cal_noise_count;
    if(p->calibration_part==CAL_EXPOSURE_AUTO) {
        double sum=0;
        for(unsigned y=192;y<288;++y)for(unsigned x=256;x<384;x+=2) {
            const u8 *q=p->pixels+((y*W+x)*2);
            sum+=auto_luma(q[0],q[1]-128,q[3]-128,p->auto_matrix);
            sum+=auto_luma(q[2],q[1]-128,q[3]-128,p->auto_matrix);
        }
        p->auto_last_luma=sum/(CAL_ROI_W*CAL_ROI_H);
        p->auto_luma_sum+=p->auto_last_luma;
        p->auto_luma_min=fmin(p->auto_luma_min,p->auto_last_luma);
        p->auto_luma_max=fmax(p->auto_luma_max,p->auto_last_luma);
    }
    cal_checkpoint(p,label,frame,"roi_end");
#ifdef __3DS__
    if(p->calibration_part!=CAL_EXPOSURE_AUTO || auto_preview_normal(label)) {
    u16 width,height;u8 *screen=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,&width,&height);
    cal_checkpoint(p,label,frame,"preview_begin");
    unsigned bpp=gspGetBytesPerPixel(gfxGetScreenFormat(GFX_BOTTOM));
    if(bpp==3 && cal_preview_draw(screen,(size_t)width*height*bpp,width,height,p->pixels)) {
        gfxFlushBuffers();
    }
    cal_checkpoint(p,label,frame,"preview_end");
    }
#endif
}
static void cal_noise_write(Probe *p,const char *label)
{
    double mean=0,variance=0;unsigned n=cal_noise_count;
    if(n<2)return;
    for(unsigned i=0;i<CAL_ROI_W*CAL_ROI_H;++i) {
        mean+=cal_sum[i]/(double)n;
        variance+=(cal_square[i]-cal_sum[i]*(double)cal_sum[i]/n)/(n-1);
    }
    mean/=CAL_ROI_W*CAL_ROI_H;variance/=CAL_ROI_W*CAL_ROI_H;
    fprintf(p->noise,"%u,%u,%s,%u,%u,%u,%.8f,%.8f,%.8f\n",p->sensor,p->phase,label,n,
        CAL_ROI_W,CAL_ROI_H,mean,variance,sqrt(fmax(0,variance)));
}
