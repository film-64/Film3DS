#define _GNU_SOURCE
#include "platform_qa.h"
#include "film_runtime_contract.h"
#include <3ds.h>
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
/* These are byte-identical copies of the existing, verified probe transport.
 * Only the application-to-frozen-file adapter below is new. */
#include "diagnostics/existing_ram/ram_log.h"
#include "diagnostics/existing_ram/ram_http.h"

bool film_app_evidence_serve(const char *run,const char *const paths[4])
{
    if(!film_runtime_source_valid("film-app-frozen-evidence/v1")||!run||!paths||ram_log.active)return false;
    ram_log.limit=4*1024*1024;ram_log.arena=malloc(ram_log.limit);
    if(!ram_log.arena)return false;
    ram_log.active=true;
    for(unsigned i=0;i<4;++i){
        struct stat info;
        if(!paths[i]||stat(paths[i],&info)||!S_ISREG(info.st_mode)||info.st_size<0||
            (size_t)info.st_size>ram_log.limit-ram_log.used){ram_log.failed=true;continue;}
        const char *name=strrchr(paths[i],'/');name=name?name+1:paths[i];
        if(strlen(name)>=sizeof ram_log.files[0].name){ram_log.failed=true;continue;}
        FILE *file=fopen(paths[i],"rb");if(!file){ram_log.failed=true;continue;}
        size_t size=(size_t)info.st_size;unsigned char *data=ram_log.arena+ram_log.used;
        bool ok=fread(data,1,size,file)==size&&!ferror(file);if(fclose(file))ok=false;
        if(!ok){ram_log.failed=true;continue;}
        RamLogFile *entry=&ram_log.files[ram_log.count++];
        strcpy(entry->name,name);entry->data=data;entry->size=entry->capacity=size;entry->closed=true;
        ram_log.used+=size;
    }
    if(!ram_log.count){ram_release();return false;}
    bool complete=ram_freeze();ram_http_serve(run,complete);ram_release();return true;
}
