#pragma once
/* ram-evidence-1: one bounded 20 MiB arena, write-only evidence streams.
 * No SD fallback, overwrite, retry, eviction or compression. A failed or full
 * stream stops acquisition. Freeze after closing the camera and all streams;
 * only then expose read-only HTTP ranges. RAM survives client disconnection,
 * not application exit or loss of power. Full58/raw/resume are not admitted
 * by v1. Registered exposure-auto/v1 uses the separate 28MiB raw-capable v2.
 */
#include <sys/types.h>
enum { RAM_LOG_BYTES=20*1024*1024, RAM_AUTO_BYTES=28*1024*1024, RAM_LOG_FILES=40 };
typedef struct {char name[96];unsigned char *data;size_t size,capacity;bool closed;FILE *stream;uint32_t crc;} RamLogFile;
static struct {unsigned char *arena;size_t used,limit;unsigned count;bool active,failed,frozen,exposure_auto;RamLogFile files[RAM_LOG_FILES];} ram_log;
static size_t ram_capacity(const char *name)
{
    if(!strcmp(name,"ipc.csv"))return 12*1024*1024;
    if(!strcmp(name,"reads.csv"))return 2*1024*1024;
    if(!strcmp(name,"calls.csv") || !strcmp(name,"roi.csv"))return 512*1024;
    if(!strcmp(name,"execution.csv"))return 128*1024;
    if(!strcmp(name,"frames.csv"))return 64*1024;
    if(!strcmp(name,"manifest.txt"))return 32*1024;
    if(!strcmp(name,"calibration_plan.csv") || !strcmp(name,"noise.csv") || !strcmp(name,"frame_burst.csv") || !strcmp(name,"lifecycle.txt"))return 16*1024;
    if(!strncmp(name,"s",1) && strlen(name)>4 && !strcmp(name+strlen(name)-4,".pgm"))return 640*480+64;
    if(ram_log.exposure_auto && !strncmp(name,"s",1) && strlen(name)>5 && !strcmp(name+strlen(name)-5,".yuyv"))return 640*480*2;
    return 0;
}
static bool ram_begin_profile(bool exposure_auto)
{
    if(ram_log.active)return false;
    ram_log.limit=exposure_auto?RAM_AUTO_BYTES:RAM_LOG_BYTES;
    ram_log.exposure_auto=exposure_auto;
    ram_log.arena=malloc(ram_log.limit);
    if(!ram_log.arena)return false;
    ram_log.active=true;return true;
}
static inline bool ram_begin(void) {return ram_begin_profile(false);}
static ssize_t ram_write(void *cookie,const char *data,size_t count)
{
    RamLogFile *file=cookie;
    if(ram_log.frozen || file->closed || count>file->capacity-file->size) {
        ram_log.failed=true;errno=ENOSPC;return -1;
    }
    memcpy(file->data+file->size,data,count);file->size+=count;return (ssize_t)count;
}
static int ram_close(void *cookie)
{
    RamLogFile *file=cookie;file->closed=true;file->stream=NULL;return 0;
}
static FILE *probe_fopen(const char *path,const char *mode)
{
    if(!ram_log.active)return fopen(path,mode);
    const char *name=strrchr(path,'/');name=name?name+1:path;
    for(const char *c=name;*c;++c)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||strchr("_.-",*c)))goto fail;
    size_t capacity=ram_capacity(name);
    if(ram_log.frozen || ram_log.count>=RAM_LOG_FILES || !capacity || strlen(name)>=sizeof ram_log.files[0].name ||
       (strcmp(mode,"w") && strcmp(mode,"wb")) || capacity>ram_log.limit-ram_log.used)goto fail;
    for(unsigned i=0;i<ram_log.count;++i)if(!strcmp(ram_log.files[i].name,name))goto fail;
    RamLogFile *file=&ram_log.files[ram_log.count];
    strcpy(file->name,name);file->data=ram_log.arena+ram_log.used;file->capacity=capacity;
    file->stream=fopencookie(file,"w",(cookie_io_functions_t){.write=ram_write,.close=ram_close});
    if(!file->stream)goto fail;
    ram_log.used+=capacity;++ram_log.count;return file->stream;
fail:
    ram_log.failed=true;errno=ENOSPC;return NULL;
}
static bool ram_freeze(void)
{
    bool ok=!ram_log.failed;
    for(unsigned i=0;i<ram_log.count;++i) {
        RamLogFile *file=&ram_log.files[i];
        if(file->stream && fclose(file->stream))ok=false;
        uint32_t crc=UINT32_MAX;
        for(size_t j=0;j<file->size;++j){crc^=file->data[j];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}
        file->crc=~crc;
    }
    ram_log.frozen=true;ram_log.failed=ram_log.failed || !ok;return ok;
}
static void ram_release(void)
{
    if(!ram_log.active)return;
    for(unsigned i=0;i<ram_log.count;++i)if(ram_log.files[i].stream)fclose(ram_log.files[i].stream);
    free(ram_log.arena);memset(&ram_log,0,sizeof ram_log);
}
