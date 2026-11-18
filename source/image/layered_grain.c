#include "layered_grain.h"
#include "film_runtime_contract.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t crc(const uint8_t *p,size_t n)
{uint32_t v=UINT32_MAX;for(size_t i=0;i<n;++i){v^=p[i];for(unsigned b=0;b<8;++b)v=(v>>1)^((v&1)?UINT32_C(0xedb88320):0);}return v^UINT32_MAX;}
FilmStatus film_grain_texture_load(FILE *file,FilmGrainTexture *out)
{
    enum {SIDE=128,COUNT=9*SIDE*SIDE,BYTES=56+2*COUNT+4};
    if(!file||!out||out->pixels)return FILM_INVALID;
    uint8_t *data=malloc(BYTES);if(!data)return FILM_NOMEM;
    if(fread(data,1,BYTES,file)!=BYTES||fgetc(file)!=EOF||ferror(file)||memcmp(data,"F3GT",4)||
       read32(data+4)!=1||read32(data+8)!=SIDE||read32(data+12)!=SIDE||read32(data+16)!=9||
       read32(data+BYTES-4)!=crc(data,BYTES-4)){free(data);return FILM_CORRUPT;}
    FilmGrainTexture result={.side=SIDE};
    for(unsigned i=0;i<9;++i){uint32_t v=read32(data+20+4*i);memcpy(&result.code[i],&v,4);
        if(!isfinite(result.code[i])||result.code[i]<0||result.code[i]>255||(i&&result.code[i]<=result.code[i-1])){free(data);return FILM_CORRUPT;}}
    result.pixels=malloc(COUNT*sizeof *result.pixels);if(!result.pixels){free(data);return FILM_NOMEM;}
    for(unsigned i=0;i<COUNT;++i){unsigned v=(unsigned)data[56+2*i]|((unsigned)data[57+2*i]<<8);result.pixels[i]=(int16_t)(v<32768?(int)v:(int)v-65536);}
    free(data);
    for(unsigned band=0;band<9;++band){double sum=0,square=0;
        for(unsigned i=0;i<SIDE*SIDE;++i){double v=result.pixels[band*SIDE*SIDE+i]/4096.;sum+=v;square+=v*v;}
        if(fabs(sum/(SIDE*SIDE))>.02||square/(SIDE*SIDE)<.95||square/(SIDE*SIDE)>1.05){free(result.pixels);return FILM_CORRUPT;}}
    *out=result;return FILM_OK;
}
void film_grain_texture_free(FilmGrainTexture *t){if(t){free(t->pixels);*t=(FilmGrainTexture){0};}}

static uint32_t hash(uint32_t seed,int x,int y)
{
    uint32_t v=seed^(uint32_t)x*UINT32_C(0x9e3779b9)^(uint32_t)y*UINT32_C(0x85ebca6b);
    v=(v^(v>>16))*UINT32_C(0x7feb352d);v=(v^(v>>15))*UINT32_C(0x846ca68b);return v^(v>>16);
}
static float random_value(uint32_t v)
{ return ((float)(v&65535)-(float)(v>>16))*(2.449489743f/65535); }
static float field(uint32_t seed,float x,float y)
{
    int ix=(int)floorf(x),iy=(int)floorf(y);float fx=x-ix,fy=y-iy;
    fx=fx*fx*(3-2*fx);fy=fy*fy*(3-2*fy);
    float a=(1-fx)*(1-fy),b=fx*(1-fy),c=(1-fx)*fy,d=fx*fy;
    return (a*random_value(hash(seed,ix,iy))+b*random_value(hash(seed,ix+1,iy))+
            c*random_value(hash(seed,ix,iy+1))+d*random_value(hash(seed,ix+1,iy+1)))/sqrtf(a*a+b*b+c*c+d*d);
}
static float decode(float x) {return x<=.04045f?x/12.92f:powf((x+.055f)/1.055f,2.4f);}
static float encode(float x) {return x<=.0031308f?12.92f*x:1.055f*powf(x,1/2.4f)-.055f;}
static float clamp(float x,float lo,float hi) {return fminf(fmaxf(x,lo),hi);}
static bool valid(float x,float lo,float hi) {return isfinite(x)&&x>=lo&&x<=hi;}

