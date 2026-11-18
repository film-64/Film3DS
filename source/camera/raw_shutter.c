#include "platform_camera.h"
void film_camera_raw_invalidate(FilmCamera *camera)
{
    if(!camera)return;
    if(!++camera->raw_epoch)++camera->raw_epoch;
    camera->raw_preparation=(FilmRawPreparation){.epoch=camera->raw_epoch,.status=FILM_STATE};
}
FilmStatus film_camera_raw_shutter_check(const char *source,const FilmCamera *camera)
{
    if(!source||strcmp(source,"film-camera-shutter-raw/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!camera)return FILM_INVALID;
    if(!camera->initialized)return FILM_STATE;
    if(camera->sensor!=FILM_CAMERA_REAR1)return FILM_UNSUPPORTED;
    if(!camera->raw_preparation.ready||!camera->raw_preparation.attempted||camera->raw_preparation.status!=FILM_OK||
       !camera->raw_epoch||camera->raw_preparation.epoch!=camera->raw_epoch)return FILM_STATE;
    if(camera->exposure_mode==FILM_EXPOSURE_NATIVE_AE)return camera->exposure_requested?FILM_STATE:FILM_OK;
    if(camera->exposure_mode!=FILM_EXPOSURE_MANUAL||!camera->exposure_requested||!camera->exposure.active||!camera->exposure.saved_valid||!camera->exposure.ready||camera->exposure.failed)return FILM_STATE;
    return FILM_OK;
}
