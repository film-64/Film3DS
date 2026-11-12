#pragma once
/* Filesystem copy only. No process handles, mappings, debug attach or driver writes. */
static void probe_collect_crash(FILE *meta,const char *directory)
{
    const char *names[]={"crash_dump_00000008.dmp","crash_dump_00000008.dump"};
    for(unsigned i=0;i<sizeof names/sizeof *names;++i) {
        char from[160],to[256];
        snprintf(from,sizeof from,"sdmc:/luma/dumps/arm11/%s",names[i]);
        FILE *input=fopen(from,"rb");
        if(!input) {fprintf(meta,"CRASH_FILE name=%s found=0\n",names[i]);continue;}
        snprintf(to,sizeof to,"%s/%s",directory,names[i]);
        FILE *output=fopen(to,"wb");
        if(!output){fclose(input);fprintf(meta,"CRASH_FILE name=%s found=1 copied=0\n",names[i]);continue;}
        char bytes[4096];size_t total=0;bool ok=true;
        while(total<8*1024*1024) {
            size_t n=fread(bytes,1,sizeof bytes,input);
            if(n && fwrite(bytes,1,n,output)!=n){ok=false;break;}
            total+=n;
            if(n<sizeof bytes){if(ferror(input))ok=false;break;}
        }
        if(total>=8*1024*1024)ok=false;
        fclose(input);if(fclose(output))ok=false;
        fprintf(meta,"CRASH_FILE name=%s found=1 copied=%u bytes=%lu\n",names[i],ok,(unsigned long)total);
    }
    fflush(meta);
}
