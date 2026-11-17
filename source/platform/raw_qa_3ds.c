#include "platform_camera.h"
#include "platform_qa.h"
#include "raw_capture.h"
#include "film_runtime_contract.h"
#include <3ds.h>
#include <string.h>
#include <sys/stat.h>

/* One explicit launch, one production capture. No alternative sensor driver,
 * fallback frame, retry, ROLL mutation, or automatic exposure substitution. */
static void milestone(FILE *out,const char *stage)
{fprintf(out,"LIVE stage=%s\n",stage);fflush(out);}

static bool recover_trace(FILE *out,const char *token)
{
    if(!film_runtime_source_valid("film-raw-capture-trace-recover/v1")||!film_raw_qa_token_valid(token))return false;
    char path[160],line[512];struct stat info;
    snprintf(path,sizeof path,"sdmc:/3ds/Film3DS/rawqa-%s/trace.txt",token);
    fprintf(out,"RECOVER token=%s\n",token);fflush(out);
    if(stat(path,&info)||info.st_size>262144){fputs("RECOVER missing_or_oversized\n",out);fflush(out);return false;}
    FILE *file=fopen(path,"r");if(!file){fputs("RECOVER open_failed\n",out);fflush(out);return false;}
    unsigned lines=0;while(lines++<2048&&fgets(line,sizeof line,file)){
        /* Prefix old results so they cannot masquerade as the current run. */
        fprintf(out,"RECOVER %s",line);if(!strchr(line,'\n'))fputc('\n',out);fflush(out);
    }
    bool complete=feof(file)&&!ferror(file);fclose(file);fputs("RECOVER end\n",out);fflush(out);return complete;
}

bool film_raw_capture_qa(const char *token,const char *recover,bool recover_only)
{
    if(!film_raw_qa_token_valid(token))return false;
    char directory[128],path[160];
    snprintf(directory,sizeof directory,"sdmc:/3ds/Film3DS/rawqa-%s",token);
    if(mkdir(directory,0777)!=0)return false;
    snprintf(path,sizeof path,"%s/trace.txt",directory);
    FILE *raw_trace=fopen(path,"w+");if(!raw_trace)return false;
    FilmHardwareQA qa={.socket_fd=-1};
    if(!film_hardware_qa_open(&qa)){fputs("FAIL return_channel\n",raw_trace);fclose(raw_trace);return false;}
    fprintf(qa.output,"F3RAWQA1 %s\n",token);fflush(qa.output);
    bool recovered=recover?recover_trace(qa.output,recover):false;
    if(recover_only){
        fprintf(qa.output,"RESULT status=%s raw_bytes=0\nDONE\n",recovered?"recovered":"recovery_failed");
        bool sent=film_hardware_qa_confirm(&qa);film_hardware_qa_close(&qa);fclose(raw_trace);return sent;
    }
    fprintf(raw_trace,"source=film-raw-capture-automation/v1 case=native_AE\n");
    FilmCamera camera={.runtime_log=raw_trace};FilmFrame raw={.recipe=film_default_recipe()};
    milestone(qa.output,"before_open");
    FilmStatus s=film_camera_open(&camera,FILM_YUYV,FILM_CAMERA_REAR1);
    fprintf(raw_trace,"QA stage=open status=%s result=%08lx\n",film_status_name(s),(unsigned long)camera.last_result);fflush(raw_trace);
    milestone(qa.output,"after_open");
    if(s==FILM_OK)s=film_camera_request_15fps(&camera);
    fprintf(raw_trace,"QA stage=15fps status=%s result=%08lx\n",film_status_name(s),(unsigned long)camera.last_result);fflush(raw_trace);
    milestone(qa.output,"after_15fps");
    if(s==FILM_OK)s=film_camera_select_exposure_mode(&camera,FILM_EXPOSURE_NATIVE_AE,0,raw_trace);
    fprintf(raw_trace,"QA stage=mode status=%s result=%08lx\n",film_status_name(s),(unsigned long)camera.last_result);fflush(raw_trace);
    milestone(qa.output,"after_mode");
    if(s==FILM_OK)s=film_camera_preview_start(&camera);
    unsigned frames=0;u64 deadline=osGetTime()+5000;
    while(s==FILM_OK&&frames<3&&osGetTime()<deadline){
        FilmImage image={0};s=film_camera_preview_poll(&camera,&image);
        if(image.pixels)++frames;
        if(s==FILM_OK&&frames<3)svcSleepThread(1000000LL);
    }
    if(s==FILM_OK&&frames!=3)s=FILM_DEVICE;
    fprintf(raw_trace,"QA stage=preview status=%s frames=%u result=%08lx\n",film_status_name(s),frames,(unsigned long)camera.last_result);fflush(raw_trace);
    milestone(qa.output,"after_preview");
    if(s==FILM_OK)s=film_camera_capture_raw(&camera,&raw,raw_trace);
    fprintf(raw_trace,"QA stage=capture status=%s camera_live=%u bytes=%u\n",film_status_name(s),camera.initialized,raw.source.pixels?614400u:0u);
    milestone(qa.output,"after_capture");film_camera_close(&camera);
    milestone(qa.output,"after_close");
    if(s==FILM_OK){
        snprintf(path,sizeof path,"%s/capture.dng",directory);
        s=film_dng_write("film-dng-write/v1",path,&raw);
        FilmFrame check={0};if(s==FILM_OK)s=film_frame_read(path,&check);
        if(s==FILM_OK&&(check.source.format!=FILM_RAW10_RGGB||check.source.width!=640||check.source.height!=480||memcmp(raw.source.pixels,check.source.pixels,614400)))s=FILM_CORRUPT;
        film_frame_free(&check);
        fprintf(raw_trace,"QA stage=source_roundtrip status=%s\n",film_status_name(s));
    }
    fprintf(raw_trace,"RESULT status=%s raw_bytes=%u\n",film_status_name(s),s==FILM_OK?614400u:0u);fflush(raw_trace);rewind(raw_trace);
    char line[512];while(fgets(line,sizeof line,raw_trace))fputs(line,qa.output);
    fclose(raw_trace);
    if(s==FILM_OK){
        fputs("RAW_BYTES 614400\n",qa.output);fflush(qa.output);
        for(size_t offset=0;offset<614400;offset+=16384){
            size_t bytes=614400-offset;if(bytes>16384)bytes=16384;
            if(fwrite(raw.source.pixels+offset,1,bytes,qa.output)!=bytes||fflush(qa.output)!=0)break;
        }
        fputc('\n',qa.output);
    }
    film_frame_free(&raw);fputs("DONE\n",qa.output);
    bool sent=film_hardware_qa_confirm(&qa);film_hardware_qa_close(&qa);return sent;
}
