#pragma once
/* Registered local installer side effect: write only the named field app,
 * icon, initial reference labels (if absent), and install receipt. No camera.
 * Temp write -> close -> CRC readback -> rename. Any failure stops install.
 * Existing label history is never replaced. No filesystem retry/fallback.
 * One existing startup checkpoint uploads the receipt, not measurement work.
 */
#include "../../build-exposure-field-payload/field_payload.h"
static bool field_install_file(const char *path,const u8 *data,size_t size,u32 expected)
{
    char temporary[256];snprintf(temporary,sizeof temporary,"%s.installing",path);
    FILE *file=fopen(temporary,"wb");if(!file)return false;
    bool ok=fwrite(data,1,size,file)==size;if(fclose(file))ok=false;if(!ok)return false;
    file=fopen(temporary,"rb");if(!file)return false;
    u8 buffer[4096];size_t total=0;uLong crc=crc32(0,NULL,0);size_t n;
    while((n=fread(buffer,1,sizeof buffer,file))){crc=crc32(crc,buffer,n);total+=n;}
    ok=!ferror(file) && total==size && crc==expected;if(fclose(file))ok=false;
    return ok && rename(temporary,path)==0;
}
static bool field_install_assets(void)
{
    mkdir("sdmc:/3ds",0777);mkdir(FIELD_ROOT,0777);
    bool ok=field_install_file(FIELD_ROOT "/FilmExposureField.3dsx",field_app,sizeof field_app,FIELD_APP_CRC) &&
            field_install_file(FIELD_ROOT "/FilmExposureField.smdh",field_icon,sizeof field_icon,FIELD_ICON_CRC);
    struct stat st;
    if(ok && stat(FIELD_LABELS,&st)!=0) {
        if(errno!=ENOENT)return false;
        ok=field_install_file(FIELD_LABELS,field_seed,sizeof field_seed,FIELD_SEED_CRC);
    }
    if(!ok)return false;
    mkdir(FIELD_ROOT "/install",0777);
    FILE *file=fopen(FIELD_ROOT "/install/receipt.txt","w");if(!file)return false;
    fprintf(file,"exposure-field-install/v1\napp_bytes=%u\napp_crc32=%08x\napp_sha256=%s\nreadback_crc_verified=1\nlabels_preserved_if_present=1\n",
        (unsigned)sizeof field_app,FIELD_APP_CRC,FIELD_APP_SHA256);
    ok=!ferror(file);if(fclose(file))ok=false;
    if(ok)remote_upload_checkpoint(FIELD_ROOT "/install","startup");
    return ok;
}
