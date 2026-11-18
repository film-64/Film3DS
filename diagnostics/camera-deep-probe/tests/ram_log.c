#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <netinet/tcp.h>
#include "../probes/exposure/ram_log.h"
#include "../probes/exposure/ram_http.h"
int main(void)
{
    assert(ram_begin());FILE *f=probe_fopen("manifest.txt","w");assert(f);
    assert(fwrite("abc",1,3,f)==3 && !fclose(f));assert(ram_freeze());
    assert(ram_log.files[0].size==3 && ram_log.files[0].crc==0x352441c2);
    assert(!probe_fopen("frames.csv","w") && ram_log.failed);ram_release();
    assert(ram_begin());assert(!probe_fopen("bad\r\n.pgm","w") && ram_log.failed);ram_release();
    assert(ram_begin());assert(!probe_fopen("unknown.dat","w") && ram_log.failed);ram_release();
    assert(ram_begin());f=probe_fopen("frames.csv","w");assert(f);
    char *overflow=malloc(65537);assert(overflow);memset(overflow,'x',65537);
    fwrite(overflow,1,65537,f);fflush(f);assert(ferror(f) && ram_log.failed);
    fclose(f);assert(!ram_freeze() && ram_log.files[0].size<=65536);free(overflow);ram_release();
    assert(ram_begin());
    const char *logs[]={"ipc.csv","reads.csv","calls.csv","roi.csv","execution.csv","frames.csv","manifest.txt","calibration_plan.csv","noise.csv","frame_burst.csv","lifecycle.txt"};
    for(unsigned i=0;i<sizeof logs/sizeof *logs;++i)assert(probe_fopen(logs[i],"w"));
    for(unsigned i=0;i<16;++i){char name[64];snprintf(name,sizeof name,"s1-p%03u-test.pgm",i);assert(probe_fopen(name,"wb"));}
    assert(ram_log.used<=RAM_LOG_BYTES && !ram_log.failed);
    assert(!probe_fopen("s1-p099-extra.pgm","wb") && ram_log.failed);ram_release();
    assert(ram_begin());f=probe_fopen("manifest.txt","w");assert(f);fputs("RAM fixture\n",f);fclose(f);
    f=probe_fopen("s1-p003-test.pgm","wb");assert(f);
    for(unsigned i=0;i<70000;++i)assert(fputc((int)(i%251),f)!=EOF);
    assert(!fclose(f));f=probe_fopen("lifecycle.txt","w");assert(f);fclose(f);
    assert(ram_freeze());ram_http_serve("ram:/fixture",true);
    int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!bind(listener,(struct sockaddr *)&address,sizeof address) && !listen(listener,4));
    socklen_t size=sizeof address;assert(!getsockname(listener,(struct sockaddr *)&address,&size));
    printf("PORT %u\n",ntohs(address.sin_port));fflush(stdout);
    for(unsigned i=0;i<100;++i) {
        int fd=accept(listener,NULL,NULL);assert(fd>=0);
#ifdef TCP_CORK
        int cork=1;assert(!setsockopt(fd,IPPROTO_TCP,TCP_CORK,&cork,sizeof cork));
#endif
        ram_http_request(fd,"ram:/fixture",true);ram_http_finish(fd);
        struct linger abortive={1,0};assert(!setsockopt(fd,SOL_SOCKET,SO_LINGER,&abortive,sizeof abortive));
        close(fd);
    }
    close(listener);ram_release();return 0;
}
