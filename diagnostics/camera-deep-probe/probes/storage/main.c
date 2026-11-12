/* storage-check-1: bounded fresh-file I/O only. No camera, network, repair or deletion. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __3DS__
#include <3ds.h>
#endif
enum { FILES=10, BLOCKS=64, BLOCK_BYTES=4096 };
static uint8_t block[BLOCK_BYTES];
static char directory[768];
static FILE *report;
static unsigned write_number;
static uint32_t crc(uint32_t value,const uint8_t *data,size_t length)
{
    for(size_t i=0;i<length;++i) {
        value^=data[i];for(unsigned bit=0;bit<8;++bit)value=(value>>1)^(0xedb88320u&(0u-(value&1)));
    }
    return value;
}
static void pattern(unsigned file,unsigned round)
{
    uint32_t state=0x91ac752bu^(file*0x9e3779b9u)^(round*0x85ebca6bu);
    for(unsigned i=0;i<BLOCK_BYTES;++i){state^=state<<13;state^=state>>17;state^=state<<5;block[i]=(uint8_t)state;}
}
static void event(const char *operation,unsigned file,unsigned round,bool ok,int error,uint32_t checksum)
{
    printf("%s file=%u block=%u ok=%u errno=%d raw=%08lx\n",operation,file,round,ok,error,(unsigned long)(uint32_t)error);
    if(report) {
        fprintf(report,"%s,%u,%u,%u,%d,%08lx,%08lx\n",operation,file,round,ok,error,(unsigned long)(uint32_t)error,(unsigned long)checksum);
        fflush(report);
    }
    fflush(stdout);
}
static bool ensure_directory(const char *path)
{
    struct stat info;
    if(stat(path,&info)==0){if(S_ISDIR(info.st_mode))return true;errno=ENOTDIR;return false;}
    if(errno!=ENOENT)return false;
    return mkdir(path,0777)==0;
}
static bool check(void)
{
    FILE *files[FILES]={0};uint32_t expected[FILES];bool ok=true;char path[840];
    for(unsigned f=0;f<FILES;++f) {
        expected[f]=UINT32_MAX;snprintf(path,sizeof path,"%s/test-%02u.bin",directory,f);
        files[f]=fopen(path,"wb");int error=files[f]?0:errno;
        event("open_write",f,0,files[f]!=NULL,error,0);
        if(!files[f]){ok=false;break;}
    }
    for(unsigned r=0;r<BLOCKS && ok;++r)for(unsigned f=0;f<FILES && ok;++f) {
        pattern(f,r);++write_number;errno=0;
        size_t written;
#if !defined(__3DS__) && defined(STORAGE_INJECT_WRITE_FAILURE)
        if(write_number==STORAGE_INJECT_WRITE_FAILURE){written=0;errno=EIO;}
        else
#endif
        written=fwrite(block,1,sizeof block,files[f]);
        int error=written==sizeof block?0:errno;
        if(written!=sizeof block){event("write",f,r,false,error,0);ok=false;break;}
        expected[f]=crc(expected[f],block,sizeof block);
        int flushed=fflush(files[f]);error=flushed?errno:0;
        if(flushed || ferror(files[f])){event("flush",f,r,false,error,0);ok=false;break;}
        if((r+1)%8==0) {
            int synced=fsync(fileno(files[f]));error=synced?errno:0;
            if(synced){event("sync",f,r,false,error,0);ok=false;break;}
        }
        if((r+1)%8==0)event("write_sync",f,r,true,0,~expected[f]);
    }
    for(unsigned f=0;f<FILES;++f)if(files[f]) {
        int closed=fclose(files[f]),error=closed?errno:0;files[f]=NULL;
        if(closed){event("close",f,0,false,error,0);ok=false;}
    }
    if(!ok)return false; /* No retries and no reads pretending a failed write succeeded. */
    for(unsigned f=0;f<FILES && ok;++f) {
        snprintf(path,sizeof path,"%s/test-%02u.bin",directory,f);FILE *input=fopen(path,"rb");
        if(!input){event("open_read",f,0,false,errno,0);return false;}
        uint32_t actual=UINT32_MAX;
        for(unsigned r=0;r<BLOCKS;++r) {
            errno=0;size_t got=fread(block,1,sizeof block,input);
            if(got!=sizeof block){event("read",f,r,false,errno,0);ok=false;break;}
#if !defined(__3DS__) && defined(STORAGE_INJECT_CORRUPTION)
            if(f==0 && r==0)block[0]^=1;
#endif
            actual=crc(actual,block,sizeof block);
        }
        if(ok && (fgetc(input)!=EOF || ferror(input))){event("length",f,0,false,errno,0);ok=false;}
        if(fclose(input)){event("close_read",f,0,false,errno,0);ok=false;}
        if(actual!=expected[f]){event("crc_mismatch",f,0,false,EILSEQ,~actual);ok=false;}
        else if(ok)event("readback_crc",f,0,true,0,~actual);
    }
    return ok;
}
int main(int argc,char **argv)
{
    const char *root="sdmc:/3ds/Film3DS";
#ifdef __3DS__
    gfxInitDefault();consoleInit(GFX_TOP,NULL);
#else
    if(argc==2)root=argv[1];
#endif
    (void)argc;(void)argv;
    char parent[640];snprintf(parent,sizeof parent,"%s/storage-checks",root);
    bool prepared=ensure_directory(parent);int prepare_error=prepared?0:errno;
    if(prepared) {
        prepared=false;
        for(unsigned suffix=0;suffix<1000;++suffix) {
            snprintf(directory,sizeof directory,"%s/run-%lu-%03u",parent,(unsigned long)time(NULL),suffix);
            struct stat info;if(!stat(directory,&info))continue;
            if(errno!=ENOENT)break;
            if(!mkdir(directory,0777)){prepared=true;break;}
            break;
        }
        if(!prepared)prepare_error=errno;
    }
    bool passed=false;
    if(prepared) {
        char path[840];snprintf(path,sizeof path,"%s/result.csv",directory);report=fopen(path,"w");
        if(report) {
            fputs("operation,file,block,ok,errno,result_hex,crc32\n",report);
            printf("storage-check-1: 10 new files x 64 x 4096 bytes\n2.5 MiB writes, flush/sync, reopen and CRC32.\nNo camera or network; originals untouched.\n%s\n",directory);
            struct statvfs space;
            if(!statvfs(root,&space))printf("Available bytes: %llu\n",(unsigned long long)space.f_bavail*space.f_frsize);
            passed=check();event(passed?"DATA_CHECK_PASS":"DATA_CHECK_FAIL",0,0,passed,passed?0:errno,0);
            if(fclose(report))passed=false;
            report=NULL;
        } else event("report_open",0,0,false,errno,0);
    } else event("directory",0,0,false,prepare_error,0);
    printf("\nSTORAGE CHECK %s\n%s\nSTART exits. No sampling has run.\n",passed?"PASS":"FAIL",directory);
#ifdef __3DS__
    while(aptMainLoop()){hidScanInput();if(hidKeysDown()&KEY_START)break;gspWaitForVBlank();}
    gfxExit();
#endif
    return passed?0:1;
}
