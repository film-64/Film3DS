#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "../probes/exposure/ram_log.h"
#include "../probes/exposure/ram_http.h"
int main(void)
{
    assert(ram_begin_sweep());assert(!probe_fopen("ipc.csv","w"));ram_release();
    assert(ram_begin_sweep());
    for(unsigned i=0;i<RAM_SWEEP_FILES;++i) {
        char name[80];snprintf(name,sizeof name,"s1-p%03u-count.yuyv",i);
        FILE *f=probe_fopen(name,"wb");assert(f);assert(!fclose(f));
    }
    assert(!probe_fopen("manifest.txt","w") && ram_log.failed);ram_release();
    assert(ram_begin_sweep());
    for(unsigned i=0;i<278;++i) {
        char name[80];snprintf(name,sizeof name,"s1-p%03u-sweep-fixture.yuyv",i);
        FILE *f=probe_fopen(name,"wb");assert(f);
        for(unsigned j=0;j<128*96*2;++j)assert(fputc((i+j)%251,f)!=EOF);
        assert(!fclose(f));
    }
    assert(ram_freeze() && ram_log.used< RAM_AUTO_BYTES);
    int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!bind(listener,(struct sockaddr *)&address,sizeof address) && !listen(listener,2));
    socklen_t n=sizeof address;assert(!getsockname(listener,(struct sockaddr *)&address,&n));
    printf("PORT %u\n",ntohs(address.sin_port));fflush(stdout);
    for(unsigned i=0;i<10;++i) {
        int fd=accept(listener,NULL,NULL);assert(fd>=0);
        ram_http_request(fd,"ram:/sweep-fixture",true);ram_http_finish(fd);close(fd);
    }
    close(listener);ram_release();return 0;
}
