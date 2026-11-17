#ifndef FILM_LAYERED_GRAIN_H
#define FILM_LAYERED_GRAIN_H
#include "film.h"
typedef struct FilmLayeredGrain {
    float amount,size,coarse,shadow,mid,highlight;
    unsigned reference_width;
    uint32_t seed;
} FilmLayeredGrain;
typedef struct FilmGrainTexture { unsigned side;float code[9];int16_t *pixels; } FilmGrainTexture;
typedef FilmStatus (*FilmGrainMixer)(unsigned,unsigned,const float *,float *);
FilmStatus film_grain_gpu_encoding(const char *,const float *,size_t,float *);
FilmStatus film_grain_layered_mix(const char *,FilmImage *,const FilmLayeredGrain *,
    const FilmGrainProfile *,const FilmGrainTexture *,FilmGrainMixer);
FilmStatus film_grain_texture_load(FILE *file,FilmGrainTexture *out);
void film_grain_texture_free(FilmGrainTexture *texture);
FilmStatus film_grain_layered(const char *source,FilmImage *image,
    const FilmLayeredGrain *params,const FilmGrainProfile *measured);
FilmStatus film_grain_layered_texture(const char *source,FilmImage *image,
    const FilmLayeredGrain *params,const FilmGrainProfile *measured,const FilmGrainTexture *texture);
#endif
