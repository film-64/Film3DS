#include "film.h"
#include "color_encoding.h"
#include "film_runtime_contract.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Strict, bounded metadata. Versioned header and ordered key/value lines.
 * Missing metadata is compatible; malformed/stale metadata never falls back. */
static bool read_line(FILE *file, char line[128])
{
    size_t length=0; int ch;
    while ((ch=fgetc(file))!=EOF && ch!='\n') {
        if (!ch || length==127) return false;
        line[length++]=(char)ch;
    }
    if (ferror(file) || (!length && ch==EOF)) return false;
    if (length && line[length-1]=='\r') --length;
    line[length]=0;
    return true;
}

static bool field(FILE *file, const char *key, char value[64])
{
    char line[128], actual[64], extra[2];
    if (!read_line(file,line)) return false;
    return sscanf(line, "%63s %63s %1s", actual, value, extra) == 2 && !strcmp(actual, key);
}

static FilmStatus apply_adapter(FILE *file, FilmLut *lut)
{
    char line[128], value[64];
    if (!read_line(file,line)) return FILM_CORRUPT;
    bool version2=!strcmp(line,"F3LUT2");
    if (!version2 && strcmp(line,"F3LUT1")) return FILM_CORRUPT;
    if (!field(file,"cube_bytes",value) || !*value || strspn(value,"0123456789") != strlen(value))
        return FILM_CORRUPT;
    errno=0; char *end;
    unsigned long long bytes=strtoull(value,&end,10);
    if (errno || *end || !bytes || bytes!=lut->source_bytes) return FILM_CORRUPT;
    if (!field(file,"cube_crc32",value) || strlen(value)!=8 ||
        strspn(value,"0123456789abcdefABCDEF")!=8) return FILM_CORRUPT;
    errno=0; unsigned long crc=strtoul(value,&end,16);
    if (errno || *end || crc!=lut->source_crc32) return FILM_CORRUPT;
    FilmTransfer input,output;
    if (!field(file,"input",value)) return FILM_CORRUPT;
    if (!film_color_encoding_parse(value,&input)) return FILM_UNSUPPORTED;
    if (!field(file,"output",value)) return FILM_CORRUPT;
    if (!film_color_encoding_parse(value,&output)) return FILM_UNSUPPORTED;
    if (!field(file,"pre_lut_shoulder",value)) return FILM_CORRUPT;
    bool bypass=!strcmp(value,"bypass");
    if (!bypass && strcmp(value,"preserve")) return FILM_UNSUPPORTED;
    bool scene=false,map=false;
    if (version2) {
        if (!field(file,"output_intent",value)) return FILM_CORRUPT;
        scene=!strcmp(value,"scene");
        if (!scene && strcmp(value,"display")) return FILM_UNSUPPORTED;
        if (!field(file,"display_mapping",value)) return FILM_CORRUPT;
        map=!strcmp(value,"peak-shoulder");
        if (!map && strcmp(value,"none")) return FILM_UNSUPPORTED;
        /* v2 admits only completed display output, or an explicitly mapped
         * scene output. Reject double knees and ambiguous unmapped scenes. */
        if (scene!=map || (map && !bypass)) return FILM_UNSUPPORTED;
    }
    int ch;
    while ((ch=fgetc(file))!=EOF) if (!isspace((unsigned char)ch)) return FILM_CORRUPT;
    if (ferror(file)) return FILM_IO;
    if (lut->has_transfer && (lut->input!=input || lut->output!=output)) return FILM_CORRUPT;
    lut->input=input; lut->output=output; lut->has_transfer=true;
    lut->has_adapter=true; lut->bypass_raw_shoulder=bypass;
    lut->has_output_intent=version2; lut->scene_output=scene; lut->map_output_peak=map;
    return FILM_OK;
}

FilmStatus film_lut_load_path(const char *path, FilmLut *out, unsigned *error_line)
{
    if (error_line) *error_line=0;
    if (!film_runtime_source_valid("film-lut-adapter/v1")) return FILM_STATE;
    if (!path || !*path || !out || out->samples) return FILM_INVALID;
    /* Same path bounds as the on-device LUT loader. */
    char metadata[800];
    if (snprintf(metadata,sizeof metadata,"%s.f3a",path)>=(int)sizeof metadata) return FILM_INVALID;
    FILE *file=fopen(path,"rb");
    if (!file) return FILM_IO;
    FilmLut lut={0};
    FilmStatus status=film_lut_load(file,&lut,error_line);
    if (fclose(file) && status==FILM_OK) status=FILM_IO;
    if (status==FILM_OK) {
        file=fopen(metadata,"rb");
        if (!file) {
            if (errno!=ENOENT) status=FILM_IO;
        } else {
            status=apply_adapter(file,&lut);
            if (fclose(file) && status==FILM_OK) status=FILM_IO;
        }
    }
    if (status==FILM_OK) *out=lut;
    else film_lut_free(&lut);
    return status;
}

bool film_lut_bypasses_raw_shoulder(const FilmLut *lut, const FilmRecipe *recipe)
{
    return lut && lut->samples && lut->has_adapter && lut->bypass_raw_shoulder &&
           recipe && recipe->profile.strength>0;
}
