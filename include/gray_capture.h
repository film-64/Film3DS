#ifndef FILM_GRAY_CAPTURE_H
#define FILM_GRAY_CAPTURE_H

#include "film.h"
#include "ae_region.h"

/* Calibration evidence from the last completed native YUYV preview, paired
 * by photo ID and Bayer source CRC with the separately verified original DNG.
 * The YUV frame precedes the shutter, and is NOT simultaneous with the RAW. */
typedef struct {
    FilmImage preview; /* owned by caller; exact 640x480 packed Y0 U Y1 V */
    FilmAeRegion meter_region; /* successfully programmed physical Native AE ROI */
    FilmYuv yuv;
    uint64_t preview_time_ms;
    unsigned id;
    uint32_t raw_source_crc;
    int native_level;
    unsigned exposure_mode;
    bool native_ae, native_awb;
} FilmGrayPair;

/* Atomic F3YG v2, 80-byte header + 614400 native YUYV bytes. Never overwrites
 * an existing calibration record. DNG and existing SD LUTs are untouched. */
FilmStatus film_gray_pair_write(const char *source,const char *directory,const FilmGrayPair *pair);
/* Readback/digest/length validation; missing or corrupted data is an error. */
FilmStatus film_gray_pair_verify(const char *source,const char *directory,unsigned id,uint32_t raw_source_crc);

#endif
