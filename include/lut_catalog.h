#ifndef FILM_LUT_CATALOG_H
#define FILM_LUT_CATALOG_H
#include "film.h"
typedef struct {const char *directory;char name[256],key[64];} FilmLook;
typedef struct {FilmLook *looks;unsigned count;} FilmLookCatalog;
FilmStatus film_lut_catalog_scan(const char *,const char *,FilmLookCatalog *);
void film_lut_catalog_free(FilmLookCatalog *);
FilmStatus film_lut_install_missing(const char *,const char *,const char *,unsigned *);
#endif
