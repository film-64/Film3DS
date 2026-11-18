#include "raw_capture.h"
#include "film_runtime_contract.h"
#include <string.h>
bool film_raw_qa_token_valid(const char *token)
{return film_runtime_source_valid("film-raw-capture-automation/v1")&&token&&strlen(token)==32&&strspn(token,"0123456789abcdef")==32;}
static const uint16_t addresses[]={0x3006,0x3008,0x321c,0x3330,0x3400,0x3012,0x3056,0x3058,0x305a,0x305c};
static const uint16_t masks[]={65535,65535,0x8f,0x40,0xfc00,65535,65535,65535,65535,65535};
static void trace(FilmRawCaptureIo *io,const char *phase)
{if(io->trace)io->trace(io->context,phase,0,0,0,FILM_OK);}
static FilmStatus check(FilmRawCaptureIo *io,bool mcu,uint16_t a,uint16_t expected,uint16_t mask)
{uint16_t v=0;FilmStatus s=io->read(io->context,mcu,a,&v);if(s==FILM_OK&&(v&mask)!=(expected&mask))s=FILM_STATE;
 if(s!=FILM_OK&&io->trace)io->trace(io->context,mcu?"verify_mcu":"verify_reg",a,expected&mask,v&mask,s);
 return s;}
static FilmStatus controls(FilmRawCaptureIo *io,const uint16_t *values)
{
    for(unsigned i=0;i<5;++i){FilmStatus s=check(io,false,addresses[i],values[i],masks[i]);if(s!=FILM_OK)return s;}
    return FILM_OK;
}
static FilmStatus exposure(FilmRawCaptureIo *io,const uint16_t *values,uint16_t seq,uint16_t fine)
{
    for(unsigned i=5;i<10;++i){FilmStatus s=check(io,false,addresses[i],values[i],masks[i]);if(s!=FILM_OK)return s;}
    FilmStatus s=check(io,true,0xa102,seq,65535);
    if(s==FILM_OK)s=check(io,false,0x3014,fine,65535);
    return s;
}
static FilmStatus hold(FilmRawCaptureIo *io,const uint16_t *values,uint16_t seq,uint16_t fine)
{
    FilmStatus s=controls(io,values);
    return s==FILM_OK?exposure(io,values,seq,fine):s;
}
FilmStatus film_raw_capture_prepare(const char *source,FilmRawCaptureIo io,uint32_t epoch,FilmRawPreparation *prepared)
{
    if(!prepared)return FILM_INVALID;
    *prepared=(FilmRawPreparation){.epoch=epoch,.attempted=true,.status=FILM_STATE};
    if(!source||strcmp(source,"film-camera-raw-prepare/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!io.read||!epoch)return prepared->status=FILM_INVALID;
    trace(&io,"preconditions");
    const uint16_t a[]={0,0x3002,0x3004,0x3006,0x3008,0x3040,0x300a};
    const uint16_t v[]={8832,0,0,487,647,0x25,704};FilmStatus s;
    for(unsigned i=0;i<7;++i)if((s=check(&io,false,a[i],v[i],65535))!=FILM_OK)return prepared->status=s;
    prepared->native_controls[0]=487;prepared->native_controls[1]=647;
    for(unsigned i=2;i<5;++i)if((s=io.read(io.context,false,addresses[i],prepared->native_controls+i))!=FILM_OK)return prepared->status=s;
    if(prepared->native_controls[3]&0x40)return FILM_STATE;
    prepared->ready=true;return prepared->status=FILM_OK;
}
FilmStatus film_raw_capture_transaction_result(const char *source,FilmRawCaptureIo io,const FilmRawPreparation *prepared,uint32_t epoch,FilmRawCaptureResult *result)
{
    if(!result)return FILM_INVALID;
    *result=(FilmRawCaptureResult){.capture_status=FILM_STATE,.restore_status=FILM_STATE};
    if(!source||strcmp(source,"film-camera-raw-capture/v1")||!film_runtime_source_valid(source))return FILM_STATE;
    if(!prepared||!prepared->ready||!prepared->attempted||prepared->status!=FILM_OK||!epoch||prepared->epoch!=epoch||
       prepared->native_controls[0]!=487||prepared->native_controls[1]!=647||(prepared->native_controls[3]&0x40))return FILM_STATE;
    if(!io.read||!io.write||!io.lock||!io.frame||!io.idle||!io.restore_auto)return result->capture_status=FILM_INVALID;
    uint16_t saved[10]={0},target[10],seq=0,fine=0;FilmStatus s;
    memcpy(saved,prepared->native_controls,sizeof prepared->native_controls);
    if((s=io.read(io.context,true,0xa102,&seq))!=FILM_OK)return result->capture_status=s;
    trace(&io,"lock_auto");s=io.lock(io.context);
    if(s==FILM_OK)s=io.read(io.context,false,0x3014,&fine);
    /* Freeze the actual shutter-time exposure after AE is locked. Both native
     * AE and the existing manual owner supply these values; do not replace
     * them with a diagnostic's fixed coarse/gain settings. */
    for(unsigned i=5;s==FILM_OK&&i<10;++i){
        s=io.read(io.context,false,addresses[i],saved+i);
        if(s==FILM_OK&&prepared->prepared_manual&&saved[i]!=prepared->prepared_exposure[i-5]){
            s=FILM_STATE;
            if(io.trace)io.trace(io.context,"verify_reg",addresses[i],prepared->prepared_exposure[i-5],saved[i],s);
        }
    }
    if(s!=FILM_OK){
        /* No RAW/gate register has been changed. Never restore an exposure
         * snapshot that failed partway through reading. */
        result->capture_status=s;trace(&io,"restore");
        FilmStatus restored=io.idle(io.context);
        if(restored==FILM_OK)restored=io.restore_auto(io.context);
        result->restore_status=restored;result->camera_restored=restored==FILM_OK;
        return restored!=FILM_OK?restored:s;
    }
    memcpy(target,saved,sizeof target);
    target[0]=479;target[1]=639;target[2]=(saved[2]&~0x8fu)|131;
    target[3]=saved[3]|0x40;target[4]=(saved[4]&0x3ff)|(30u<<10);
    trace(&io,"native_gate");if(s==FILM_OK)s=io.write(io.context,true,0xa102,seq&~13u);
    uint16_t gate[10];memcpy(gate,saved,sizeof gate);memcpy(gate+5,target+5,5*sizeof *gate);
    if(s==FILM_OK)s=check(&io,true,0xa102,seq&~13u,65535);
    if(s==FILM_OK)s=io.frame(io.context,false,false);
    if(s==FILM_OK)s=exposure(&io,gate,seq&~13u,fine);
    trace(&io,"raw_switch");for(unsigned i=0;s==FILM_OK&&i<5;++i)s=io.write(io.context,false,addresses[i],target[i]);
    /* Retain the first RAW, never a later scene after fixed discarded frames.
     * The frame callback and post-hold still have to prove it valid. A failed
     * first receive is an error, not permission to capture a replacement. */
    trace(&io,"raw_retain");
    if(s==FILM_OK)s=controls(&io,target);
    if(s==FILM_OK)s=io.frame(io.context,true,true);
    if(s==FILM_OK)result->raw_received=true;
    if(s==FILM_OK)s=hold(&io,target,seq&~13u,fine);
    if(s==FILM_OK)result->raw_valid=true;
    result->capture_status=s;
    trace(&io,"restore");FilmStatus restore=io.idle(io.context);
    if(restore!=FILM_OK){result->restore_status=restore;return restore;}
    for(unsigned i=10;i>0;--i){FilmStatus r=io.write(io.context,false,addresses[i-1],saved[i-1]);if(r!=FILM_OK)restore=r;}
    FilmStatus r=io.write(io.context,true,0xa102,seq);if(r!=FILM_OK)restore=r;
    r=hold(&io,saved,seq,fine);if(r!=FILM_OK)restore=r;
    if(restore==FILM_OK)restore=io.restore_auto(io.context);
    if(restore==FILM_OK)restore=io.frame(io.context,false,false);
    result->restore_status=restore;result->camera_restored=restore==FILM_OK;
    return restore!=FILM_OK?restore:s;
}
