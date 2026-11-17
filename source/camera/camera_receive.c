#include "camera_receive.h"
#include "film_runtime_contract.h"
FilmStatus film_camera_receive_complete(const char *source,FilmCameraReceiveIo io)
{
    if(!source||strcmp(source,"film-camera-receive-complete/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!io.wait||!io.finished||!io.now_ms||!io.delay)return FILM_INVALID;
    uint64_t start=io.now_ms(io.context);
    FilmStatus s=io.wait(io.context,1500);if(s!=FILM_OK)return s;
    for(unsigned i=0;i<=1500;++i){bool finished=false;s=io.finished(io.context,&finished);
        if(s!=FILM_OK)return s;
        if(finished)return FILM_OK;
        if(io.now_ms(io.context)-start>=1500||i==1500)return FILM_DEVICE;
        io.delay(io.context,1);
    }
    return FILM_DEVICE;
}
