#include "native_decode.h"
#include "film_runtime_contract.h"
#include <jpeglib.h>
#include <jerror.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct jpeg_decompress_struct jpeg;
    struct jpeg_error_mgr error;
    jmp_buf escape;
    FilmStatus status;
} Decoder;

static void failed(j_common_ptr common)
{
    Decoder *d = common->client_data;
    d->status = common->err->msg_code == JERR_OUT_OF_MEMORY ||
                common->err->msg_code == JERR_NO_BACKING_STORE ? FILM_NOMEM : FILM_CORRUPT;
    longjmp(d->escape,1);
}

static void message(j_common_ptr common, int level)
{
    /* Libjpeg's synthetic recovery is unsuitable as a successful source image. */
    if (level < 0) failed(common);
}

static bool overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    if (!an || !bn) return false;
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (an > UINTPTR_MAX-x || bn > UINTPTR_MAX-y) return true;
    return x < y+bn && y < x+an;
}

FilmStatus film_native_jpeg_decode(const char *source, const uint8_t *data, size_t bytes,
                                  unsigned member, uint8_t *pixels, size_t stride,
                                  size_t capacity, unsigned *width, unsigned *height)
{
    if ((width && height && overlaps(width,sizeof *width,height,sizeof *height)) ||
        (width && (overlaps(data,bytes,width,sizeof *width) || overlaps(pixels,capacity,width,sizeof *width))) ||
        (height && (overlaps(data,bytes,height,sizeof *height) || overlaps(pixels,capacity,height,sizeof *height)))) return FILM_INVALID;
    if (width) *width = 0;
    if (height) *height = 0;
    if (!source || strcmp(source,"film-native-jpeg-decode/v1") || !film_runtime_source_valid(source)) return FILM_STATE;
    if (!pixels || !width || !height || width == height) return FILM_INVALID;
    FilmNativeMedia media;
    FilmStatus status = film_native_media_inspect("film-native-media-inspect/v1",data,bytes,&media);
    if (status != FILM_OK) return status;
    if (member >= media.count) return FILM_INVALID;
    FilmNativeJpeg image = media.images[member];
    if ((size_t)image.width*image.height > FILM_NATIVE_DECODE_MAX_PIXELS) return FILM_UNSUPPORTED;
    size_t row = (size_t)image.width*3;
    if (stride < row || (image.height > 1 && stride > (SIZE_MAX-row)/(image.height-1))) return FILM_INVALID;
    size_t needed = (image.height-1)*stride+row;
    if (capacity < needed || overlaps(data,bytes,pixels,needed) ||
        overlaps(data,bytes,width,sizeof *width) || overlaps(data,bytes,height,sizeof *height) ||
        overlaps(pixels,needed,width,sizeof *width) || overlaps(pixels,needed,height,sizeof *height)) return FILM_INVALID;
    Decoder *d = calloc(1,sizeof *d);
    if (!d) return FILM_NOMEM;
    d->status = FILM_CORRUPT;
    d->jpeg.err = jpeg_std_error(&d->error);
    d->error.error_exit = failed;
    d->error.emit_message = message;
    d->jpeg.client_data = d;
    if (setjmp(d->escape) == 0) {
        jpeg_create_decompress(&d->jpeg);
        d->jpeg.mem->max_memory_to_use = FILM_NATIVE_DECODE_VIRTUAL_BUDGET_BYTES;
        jpeg_mem_src(&d->jpeg,data+image.offset,(unsigned long)image.bytes);
        if (jpeg_read_header(&d->jpeg,TRUE) == JPEG_HEADER_OK) {
            J_COLOR_SPACE color = d->jpeg.jpeg_color_space;
            if (color != JCS_GRAYSCALE && color != JCS_YCbCr && color != JCS_RGB) {
                d->status = FILM_UNSUPPORTED;
            } else if (d->jpeg.image_width == image.width && d->jpeg.image_height == image.height) {
                d->jpeg.out_color_space = JCS_RGB;
                if (jpeg_start_decompress(&d->jpeg) && d->jpeg.output_components == 3 &&
                    d->jpeg.output_width == image.width && d->jpeg.output_height == image.height) {
                    while (d->jpeg.output_scanline < d->jpeg.output_height) {
                        JSAMPROW output = pixels+(size_t)d->jpeg.output_scanline*stride;
                        if (jpeg_read_scanlines(&d->jpeg,&output,1) != 1) break;
                    }
                    if (d->jpeg.output_scanline == d->jpeg.output_height && jpeg_finish_decompress(&d->jpeg)) d->status = FILM_OK;
                }
            }
        }
    }
    jpeg_destroy_decompress(&d->jpeg);
    status = d->status;
    free(d);
    if (status == FILM_OK) { *width = image.width; *height = image.height; }
    return status;
}
