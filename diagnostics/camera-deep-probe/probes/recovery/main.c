/* Explicit SD-only recovery entry: no camera, I2C or process-memory APIs. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <zlib.h>
#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
#endif

enum { MAX_FILES=2048, CHUNK=16384, HTTP_SLICE=262144 };
static unsigned char buffer[CHUNK];
static bool run_name(const char *s)
{
    if(strncmp(s,"run-",4) || !s[4] || strlen(s)>64)return false;
    for(s+=4;*s;++s)if((*s<'0'||*s>'9') && *s!='-')return false;
    return true;
}
static bool file_name(const char *s)
{
    size_t n=strlen(s);
    if(!n || n>120 || s[0]=='.')return false;
    for(const char *p=s;*p;++p)
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("-_.",*p)))return false;
    const char *ext=strrchr(s,'.');
    return ext && (!strcmp(ext,".csv")||!strcmp(ext,".txt")||!strcmp(ext,".pgm")||!strcmp(ext,".yuyv")||!strcmp(ext,".dmp")||!strcmp(ext,".dump"));
}
static bool gzip_write(gzFile out,const void *data,size_t n)
{
    return gzwrite(out,data,(unsigned)n)==(int)n;
}
static bool tar_header(gzFile out,const char *name,unsigned long size)
{
    unsigned char h[512]={0};unsigned sum=0;
    if(strlen(name)>99 || size>512UL*1024*1024)return false;
    memcpy(h,name,strlen(name));snprintf((char *)h+100,8,"%07o",0444);
    snprintf((char *)h+108,8,"%07o",0);snprintf((char *)h+116,8,"%07o",0);
    snprintf((char *)h+124,12,"%011lo",size);snprintf((char *)h+136,12,"%011o",0);
    memset(h+148,' ',8);h[156]='0';memcpy(h+257,"ustar",5);memcpy(h+263,"00",2);
    for(unsigned i=0;i<512;++i)sum+=h[i];
    snprintf((char *)h+148,7,"%06o",sum);h[155]=' ';
    return gzip_write(out,h,sizeof h);
}
static bool tar_padding(gzFile out,size_t n)
{
    unsigned char zero[512]={0};return gzip_write(out,zero,(512-n%512)%512);
}
static bool archive(const char *root,const char *run,char *result,size_t capacity)
{
    char source[512],destination[512],temporary[576],path[680];
    snprintf(source,sizeof source,"%s/probes/%s",root,run);
    snprintf(destination,sizeof destination,"%s/recovery",root);
    if(mkdir(destination,0777) && errno!=EEXIST)return false;
    int n=snprintf(result,capacity,"%s/%s.tar.gz",destination,run);
    if(n<0 || (size_t)n>=capacity)return false;
    struct stat existing;
    if(!stat(result,&existing)) {
        puts("Archive already exists; use --serve-existing to reuse it.");
        return false;
    }
    snprintf(temporary,sizeof temporary,"%s.partial",result);
    DIR *dir=opendir(source);if(!dir)return false;
    gzFile out=gzopen(temporary,"wb6");if(!out){closedir(dir);return false;}
    /* Bounded index; originals are opened only rb and never renamed/deleted. */
    size_t used=0,limit=MAX_FILES*160;char *index=malloc(limit);bool ok=index!=NULL;
    unsigned count=0;uint64_t total=0;struct dirent *entry;
    while(ok && (entry=readdir(dir))) {
        if(!file_name(entry->d_name))continue;
        if(++count>MAX_FILES){ok=false;break;}
        snprintf(path,sizeof path,"%s/%s",source,entry->d_name);
        struct stat info;if(stat(path,&info) || !S_ISREG(info.st_mode) || info.st_size<0){ok=false;break;}
        size_t length=(size_t)info.st_size;total+=length;
        if(total>1024ULL*1024*1024 || !tar_header(out,entry->d_name,(unsigned long)length)){ok=false;break;}
        FILE *input=fopen(path,"rb");if(!input){ok=false;break;}
        size_t left=length;uLong crc=crc32(0,NULL,0);
        while(ok && left) {
            size_t want=left>CHUNK?CHUNK:left,got=fread(buffer,1,want,input);
            if(got!=want || !gzip_write(out,buffer,got)){ok=false;break;}
            crc=crc32(crc,buffer,(uInt)got);left-=got;
        }
        if(fgetc(input)!=EOF || ferror(input))ok=false;
        if(fclose(input))ok=false;
        if(ok)ok=tar_padding(out,length);
        n=snprintf(index?index+used:NULL,index?limit-used:0,"%s\t%lu\t%08lx\n",entry->d_name,(unsigned long)length,(unsigned long)crc);
        if(n<0 || (size_t)n>=limit-used){ok=false;break;}used+=(size_t)n;
        printf("Packed %u: %s\n",count,entry->d_name);fflush(stdout);
    }
    closedir(dir);
    if(ok && count)ok=tar_header(out,"RECOVERY_INDEX.tsv",(unsigned long)used) && gzip_write(out,index,used) && tar_padding(out,used);
    else ok=false;
    unsigned char end[1024]={0};if(ok)ok=gzip_write(out,end,sizeof end);
    free(index);if(gzclose(out)!=Z_OK)ok=false;
    if(ok && rename(temporary,result))ok=false;
    return ok;
}
static bool send_bytes(int fd,const void *data,size_t n)
{
    const char *p=data;
    while(n) {
        struct pollfd poller={fd,POLLOUT,0};
        if(poll(&poller,1,10000)<=0 || !(poller.revents&POLLOUT))return false;
        ssize_t sent=send(fd,p,n,0);
        if(sent<0 && (errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(sent<=0)return false;
        p+=sent;n-=(size_t)sent;
    }
    return true;
}
static void request(int fd,const char *path,unsigned long size,uLong checksum)
{
    char query[1024]={0};size_t used=0;
    while(used<sizeof query-1 && !strstr(query,"\r\n\r\n")) {
        struct pollfd poller={fd,POLLIN,0};if(poll(&poller,1,5000)<=0)return;
        ssize_t got=recv(fd,query+used,sizeof query-1-used,0);if(got<=0)return;
        used+=(size_t)got;query[used]=0;
    }
    char body[128],header[512];
    if(!strncmp(query,"GET /info HTTP/1.",17)) {
        int length=snprintf(body,sizeof body,"FILMRECOVERY 1\n%lu %08lx\n",size,(unsigned long)checksum);
        int n=snprintf(header,sizeof header,"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",length);
        if(send_bytes(fd,header,(size_t)n))send_bytes(fd,body,(size_t)length);
        return;
    }
    unsigned long start,end;int consumed=0;
    char *range=strstr(query,"\r\nRange: bytes=");
    if(strncmp(query,"GET /archive HTTP/1.",20) || !range ||
       sscanf(range,"\r\nRange: bytes=%lu-%lu%n",&start,&end,&consumed)!=2 ||
       strncmp(range+consumed,"\r\n",2) || start>end || end>=size || end-start+1>HTTP_SLICE) {
        const char error[]="HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_bytes(fd,error,sizeof error-1);return;
    }
    FILE *file=fopen(path,"rb");if(!file || fseek(file,(long)start,SEEK_SET)){if(file)fclose(file);return;}
    unsigned long left=end-start+1;
    int n=snprintf(header,sizeof header,"HTTP/1.1 206 Partial Content\r\nContent-Length: %lu\r\nContent-Range: bytes %lu-%lu/%lu\r\nETag: \"%08lx\"\r\nConnection: close\r\n\r\n",left,start,end,size,(unsigned long)checksum);
    bool ok=send_bytes(fd,header,(size_t)n);
    while(ok && left) {
        size_t count=left>CHUNK?CHUNK:(size_t)left;
        ok=fread(buffer,1,count,file)==count && send_bytes(fd,buffer,count);left-=count;
    }
    fclose(file);printf("Range %lu-%lu %s\n",start,end,ok?"sent":"interrupted");fflush(stdout);
}
int main(int argc,char **argv)
{
    const char *run=NULL,*root="sdmc:/3ds/Film3DS";bool pack_only=false,serve_existing=false;unsigned port=5000;
#ifdef __3DS__
    gfxInitDefault();consoleInit(GFX_TOP,NULL);
#endif
    for(int i=1;i<argc;++i) {
        if(!strncmp(argv[i],"--run=",6))run=argv[i]+6;
        else if(!strcmp(argv[i],"--pack-only"))pack_only=true;
        else if(!strcmp(argv[i],"--serve-existing"))serve_existing=true;
#ifndef __3DS__
        else if(!strncmp(argv[i],"--root=",7))root=argv[i]+7;
        else if(!strncmp(argv[i],"--port=",7))port=(unsigned)strtoul(argv[i]+7,NULL,10);
#endif
        else {puts("Unknown recovery option");return 2;}
    }
    if(!run || !run_name(run) || port<1024 || port>65535){puts("Required: --run=run-<digits>-<digits>");return 2;}
    printf("SD recovery only: %s\nNo camera/driver access. Packing originals...\n",run);fflush(stdout);
    char path[560];
    if(serve_existing) {
        int length=snprintf(path,sizeof path,"%s/recovery/%s.tar.gz",root,run);
        if(length<0 || (size_t)length>=sizeof path)return 1;
        gzFile check=gzopen(path,"rb");if(!check){puts("Existing archive unavailable");return 1;}
        int got;
        do {got=gzread(check,buffer,sizeof buffer);}while(got>0);
        bool valid=got==0 && gzeof(check);
        if(gzclose(check)!=Z_OK)valid=false;
        if(!valid){puts("Existing gzip archive failed validation");return 1;}
        puts("Reusing existing validated archive; no compression performed.");
    } else if(!archive(root,run,path,sizeof path)){puts("Archive failed; originals retained");return 1;}
    FILE *file=fopen(path,"rb");if(!file)return 1;
    uLong checksum=crc32(0,NULL,0);unsigned long size=0;size_t n;
    while((n=fread(buffer,1,sizeof buffer,file))){checksum=crc32(checksum,buffer,(uInt)n);size+=(unsigned long)n;}
    bool ok=!ferror(file);fclose(file);if(!ok)return 1;
    printf("ARCHIVE %lu bytes CRC32 %08lx\n",size,(unsigned long)checksum);fflush(stdout);
    if(pack_only)return 0;
#ifdef __3DS__
    void *soc_buffer=memalign(0x1000,0x100000);if(!soc_buffer)return 1;
    Result rc=socInit(soc_buffer,0x100000);
    if(R_FAILED(rc)){printf("socInit %08lx\n",(unsigned long)rc);free(soc_buffer);return 1;}
#endif
    int listener=socket(AF_INET,SOCK_STREAM,0);int reuse=1;
    if(listener<0)return 1;
    setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof reuse);
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons((uint16_t)port);address.sin_addr.s_addr=INADDR_ANY;
    if(bind(listener,(struct sockaddr *)&address,sizeof address) || listen(listener,2)){perror("listen");close(listener);return 1;}
    printf("Recovery HTTP port %u; START exits. Keep open.\n",port);fflush(stdout);
    bool active=true;
    while(active) {
#ifdef __3DS__
        if(!aptMainLoop())break;
        hidScanInput();if(hidKeysDown()&KEY_START)break;
#endif
        struct pollfd poller={listener,POLLIN,0};int ready=poll(&poller,1,100);
        if(ready<0){perror("poll");break;}
        if(!ready)continue;
        int fd=accept(listener,NULL,NULL);
        if(fd<0){perror("accept");break;}
        if(fcntl(fd,F_SETFL,O_NONBLOCK)==0)request(fd,path,size,checksum);
        close(fd);
    }
    close(listener);
#ifdef __3DS__
    socExit();free(soc_buffer);gfxExit();
#endif
    return 0;
}
