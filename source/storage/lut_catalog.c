#include "lut_catalog.h"
#include "film_runtime_contract.h"
#include "storage_internal.h"
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static bool cube(const char *name){size_t n=strlen(name);return n>5&&n<256&&!strcasecmp(name+n-5,".cube");}
static bool source_is(const char *s,const char *expected){return s&&!strcmp(s,expected)&&film_runtime_source_valid(s);}
static bool path(char out[768],const char *directory,const char *name)
{int n=snprintf(out,768,"%s%s%s",directory,directory[strlen(directory)-1]=='/'?"":"/",name);return n>0&&n<768;}
static int order(const void *a,const void *b){return strcasecmp(((const FilmLook *)a)->name,((const FilmLook *)b)->name);}
void film_lut_catalog_free(FilmLookCatalog *c){if(c){free(c->looks);*c=(FilmLookCatalog){0};}}
FilmStatus film_lut_catalog_scan(const char *source,const char *directory,FilmLookCatalog *out)
{
    if(!source_is(source,"film-lut-catalog/v1"))return FILM_STATE;
    if(!directory||!*directory||!out||out->looks||out->count)return FILM_INVALID;
    DIR *d=opendir(directory);if(!d)return FILM_IO;FilmLookCatalog result={0};FilmStatus s=FILM_OK;struct dirent *e;
    while((e=readdir(d))){if(!cube(e->d_name))continue;char name[768];struct stat st;
        if(!path(name,directory,e->d_name)){s=FILM_INVALID;break;}
        if(stat(name,&st)){s=FILM_IO;break;}if(!S_ISREG(st.st_mode))continue;
        if(result.count==UINT32_MAX||(size_t)result.count+1>SIZE_MAX/sizeof(FilmLook)){s=FILM_NOMEM;break;}
        FilmLook *items=realloc(result.looks,((size_t)result.count+1)*sizeof *items);if(!items){s=FILM_NOMEM;break;}result.looks=items;
        FilmLook *look=&items[result.count];*look=(FilmLook){.directory=directory};strcpy(look->name,e->d_name);
        if(strlen(e->d_name)<sizeof look->key)strcpy(look->key,e->d_name);
        else{uint64_t hash=UINT64_C(14695981039346656037);for(const unsigned char *p=(const unsigned char *)e->d_name;*p;++p){hash^=*p;hash*=UINT64_C(1099511628211);}snprintf(look->key,sizeof look->key,"cube:%016" PRIx64,hash);}
        for(unsigned i=0;i<result.count;++i)if(!strcmp(items[i].key,look->key)){s=FILM_STATE;break;}
        if(s!=FILM_OK)break;++result.count;
    }
    if(closedir(d))s=FILM_IO;
    if(s!=FILM_OK){film_lut_catalog_free(&result);return s;}
    if(result.count)qsort(result.looks,result.count,sizeof *result.looks,order);*out=result;return FILM_OK;
}
FilmStatus film_lut_install_missing(const char *source,const char *bundled,const char *directory,unsigned *installed)
{
    if(!source_is(source,"film-lut-install/v1"))return FILM_STATE;
    if(!installed)return FILM_INVALID;*installed=0;FilmLookCatalog catalog={0};
    FilmStatus s=film_lut_catalog_scan("film-lut-catalog/v1",bundled,&catalog);if(s!=FILM_OK)return s;
    if(!directory||!*directory){film_lut_catalog_free(&catalog);return FILM_INVALID;}
    for(unsigned i=0;s==FILM_OK&&i<catalog.count;++i){char from[768],to[768];struct stat st;
        if(!path(from,bundled,catalog.looks[i].name)||!path(to,directory,catalog.looks[i].name)){s=FILM_INVALID;break;}
        if(!stat(to,&st)){if(!S_ISREG(st.st_mode))s=FILM_IO;continue;}if(errno!=ENOENT){s=FILM_IO;break;}
        FILE *input=fopen(from,"rb");if(!input){s=FILM_IO;break;}FilmWrite write;s=film_write_begin(&write,to);
        if(s==FILM_OK){uint8_t bytes[4096];size_t n;while((n=fread(bytes,1,sizeof bytes,input)))if(fwrite(bytes,1,n,write.file)!=n){s=FILM_IO;break;}
            if(ferror(input))s=FILM_IO;if(fclose(input))s=FILM_IO;s=film_write_end(&write,s);if(s==FILM_OK)++*installed;
        }else fclose(input);
    }
    film_lut_catalog_free(&catalog);return s;
}
