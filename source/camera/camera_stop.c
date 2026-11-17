#include "camera_stop.h"
#include "film_runtime_contract.h"
FilmStatus film_camera_stop_transaction(const char *source,FilmCameraStopIo io,bool *dma_safe)
{
    if(!dma_safe)return FILM_INVALID;*dma_safe=false;
    if(!source||strcmp(source,"film-camera-stop/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!io.stop||!io.clear||!io.busy||!io.delay)return FILM_INVALID;
    FilmStatus stopped=io.stop(io.context),cleared=io.clear(io.context);
    for(unsigned i=0;i<20;++i){bool busy=true;FilmStatus s=io.busy(io.context,&busy);
        if(s!=FILM_OK)return s;
        if(!busy){*dma_safe=stopped==FILM_OK&&cleared==FILM_OK;return stopped!=FILM_OK?stopped:cleared;}
        if(i+1<20)io.delay(io.context,10);
    }
    return FILM_DEVICE;
}
