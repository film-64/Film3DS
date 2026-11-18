#pragma once
/* v3-only bounded local trace. Old RAM-only sources never activate this writer. */
typedef struct {bool active,failed;unsigned count,record_limit;u64 run_ms,offset,run_offset;Handle handle;Result error;} BootJournal;
static BootJournal boot_journal;
typedef bool (*BootSink)(const char *,size_t,void *);
static bool boot_emit(BootJournal *j,unsigned stage,u32 address,u32 result,u64 now,u32 heap,u32 linear,BootSink sink,void *cookie)
{
    if(!j->active)return true;
    if(j->failed)return false;
    char line[BOOT_LINE_CAP];
    int n=snprintf(line,sizeof line,"BOOT run=%" PRIu64 " seq=%u stage=%u addr=%08" PRIx32 " result=%08" PRIx32 " at=%" PRIu64 " heap=%u linear=%u\n",
        (uint64_t)j->run_ms,j->count,stage,address,result,(uint64_t)now,(unsigned)heap,(unsigned)linear);
    unsigned limit=j->record_limit?j->record_limit:BOOT_RECORD_CAP;
    bool valid=(limit==768 || limit==1024) && ((stage>=1 && stage<=25) || stage==255);
    if(!valid || j->offset>BOOT_TOTAL_CAP || j->count>=limit || n<=0 || (size_t)n>=sizeof line ||
       (u64)n>BOOT_TOTAL_CAP-j->offset || !sink(line,(size_t)n,cookie)) {j->failed=true;if(j->error==0)j->error=(Result)-1;return false;}
    j->offset+=(u64)n;++j->count;return true;
}
#ifdef __3DS__
extern u32 __ctru_heap_size,__ctru_linear_heap_size;
static bool boot_sink(const char *bytes,size_t size,void *cookie)
{
    BootJournal *j=cookie;u32 wrote=0;
    Result rc=FSFILE_Write(j->handle,&wrote,j->offset,bytes,(u32)size,FS_WRITE_FLUSH);
    if(rc==0 && wrote==size)rc=FSFILE_Flush(j->handle);
    if(rc!=0 || wrote!=size){j->error=rc?rc:(Result)-1;return false;}
    return true;
}
static bool boot_begin(unsigned limit)
{
    BootJournal *j=&boot_journal;memset(j,0,sizeof *j);j->active=true;j->record_limit=limit;j->run_ms=osGetTime();
    if(mkdir("sdmc:/3ds/Film3DS",0777) && errno!=EEXIST){j->failed=true;j->error=(Result)-1;return false;}
    j->error=FSUSER_OpenFileDirectly(&j->handle,ARCHIVE_SDMC,fsMakePath(PATH_EMPTY,""),
        fsMakePath(PATH_ASCII,"/3ds/Film3DS/ccm-boot-v3.log"),FS_OPEN_READ|FS_OPEN_WRITE|FS_OPEN_CREATE,0);
    if(j->error==0)j->error=FSFILE_GetSize(j->handle,&j->offset);
    if(j->error!=0 || j->offset>BOOT_TOTAL_CAP){j->failed=true;if(j->error==0)j->error=(Result)-1;return false;}
    j->run_offset=j->offset;return true;
}
static bool boot_mark(unsigned stage,u32 address,Result result)
{if(!boot_journal.active)return true;return boot_emit(&boot_journal,stage,address,(u32)result,osGetTime(),__ctru_heap_size,__ctru_linear_heap_size,boot_sink,&boot_journal);}
static void boot_hold(const char *reason)
{
    printf("\nSTARTUP DIAGNOSTIC STOP\n%s\nresult=%08lx\nSD: /3ds/Film3DS/ccm-boot-v3.log\nSTART exits.\n",reason,(unsigned long)(u32)boot_journal.error);
    while(aptMainLoop()){hidScanInput();if(hidKeysDown()&KEY_START)break;gspWaitForVBlank();}
}
#else
static FILE *boot_host_file;
static u32 boot_host_heap,boot_host_linear;
static bool boot_sink(const char *bytes,size_t size,void *cookie)
{(void)cookie;return boot_host_file && fwrite(bytes,1,size,boot_host_file)==size && fflush(boot_host_file)==0;}
static bool boot_mark(unsigned stage,u32 address,Result result)
{if(!boot_journal.active)return true;return boot_emit(&boot_journal,stage,address,(u32)result,osGetTime(),boot_host_heap,boot_host_linear,boot_sink,&boot_journal);}
#endif
static unsigned boot_request_limit(int argc,char **argv)
{
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"--calibration-part=camera-gain-matrix-v4") || !strcmp(argv[i],"--calibration-part=camera-exposure-rebuild-v2") || !strcmp(argv[i],"--calibration-part=camera-exposure-dense-v1"))return 1024;
        if(!strcmp(argv[i],"--calibration-part=camera-ccm-normalization-v3"))return 768;
    }
    return 0;
}
static bool boot_requested(int argc,char **argv) {return boot_request_limit(argc,argv)!=0;}
static bool boot_copy(void)
{
    if(!boot_journal.active)return true;
    if(boot_journal.failed || boot_journal.offset<boot_journal.run_offset || boot_journal.offset-boot_journal.run_offset>128*1024)return false;
    FILE *out=probe_fopen("startup_journal.txt","w");if(!out)return false;
    char data[1024];bool ok=true;
    for(u64 pos=boot_journal.run_offset;pos<boot_journal.offset && ok;) {
        u32 count=(u32)(boot_journal.offset-pos);if(count>sizeof data)count=sizeof data;
#ifdef __3DS__
        u32 got=0;Result rc=FSFILE_Read(boot_journal.handle,&got,pos,data,count);ok=rc==0 && got==count;
#else
        ok=boot_host_file && fseek(boot_host_file,(long)pos,SEEK_SET)==0 && fread(data,1,count,boot_host_file)==count;
#endif
        if(ok)ok=fwrite(data,1,count,out)==count;
        pos+=count;
    }
    if(fclose(out))ok=false;
#ifndef __3DS__
    if(boot_host_file)fseek(boot_host_file,0,SEEK_END);
#endif
    return ok;
}
