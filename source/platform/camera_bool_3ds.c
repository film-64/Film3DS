#include "platform_camera.h"
#include <3ds.h>
#include <string.h>
extern Handle camHandle;
uint32_t film_camera_read_bool(unsigned command,unsigned argument,bool *value,FILE *log)
{
    if(!value||(command!=3&&command!=8&&command!=0x1a&&command!=0x1c))
        return MAKERESULT(RL_PERMANENT,RS_INVALIDARG,RM_CAM,RD_INVALID_RESULT_VALUE);
    /* Filesystem logging uses this same TLS IPC buffer. Log before building
     * the request, and snapshot the reply before any subsequent logging. */
    if(log){fprintf(log,"CAM bool cmd=%02x argument=%u begin\n",command,argument);fflush(log);}
    u32 *b=getThreadCommandBuffer();memset(b,0,64*sizeof *b);
    b[0]=IPC_MakeHeader(command,1,0);b[1]=argument;b[2]=0xa5a50000;
    Result transport=svcSendSyncRequest(camHandle);uint32_t reply[3];memcpy(reply,b,sizeof reply);
    FilmStatus s=R_FAILED(transport)?FILM_DEVICE:film_camera_bool_reply(command,reply,value);
    if(log){fprintf(log,"CAM bool cmd=%02x header=%08lx word=%08lx transport=%08lx service=%08lx status=%s value=%u\n",
        command,(unsigned long)reply[0],(unsigned long)reply[2],(unsigned long)transport,(unsigned long)reply[1],film_status_name(s),s==FILM_OK?*value:0);fflush(log);}
    if(R_FAILED(transport))return transport;
    if(R_FAILED((Result)reply[1]))return reply[1];
    return s==FILM_OK?0:MAKERESULT(RL_PERMANENT,RS_INVALIDRESVAL,RM_CAM,RD_INVALID_RESULT_VALUE);
}