FilmStatus film_grain_gpu_encoding(const char *source,const float *planes,size_t pixels,float *scale)
{
    if(!source||strcmp(source,"film-grain-gpu-mix/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!planes||!scale||!pixels||pixels>4096u*4096u)return FILM_INVALID;
    float peak=0;
    for(size_t i=0;i<pixels*3;++i){if(!isfinite(planes[i]))return FILM_INVALID;peak=fmaxf(peak,fabsf(planes[i]));}
    *scale=peak?fminf(8,40/peak):8;
    return *scale>=3.75f?FILM_OK:FILM_UNSUPPORTED;
}

static float texture_at(const FilmGrainTexture *t,unsigned band,float x,float y)
{
    unsigned ix=(unsigned)floorf(x),iy=(unsigned)floorf(y);float fx=x-ix,fy=y-iy;
    const int16_t *p=t->pixels+band*128*128+iy*128+ix;
    float a=(1-fx)*(1-fy),b=fx*(1-fy),c=(1-fx)*fy,d=fx*fy;
    return (a*p[0]+b*p[1]+c*p[128]+d*p[129])/(4096*sqrtf(a*a+b*b+c*c+d*d));
}
static float texture_field(const FilmGrainTexture *t,uint32_t seed,float code,float u,float v)
{
    unsigned lo=0;while(lo<7&&code>t->code[lo+1])++lo;
    float tone=clamp((code-t->code[lo])/(t->code[lo+1]-t->code[lo]),0,1);
    int ix=(int)floorf(u/32),iy=(int)floorf(v/32);float fx=u/32-ix,fy=v/32-iy;
    fx=fx*fx*(3-2*fx);fy=fy*fy*(3-2*fy);float result=0,squares=0;
    for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){
        uint32_t h=hash(seed,ix+dx,iy+dy);
        float x=u-(ix+dx)*32,y=v-(iy+dy)*32;
        if(h&4)x=-x;
        for(unsigned r=0;r<(h&3);++r){float z=x;x=-y;y=z;}
        x+=32+(h>>8)%63;y+=32+(h>>16)%63;
        float a=texture_at(t,lo,x,y),b=texture_at(t,lo+1,x,y);
        float sample=(a*(1-tone)+b*tone)/sqrtf((1-tone)*(1-tone)+tone*tone);
        float w=(dx?fx:1-fx)*(dy?fy:1-fy);
        w*=1+.2f*field(h,u/9,v/9);result+=w*sample;squares+=w*w;
    }
    return result/sqrtf(squares);
}

FilmStatus film_grain_layered(const char *source,FilmImage *im,const FilmLayeredGrain *p,const FilmGrainProfile *profile)
{return film_grain_layered_texture(source,im,p,profile,NULL);}

static void density_pixel(uint8_t *pixel,const FilmLayeredGrain *p,const float *rms,float n)
{
    float rgb[3]={decode(pixel[0]/255.f),decode(pixel[1]/255.f),decode(pixel[2]/255.f)};
    float l=.2126f*rgb[0]+.7152f*rgb[1]+.0722f*rgb[2],code=clamp(encode(l),0,1);
    if(l<=0||code>=1)return;
    unsigned idx=(unsigned)lroundf(code*255);
    float response=p->shadow*(1-code)*(1-code)+p->mid*2*code*(1-code)+p->highlight*code*code;
    float target=fminf(p->amount*rms[idx]/255*response,fminf(code,1-code)*.5f);
    float derivative=l<=.0031308f?12.92f:1.055f/2.4f*powf(l,1/2.4f-1);
    float sigma=fminf(target/fmaxf(derivative*l,.000001f),.7f);
    float gain=expf(sigma*n-.5f*sigma*sigma);
    for(unsigned k=0;k<3;++k)pixel[k]=(uint8_t)lroundf(clamp(encode(fmaxf(rgb[k]*gain,0)),0,1)*255);
}

FilmStatus film_grain_layered_texture(const char *source,FilmImage *im,const FilmLayeredGrain *p,const FilmGrainProfile *profile,const FilmGrainTexture *texture)
{return film_grain_layered_mix(source,im,p,profile,texture,NULL);}

FilmStatus film_grain_layered_mix(const char *source,FilmImage *im,const FilmLayeredGrain *p,const FilmGrainProfile *profile,const FilmGrainTexture *texture,FilmGrainMixer mix)
{
    if (!source || strcmp(source,"film-grain-layered/v1") || !film_runtime_source_valid(source)) return FILM_STATE;
    if (!im || !im->pixels || im->format!=FILM_RGB24 || !film_image_bytes(im->width,im->height,im->format) ||
        !p || !profile || !valid(p->amount,0,1) || !valid(p->size,.25f,8) || !valid(p->coarse,0,1) ||
        !valid(p->shadow,0,2) || !valid(p->mid,0,2) || !valid(p->highlight,0,2) ||
        !p->reference_width || p->reference_width>4096 || im->width>4096 || im->height>4096) return FILM_INVALID;
    for (unsigned i=0;i<9;++i)
        if (!valid(profile->code[i],0,255) || (i && profile->code[i]<=profile->code[i-1]) ||
            !valid(profile->rms[i],0,32) || !valid(profile->correlation[i],-.95f,.95f)) return FILM_INVALID;
    if(texture){if(texture->side!=128||!texture->pixels)return FILM_INVALID;
        for(unsigned i=0;i<9;++i)if(!valid(texture->code[i],0,255)||(i&&texture->code[i]<=texture->code[i-1]))return FILM_INVALID;}
    if (!p->amount) return FILM_OK;
    float rms[256],corr[256];unsigned lower=0;
    for (unsigned i=0;i<256;++i) {
        while(lower<7 && i>profile->code[lower+1]) ++lower;
        float t=clamp((i-profile->code[lower])/(profile->code[lower+1]-profile->code[lower]),0,1);
        rms[i]=profile->rms[lower]*(1-t)+profile->rms[lower+1]*t;
        corr[i]=profile->correlation[lower]*(1-t)+profile->correlation[lower+1]*t;
    }
    size_t count=(size_t)im->width*im->height;
    float *planes=mix?calloc(count*3,sizeof(float)):NULL,*noise=mix?calloc(count,sizeof(float)):NULL;
    if(mix&&(!planes||!noise)){free(planes);free(noise);return FILM_NOMEM;}
    float scale=(float)im->width/p->reference_width*p->size;
    for (unsigned y=0;y<im->height;++y) for (unsigned x=0;x<im->width;++x) {
        uint8_t *pixel=im->pixels+((size_t)y*im->width+x)*3;
        float rgb[3]={decode(pixel[0]/255.f),decode(pixel[1]/255.f),decode(pixel[2]/255.f)};
        float l=.2126f*rgb[0]+.7152f*rgb[1]+.0722f*rgb[2];
        float code=clamp(encode(l),0,1);
        if (l<=0 || code>=1) continue;
        unsigned idx=(unsigned)lroundf(code*255);
        float u=(x+.5f)/scale,v=(y+.5f)/scale;
        float warp=field(p->seed^UINT32_C(0x561748ac),u/7.1f,v/7.1f);
        float a=field(p->seed,u*.8660254f-v*.5f+.6f*warp,u*.5f+v*.8660254f-.4f*warp);
        if(texture)a=texture_field(texture,p->seed,code*255,u+.6f*warp,v-.4f*warp);
        float b=field(p->seed^UINT32_C(0xb4761da1),u/2.4f+.3f*warp,v/2.4f-.6f*warp);
        float c=field(p->seed^UINT32_C(0xd1b54a35),u/7.1f+.17f,v/7.1f+.91f);
        float wb=.25f+.7f*p->coarse+.2f*corr[idx],wc=.15f*p->coarse;
        float norm=sqrtf(1+wb*wb+wc*wc);
        if(mix){size_t i=((size_t)y*im->width+x)*3;planes[i]=a/norm;planes[i+1]=wb*b/norm;planes[i+2]=wc*c/norm;}
        else density_pixel(pixel,p,rms,(a+wb*b+wc*c)/norm);
    }
    if(mix){FilmStatus status=mix(im->width,im->height,planes,noise);free(planes);
        if(status!=FILM_OK){free(noise);return status;}
        for(size_t i=0;i<count;++i)if(!isfinite(noise[i])){free(noise);return FILM_CORRUPT;}
        for(size_t i=0;i<count;++i)density_pixel(im->pixels+3*i,p,rms,noise[i]);
        free(noise);
    }
    return FILM_OK;
}

static const float decode8[256]={
    0.0000000000e+00f,3.0352698355e-04f,6.0705396710e-04f,9.1058095065e-04f,1.2141079342e-03f,1.5176349177e-03f,1.8211619013e-03f,2.1246888848e-03f,
    2.4282158684e-03f,2.7317428519e-03f,3.0352698355e-03f,3.3465357639e-03f,3.6765073240e-03f,4.0247170185e-03f,4.3914420374e-03f,4.7769534807e-03f,
    5.1815167023e-03f,5.6053916242e-03f,6.0488330229e-03f,6.5120907926e-03f,6.9954101873e-03f,7.4990320432e-03f,8.0231929854e-03f,8.5681256181e-03f,
    9.1340587022e-03f,9.7212173202e-03f,1.0329823030e-02f,1.0960094006e-02f,1.1612245180e-02f,1.2286488357e-02f,1.2983032342e-02f,1.3702083047e-02f,
    1.4443843596e-02f,1.5208514423e-02f,1.5996293366e-02f,1.6807375753e-02f,1.7641954488e-02f,1.8500220128e-02f,1.9382360957e-02f,2.0288563057e-02f,
    2.1219010376e-02f,2.2173884793e-02f,2.3153366178e-02f,2.4157632449e-02f,2.5186859627e-02f,2.6241221895e-02f,2.7320891639e-02f,2.8426039504e-02f,
    2.9556834438e-02f,3.0713443733e-02f,3.1896033073e-02f,3.3104766571e-02f,3.4339806809e-02f,3.5601314875e-02f,3.6889450401e-02f,3.8204371595e-02f,
    3.9546235277e-02f,4.0915196907e-02f,4.2311410621e-02f,4.3735029257e-02f,4.5186204386e-02f,4.6665086337e-02f,4.8171824227e-02f,4.9706565984e-02f,
    5.1269458374e-02f,5.2860647023e-02f,5.4480276442e-02f,5.6128490050e-02f,5.7805430191e-02f,5.9511238163e-02f,6.1246054232e-02f,6.3010017653e-02f,
    6.4803266693e-02f,6.6625938644e-02f,6.8478169844e-02f,7.0360095697e-02f,7.2271850682e-02f,7.4213568380e-02f,7.6185381481e-02f,7.8187421805e-02f,
    8.0219820314e-02f,8.2282707130e-02f,8.4376211544e-02f,8.6500462037e-02f,8.8655586286e-02f,9.0841711183e-02f,9.3058962847e-02f,9.5307466631e-02f,
    9.7587347142e-02f,9.9898728247e-02f,1.0224173309e-01f,1.0461648409e-01f,1.0702310298e-01f,1.0946171078e-01f,1.1193242784e-01f,1.1443537383e-01f,
    1.1697066776e-01f,1.1953842799e-01f,1.2213877223e-01f,1.2477181756e-01f,1.2743768044e-01f,1.3013647669e-01f,1.3286832155e-01f,1.3563332966e-01f,
    1.3843161503e-01f,1.4126329114e-01f,1.4412847086e-01f,1.4702726650e-01f,1.4995978981e-01f,1.5292615200e-01f,1.5592646371e-01f,1.5896083506e-01f,
    1.6202937564e-01f,1.6513219450e-01f,1.6826940019e-01f,1.7144110073e-01f,1.7464740366e-01f,1.7788841598e-01f,1.8116424425e-01f,1.8447499450e-01f,
    1.8782077230e-01f,1.9120168274e-01f,1.9461783044e-01f,1.9806931956e-01f,2.0155625379e-01f,2.0507873639e-01f,2.0863687015e-01f,2.1223075741e-01f,
    2.1586050011e-01f,2.1952619973e-01f,2.2322795732e-01f,2.2696587351e-01f,2.3074004852e-01f,2.3455058216e-01f,2.3839757381e-01f,2.4228112247e-01f,
    2.4620132671e-01f,2.5015828473e-01f,2.5415209433e-01f,2.5818285292e-01f,2.6225065753e-01f,2.6635560480e-01f,2.7049779101e-01f,2.7467731206e-01f,
    2.7889426348e-01f,2.8314874043e-01f,2.8744083773e-01f,2.9177064982e-01f,2.9613827080e-01f,3.0054379442e-01f,3.0498731407e-01f,3.0946892282e-01f,
    3.1398871338e-01f,3.1854677813e-01f,3.2314320911e-01f,3.2777809806e-01f,3.3245153635e-01f,3.3716361505e-01f,3.4191442491e-01f,3.4670405636e-01f,
    3.5153259950e-01f,3.5640014415e-01f,3.6130677978e-01f,3.6625259560e-01f,3.7123768047e-01f,3.7626212299e-01f,3.8132601143e-01f,3.8642943379e-01f,
    3.9157247775e-01f,3.9675523073e-01f,4.0197777983e-01f,4.0724021190e-01f,4.1254261348e-01f,4.1788507085e-01f,4.2326766999e-01f,4.2869049661e-01f,
    4.3415363617e-01f,4.3965717384e-01f,4.4520119452e-01f,4.5078578284e-01f,4.5641102318e-01f,4.6207699965e-01f,4.6778379611e-01f,4.7353149615e-01f,
    4.7932018310e-01f,4.8514994006e-01f,4.9102084985e-01f,4.9693299506e-01f,5.0288645803e-01f,5.0888132085e-01f,5.1491766538e-01f,5.2099557320e-01f,
    5.2711512571e-01f,5.3327640401e-01f,5.3947948901e-01f,5.4572446137e-01f,5.5201140151e-01f,5.5834038963e-01f,5.6471150570e-01f,5.7112482946e-01f,
    5.7758044043e-01f,5.8407841789e-01f,5.9061884092e-01f,5.9720178836e-01f,6.0382733886e-01f,6.1049557081e-01f,6.1720656242e-01f,6.2396039168e-01f,
    6.3075713635e-01f,6.3759687399e-01f,6.4447968197e-01f,6.5140563742e-01f,6.5837481728e-01f,6.6538729828e-01f,6.7244315696e-01f,6.7954246963e-01f,
    6.8668531244e-01f,6.9387176129e-01f,7.0110189193e-01f,7.0837577989e-01f,7.1569350051e-01f,7.2305512892e-01f,7.3046074009e-01f,7.3791040877e-01f,
    7.4540420954e-01f,7.5294221678e-01f,7.6052450468e-01f,7.6815114725e-01f,7.7582221832e-01f,7.8353779153e-01f,7.9129794033e-01f,7.9910273801e-01f,
    8.0695225767e-01f,8.1484657222e-01f,8.2278575440e-01f,8.3076987677e-01f,8.3879901174e-01f,8.4687323151e-01f,8.5499260812e-01f,8.6315721345e-01f,
    8.7136711920e-01f,8.7962239689e-01f,8.8792311788e-01f,8.9626935337e-01f,9.0466117439e-01f,9.1309865179e-01f,9.2158185628e-01f,9.3011085838e-01f,
    9.3868572846e-01f,9.4730653673e-01f,9.5597335325e-01f,9.6468624789e-01f,9.7344529040e-01f,9.8225055033e-01f,9.9110209711e-01f,1.0000000000e+00f
};
float film_grain_linear_luma(const uint8_t p[3])
{return .2126f*decode8[p[0]]+.7152f*decode8[p[1]]+.0722f*decode8[p[2]];}

FilmStatus film_grain_response_prepare(const char *source,const FilmImage *im,const FilmLayeredGrain *p,
    const FilmGrainProfile *profile,const FilmGrainTexture *texture,float *planes,float *sigma,uint8_t *tone)
{
    if(!source||strcmp(source,"film-grain-gpu-compose/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!p||!planes||!sigma||!tone||!valid(p->amount,0,1))return FILM_INVALID;
    FilmLayeredGrain check=*p;check.amount=0;
    FilmStatus status=film_grain_layered_mix("film-grain-layered/v1",(FilmImage *)im,&check,profile,texture,NULL);
    if(status!=FILM_OK)return status;
    float rms[256],corr[256];unsigned lower=0;
    for(unsigned i=0;i<256;++i){
        while(lower<7&&i>profile->code[lower+1])++lower;
        float t=clamp((i-profile->code[lower])/(profile->code[lower+1]-profile->code[lower]),0,1);
        rms[i]=profile->rms[lower]*(1-t)+profile->rms[lower+1]*t;
        corr[i]=profile->correlation[lower]*(1-t)+profile->correlation[lower+1]*t;
    }
    float scale=(float)im->width/p->reference_width*p->size;
    for(unsigned y=0;y<im->height;++y)for(unsigned x=0;x<im->width;++x){
        size_t i=(size_t)y*im->width+x;const uint8_t *pixel=im->pixels+3*i;
        float l=film_grain_linear_luma(pixel);
        float code=clamp(encode(l),0,1);tone[i]=(uint8_t)lroundf(code*255);sigma[i]=0;planes[3*i]=planes[3*i+1]=planes[3*i+2]=0;
        if(l<=0||code>=1||!p->amount)continue;
        unsigned idx=(unsigned)lroundf(code*255);
        float response=p->shadow*(1-code)*(1-code)+p->mid*2*code*(1-code)+p->highlight*code*code;
        float target=fminf(p->amount*rms[idx]/255*response,fminf(code,1-code)*.5f);
        float derivative=l<=.0031308f?12.92f:1.055f/2.4f*powf(l,1/2.4f-1);
        sigma[i]=fminf(target/fmaxf(derivative*l,.000001f),.7f);
        float u=(x+.5f)/scale,v=(y+.5f)/scale;
        float warp=field(p->seed^UINT32_C(0x561748ac),u/7.1f,v/7.1f);
        float a=texture?texture_field(texture,p->seed,code*255,u+.6f*warp,v-.4f*warp):
            field(p->seed,u*.8660254f-v*.5f+.6f*warp,u*.5f+v*.8660254f-.4f*warp);
        float b=field(p->seed^UINT32_C(0xb4761da1),u/2.4f+.3f*warp,v/2.4f-.6f*warp);
        float c=field(p->seed^UINT32_C(0xd1b54a35),u/7.1f+.17f,v/7.1f+.91f);
        float wb=.25f+.7f*p->coarse+.2f*corr[idx],wc=.15f*p->coarse,norm=sqrtf(1+wb*wb+wc*wc);
        planes[3*i]=a/norm;planes[3*i+1]=wb*b/norm;planes[3*i+2]=wc*c/norm;
    }
    return FILM_OK;
}

FilmStatus film_grain_gpu_masks(const char *source,const float planes[3],float sigma,uint8_t tone,uint8_t masks[5])
{
    if(!source||strcmp(source,"film-grain-gpu-compose/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!planes||!masks||!valid(sigma,0,.7f))return FILM_INVALID;
    uint8_t result[5];float gain=1;unsigned accumulator=128;
    for(unsigned i=0;i<3;++i){
        if(!isfinite(planes[i]))return FILM_INVALID;
        float g=expf((sigma*planes[i]-sigma*sigma/6)/2.4f);gain*=g;
        if(!isfinite(g)||g>2||gain>FILM_GPU_GAIN_LIMIT)return FILM_UNSUPPORTED;
        /* Quantize against the cumulative target, so three RGBA8 rounds do
         * not accumulate a positive bias. This only prepares scalar masks;
         * RGB multiplication and clipping remain on the GPU. */
        float target=127.5f*gain;
        if(!accumulator&&target>=.5f)return FILM_UNSUPPORTED;
        float estimate=accumulator?target*255/(2*accumulator):0;
        unsigned lo=(unsigned)clamp(floorf(estimate),0,255),hi=lo<255?lo+1:lo;
        unsigned a=(unsigned)lroundf(fminf(2.f*accumulator*lo/255,255));
        unsigned b=(unsigned)lroundf(fminf(2.f*accumulator*hi/255,255));
        bool upper=fabsf(b-target)<fabsf(a-target);
        result[i]=(uint8_t)(upper?hi:lo);accumulator=upper?b:a;
    }
    /* CPU supplies only the scalar gray response, including the sRGB toe.
     * Color multiplication and the final photo blend are still GPU work. */
    float gray=tone/255.f;
    float bias=encode(decode8[tone]*powf(gain,2.4f))-gray*gain;
    result[3]=(uint8_t)lroundf(clamp(bias,0,1)*255);
    result[4]=(uint8_t)lroundf(clamp(-bias,0,1)*255);
    memcpy(masks,result,sizeof result);return FILM_OK;
}
