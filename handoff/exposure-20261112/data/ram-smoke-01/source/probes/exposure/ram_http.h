#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
static bool ram_send(int fd,const void *data,size_t size)
{
    const unsigned char *p=data;
    while(size) {
        struct pollfd ready={fd,POLLOUT,0};if(poll(&ready,1,10000)<=0)return false;
        ssize_t n=send(fd,p,size,0);if(n<=0)return false;p+=n;size-=(size_t)n;
    }
    return true;
}
static void ram_http_request(int fd,const char *run,bool complete)
{
    if(!ram_log.active || !ram_log.frozen)return;
    char query[1024]={0};size_t used=0;
    while(used<sizeof query-1 && !strstr(query,"\r\n\r\n")) {
        struct pollfd ready={fd,POLLIN,0};if(poll(&ready,1,5000)<=0)return;
        ssize_t n=recv(fd,query+used,sizeof query-1-used,0);if(n<=0)return;
        used+=(size_t)n;query[used]=0;
    }
    char header[512];
    if(!strncmp(query,"GET /info HTTP/1.1\r\n",20)) {
        char body[8192];int pos=snprintf(body,sizeof body,"FILMRAM 1\nrun %s\ncomplete %u\nfailed %u\ncount %u\n",run,complete,ram_log.failed,ram_log.count);
        for(unsigned i=0;i<ram_log.count;++i) {
            RamLogFile *f=&ram_log.files[i];
            int n=snprintf(body+pos,sizeof body-(size_t)pos,"%u %s %lu %08lx\n",i,f->name,(unsigned long)f->size,(unsigned long)f->crc);
            if(n<0 || (size_t)n>=sizeof body-(size_t)pos)return;
            pos+=n;
        }
        int n=snprintf(header,sizeof header,"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",pos);
        if(ram_send(fd,header,(size_t)n))ram_send(fd,body,(size_t)pos);
        return;
    }
    unsigned id;unsigned long start,end;int consumed=0,line=0;
    char *range=strstr(query,"\r\nRange: bytes=");
    bool valid=sscanf(query,"GET /file/%u HTTP/1.1%n",&id,&line)==1 && line>0 && !strncmp(query+line,"\r\n",2) && id<ram_log.count && range &&
        sscanf(range,"\r\nRange: bytes=%lu-%lu%n",&start,&end,&consumed)==2 &&
        !strncmp(range+consumed,"\r\n",2) && start<=end && end<ram_log.files[id].size && end-start+1<=262144;
    if(!valid) {
        const char error[]="HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ram_send(fd,error,sizeof error-1);return;
    }
    RamLogFile *file=&ram_log.files[id];
    int n=snprintf(header,sizeof header,"HTTP/1.1 206 Partial Content\r\nContent-Length: %lu\r\nContent-Range: bytes %lu-%lu/%lu\r\nETag: \"%08lx\"\r\nConnection: close\r\n\r\n",end-start+1,start,end,(unsigned long)file->size,(unsigned long)file->crc);
    if(ram_send(fd,header,(size_t)n))ram_send(fd,file->data+start,end-start+1);
}
static void ram_http_serve(const char *run,bool complete)
{
#ifdef __3DS__
    void *soc_buffer=memalign(0x1000,0x100000);
    Result rc=soc_buffer?socInit(soc_buffer,0x100000):(Result)-1;
    if(R_FAILED(rc)) {
        printf("RAM NETWORK ERROR %08lx; data retained until exit.\n",(unsigned long)rc);
        free(soc_buffer);goto keep;
    }
    int listener=socket(AF_INET,SOCK_STREAM,0);int reuse=1;
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons(5000);address.sin_addr.s_addr=INADDR_ANY;
    if(listener>=0)setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof reuse);
    if(listener<0 || bind(listener,(struct sockaddr *)&address,sizeof address) || listen(listener,2)) {
        printf("RAM NETWORK ERROR errno=%d; data retained until exit.\n",errno);
    } else {
        printf("RAM HTTP port 5000; %u files.\nKEEP OPEN. START discards RAM and exits.\n",ram_log.count);
        while(aptMainLoop()) {
            hidScanInput();if(hidKeysDown()&KEY_START)break;
            struct pollfd ready={listener,POLLIN,0};int n=poll(&ready,1,100);
            if(n<0)break;
            if(!n)continue;
            int fd=accept(listener,NULL,NULL);if(fd<0)break;
            if(fcntl(fd,F_SETFL,O_NONBLOCK)==0)ram_http_request(fd,run,complete);
            close(fd);
        }
        if(listener>=0)close(listener);
        socExit();free(soc_buffer);return;
    }
    if(listener>=0)close(listener);
    socExit();free(soc_buffer);
keep:
    while(aptMainLoop()){hidScanInput();if(hidKeysDown()&KEY_START)break;gspWaitForVBlank();}
#else
    (void)run;(void)complete;
#endif
}
