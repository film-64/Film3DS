/* This product includes DNG technology under license by Adobe.
 * TIFF/DNG 1.2, one uncompressed CFA strip. No private tags or recipes.
 * The generic sRGB color matrix is an approximation, not sensor calibration. */
#include "film.h"
#include "film_runtime_contract.h"
#include "storage_internal.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {uint32_t h[4];uint64_t bytes;uint8_t block[64];unsigned used;} Md5;
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned u16(const uint8_t *p){return p[0]|(unsigned)p[1]<<8;}
static void p32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void p16(uint8_t *p,unsigned n){p[0]=n;p[1]=n>>8;}
static void md5_block(Md5 *m,const uint8_t *p)
{
    static const uint32_t k[]={0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
      0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
      0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
      0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
      0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
      0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
      0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
      0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const unsigned shift[]={7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
    uint32_t a=m->h[0],b=m->h[1],c=m->h[2],d=m->h[3];
    for(unsigned i=0;i<64;++i){unsigned round=i/16,g;uint32_t f;
        if(round==0){f=(b&c)|(~b&d);g=i;}else if(round==1){f=(d&b)|(~d&c);g=(5*i+1)%16;}
        else if(round==2){f=b^c^d;g=(3*i+5)%16;}else{f=c^(b|~d);g=7*i%16;}
        uint32_t v=a+f+k[i]+u32(p+4*g);unsigned n=shift[4*round+i%4];
        a=d;d=c;c=b;b+=(v<<n)|(v>>(32-n));
    }
    m->h[0]+=a;m->h[1]+=b;m->h[2]+=c;m->h[3]+=d;
}
static Md5 md5_init(void){return (Md5){.h={0x67452301,0xefcdab89,0x98badcfe,0x10325476}};}
static void md5_add(Md5 *m,const uint8_t *p,size_t n)
{m->bytes+=n;while(n){unsigned take=64-m->used;if(take>n)take=n;memcpy(m->block+m->used,p,take);m->used+=take;p+=take;n-=take;if(m->used==64){md5_block(m,m->block);m->used=0;}}}
static void md5_end(Md5 *m,uint8_t digest[16])
{uint64_t bits=m->bytes*8;uint8_t pad[72]={0x80};unsigned n=m->used<56?56-m->used:120-m->used;md5_add(m,pad,n);for(unsigned i=0;i<8;++i)pad[i]=bits>>(8*i);md5_add(m,pad,8);for(unsigned i=0;i<4;++i)p32(digest+4*i,m->h[i]);}
static bool source_is(const char *s,const char *expected){return s&&!strcmp(s,expected)&&film_runtime_source_valid(s);}

typedef struct {uint8_t data[2048];unsigned entries,end;} Tiff;
static void tag(Tiff *t,unsigned id,unsigned type,unsigned count,const void *data)
{
    unsigned size=type==1||type==2?1:type==3?2:type==4?4:8,bytes=count*size;
    uint8_t *e=t->data+10+12*t->entries++;p16(e,id);p16(e+2,type);p32(e+4,count);
    if(bytes<=4)memcpy(e+8,data,bytes);
    else{p32(e+8,t->end);memcpy(t->data+t->end,data,bytes);t->end=(t->end+bytes+1)&~1u;}
}
static void scalar(Tiff *t,unsigned id,unsigned type,unsigned value)
{uint8_t b[4];p32(b,value);tag(t,id,type,1,b);}
static void text_tag(Tiff *t,unsigned id,const char *s){tag(t,id,2,strlen(s)+1,s);}
FilmStatus film_dng_write(const char *source,const char *path,const FilmFrame *frame)
{
    if(!source_is(source,"film-dng-write/v1"))return FILM_STATE;
    if(!frame||!frame->source.pixels||frame->source.format!=FILM_RAW10_RGGB)return FILM_INVALID;
    unsigned w=frame->source.width,h=frame->source.height;
    if(w<4||h<4||(w|h)&1||(size_t)w>FILM_RAW_MAX_PIXELS/h)return FILM_INVALID;
    size_t bytes=(size_t)w*h*2;Md5 md5=md5_init();uint8_t row[2048];
    /* Preserve every 10-bit sample; encode standard little-endian uint16. */
    for(size_t offset=0;offset<bytes;){size_t n=bytes-offset;if(n>sizeof row)n=sizeof row;
        for(size_t i=0;i<n;i+=2){if(frame->source.pixels[offset+i+1]>3)return FILM_CORRUPT;p16(row+i,((unsigned)frame->source.pixels[offset+i]<<2)|frame->source.pixels[offset+i+1]);}
        md5_add(&md5,row,n);offset+=n;}
    uint8_t digest[16];md5_end(&md5,digest);
    Tiff t={.end=512};memcpy(t.data,"II\052\0",4);p32(t.data+4,8);
    scalar(&t,254,4,0);scalar(&t,256,4,w);scalar(&t,257,4,h);scalar(&t,258,3,16);
    scalar(&t,259,3,1);scalar(&t,262,3,32803);text_tag(&t,271,"Nintendo");text_tag(&t,272,"Nintendo 3DS rear camera");
    scalar(&t,273,4,2048);scalar(&t,274,3,1);scalar(&t,277,3,1);scalar(&t,278,4,h);scalar(&t,279,4,bytes);scalar(&t,284,3,1);
    text_tag(&t,305,"Film3DS");
    if(frame->captured_at){time_t when=(time_t)frame->captured_at;struct tm date;char stamp[20];if(localtime_r(&when,&date)&&strftime(stamp,sizeof stamp,"%Y:%m:%d %H:%M:%S",&date))text_tag(&t,306,stamp);}
    uint8_t dims[]={2,0,2,0},cfa[]={0,1,1,2},version[]={1,2,0,0},planes[]={0,1,2};
    tag(&t,33421,3,2,dims);tag(&t,33422,1,4,cfa);tag(&t,50706,1,4,version);tag(&t,50707,1,4,version);
    text_tag(&t,50708,"Nintendo 3DS rear RGGB (generic uncalibrated color)");tag(&t,50710,1,3,planes);scalar(&t,50711,3,1);scalar(&t,50717,3,1023);
    uint8_t origin[8]={0},crop[8];p32(crop,w);p32(crop+4,h);tag(&t,50719,4,2,origin);tag(&t,50720,4,2,crop);
    const int32_t xyz_to_rgb[]={3240454,-1537139,-498531,-969266,1876011,41556,55643,-204026,1057225};uint8_t matrix[72],neutral[24];
    for(unsigned i=0;i<9;++i){p32(matrix+8*i,(uint32_t)xyz_to_rgb[i]);p32(matrix+8*i+4,1000000);}
    for(unsigned i=0;i<3;++i){p32(neutral+8*i,1);p32(neutral+8*i+4,1);}
    tag(&t,50721,10,9,matrix);tag(&t,50728,5,3,neutral);scalar(&t,50778,3,21);tag(&t,50972,1,16,digest);
    p16(t.data+8,t.entries); /* Zero-filled next-IFD offset terminates TIFF chain. */
    FilmWrite write;FilmStatus s=film_write_begin(&write,path);if(s!=FILM_OK)return s;
    if(fwrite(t.data,1,sizeof t.data,write.file)!=sizeof t.data)s=FILM_IO;
    for(size_t offset=0;s==FILM_OK&&offset<bytes;){size_t n=bytes-offset;if(n>sizeof row)n=sizeof row;
        for(size_t i=0;i<n;i+=2)p16(row+i,((unsigned)frame->source.pixels[offset+i]<<2)|frame->source.pixels[offset+i+1]);
        if(fwrite(row,1,n,write.file)!=n)s=FILM_IO;offset+=n;}
    return film_write_end(&write,s);
}

FilmStatus film_dng_read(const char *source,FILE *file,FilmFrame *out)
{
    if(!source_is(source,"film-dng-read/v1"))return FILM_STATE;
    if(!file||!out||out->source.pixels)return FILM_INVALID;
    uint8_t header[2048];if(fseek(file,0,SEEK_END))return FILM_IO;long length=ftell(file);
    if(length<2048||length>2048+2L*FILM_RAW_MAX_PIXELS||fseek(file,0,SEEK_SET))return FILM_CORRUPT;
    if(fread(header,1,sizeof header,file)!=sizeof header)return FILM_CORRUPT;
    if(memcmp(header,"II\052\0",4)||u32(header+4)!=8)return FILM_UNSUPPORTED;
    unsigned count=u16(header+8);if(!count||count>40||u32(header+10+12*count))return FILM_CORRUPT;
    unsigned w=0,h=0,bits=0,compression=0,photo=0,offset=0,bytes=0,samples=0,rows=0,version=0;
    bool cfa=false,repeat=false;const uint8_t *digest=NULL,*stamp=NULL;unsigned previous=0;
    for(unsigned i=0;i<count;++i){const uint8_t *e=header+10+12*i;unsigned id=u16(e),type=u16(e+2);uint32_t n=u32(e+4);
        unsigned size=type==1||type==2?1:type==3?2:type==4?4:type==5||type==10?8:0;
        if(!size||!n||n>2048/size||id<=previous)return FILM_CORRUPT;previous=id;
        unsigned total=n*size;const uint8_t *p=e+8;
        if(total>4){unsigned pos=u32(e+8);if(pos<512||pos>2048-total)return FILM_CORRUPT;p=header+pos;}
        unsigned v=type==3?u16(p):u32(p);
        switch(id){
        case 256:if(type!=4||n!=1)return FILM_UNSUPPORTED;w=v;break;
        case 257:if(type!=4||n!=1)return FILM_UNSUPPORTED;h=v;break;
        case 258:bits=v;break;case 259:compression=v;break;case 262:photo=v;break;
        case 273:if(type!=4||n!=1)return FILM_UNSUPPORTED;offset=v;break;
        case 277:samples=v;break;case 278:rows=v;break;case 279:bytes=v;break;
        case 306:if(type==2&&n==20&&p[19]==0)stamp=p;break;
        case 33421:repeat=type==3&&n==2&&u16(p)==2&&u16(p+2)==2;break;
        case 33422:cfa=type==1&&n==4&&!memcmp(p,"\0\1\1\2",4);break;
        case 50706:if(type==1&&n==4)version=p[0];break;
        case 50717:if(v!=1023)return FILM_UNSUPPORTED;break;
        case 50972:if(type==1&&n==16)digest=p;break;
        default:break;
        }
    }
    if(w<4||h<4||((w|h)&1)||(size_t)w>FILM_RAW_MAX_PIXELS/h||bits!=16||compression!=1||photo!=32803||
       samples!=1||rows!=h||!repeat||!cfa||version!=1||!digest||offset!=2048||bytes!=(size_t)w*h*2||length!=2048+(long)bytes)return FILM_UNSUPPORTED;
    FilmFrame frame={.recipe=film_default_recipe(),.camera=1};FilmStatus s=film_image_alloc(&frame.source,w,h,FILM_RAW10_RGGB);if(s!=FILM_OK)return s;
    Md5 md5=md5_init();uint8_t row[2048];
    for(size_t pos=0;s==FILM_OK&&pos<bytes;){size_t n=bytes-pos;if(n>sizeof row)n=sizeof row;
        if(fread(row,1,n,file)!=n){s=FILM_CORRUPT;break;}md5_add(&md5,row,n);
        for(size_t i=0;i<n;i+=2){unsigned v=u16(row+i);if(v>1023){s=FILM_CORRUPT;break;}frame.source.pixels[pos+i]=v>>2;frame.source.pixels[pos+i+1]=v&3;}pos+=n;}
    uint8_t actual[16];md5_end(&md5,actual);if(s==FILM_OK&&memcmp(actual,digest,16))s=FILM_CORRUPT;
    if(s==FILM_OK&&stamp){struct tm date={0};int y,month,day,hh,mm,ss;
        if(sscanf((const char *)stamp,"%d:%d:%d %d:%d:%d",&y,&month,&day,&hh,&mm,&ss)==6){date.tm_year=y-1900;date.tm_mon=month-1;date.tm_mday=day;date.tm_hour=hh;date.tm_min=mm;date.tm_sec=ss;date.tm_isdst=-1;time_t t=mktime(&date);if(t>=0)frame.captured_at=t;}}
    if(s==FILM_OK)*out=frame;else film_frame_free(&frame);return s;
}
