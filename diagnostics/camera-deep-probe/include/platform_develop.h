#ifndef FILM_PLATFORM_DEVELOP_H
#define FILM_PLATFORM_DEVELOP_H
#include "film.h"

typedef struct {
    bool gpu_accepted;
    const char *gpu_error;
    unsigned tiles, sample_max;
    unsigned max_error, p99_error, seam_error;
    unsigned peak_x, peak_y, over_four;
    double mean_error;
    uint64_t cpu_us, gpu_us;
} FilmResizeQuality;

FilmStatus film_3ds_resize_quality(const FilmImage *image, unsigned factor, FilmResizeQuality *quality);

typedef struct {
    unsigned max_error, p99_error;
    double mean_error;
    uint64_t prepare_us, cpu_us, gpu_us;
} FilmLayerQuality;

const char *film_3ds_layer_quality(unsigned side, FilmLayerQuality *quality);
#endif
