#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
/* The one diagnostic entry has an explicit scope; replayed setup is not a sample. */
enum { PROBE_FIRST_PHASE=3, PROBE_SWEEP_LAST_PHASE=58, PROBE_DIRECT_LAST_PHASE=72 };
typedef struct { unsigned sensor,first_phase;bool auto_start,direct,raw,calibrate; } ProbeRequest;
static bool probe_number(const char *text,unsigned lo,unsigned hi,unsigned *out)
{
    char *end;unsigned long n=strtoul(text,&end,10);
    if(!*text || *end || n<lo || n>hi)return false;
    *out=(unsigned)n;return true;
}
static bool probe_parse_request(int argc,char **argv,ProbeRequest *request)
{
    *request=(ProbeRequest){.first_phase=PROBE_FIRST_PHASE};
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"--auto"))request->auto_start=true;
        else if(!strcmp(argv[i],"--direct"))request->direct=true;
        else if(!strcmp(argv[i],"--raw"))request->raw=true;
        else if(!strcmp(argv[i],"--calibrate"))request->calibrate=true;
        else if(!strncmp(argv[i],"--sensor=",9)) {
            if(!probe_number(argv[i]+9,1,3,&request->sensor))return false;
        } else if(!strncmp(argv[i],"--from-phase=",13)) {
            if(!probe_number(argv[i]+13,PROBE_FIRST_PHASE,PROBE_SWEEP_LAST_PHASE,&request->first_phase))return false;
        } else return false;
    }
    if(request->calibrate && (!request->sensor || request->first_phase!=PROBE_FIRST_PHASE || request->direct))return false;
    return request->first_phase==PROBE_FIRST_PHASE || request->sensor!=0;
}
