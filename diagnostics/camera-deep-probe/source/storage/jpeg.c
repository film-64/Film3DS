#include "storage_internal.h"

#include <jpeglib.h>
#include <jerror.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct jpeg_compress_struct compressor;
    struct jpeg_error_mgr error;
    jmp_buf escape;
    FilmStatus status;
} Encoder;

static void jpeg_failed(j_common_ptr common)
{
    Encoder *encoder = common->client_data;
    encoder->status = common->err->msg_code == JERR_OUT_OF_MEMORY ? FILM_NOMEM :
                      common->err->msg_code == JERR_FILE_WRITE ? FILM_IO : FILM_INVALID;
    longjmp(encoder->escape, 1);
}

static void exif_entry(uint8_t *p, unsigned tag, unsigned type, unsigned count, uint32_t value)
{
    p[0] = tag & 255; p[1] = tag >> 8; p[2] = type;
    p[4] = count & 255; p[5] = count >> 8;
    for (unsigned i = 0; i < 4; ++i) p[8 + i] = (uint8_t)(value >> (i * 8));
}

static void write_exif(j_compress_ptr jpeg, unsigned w, unsigned h)
{
    // TIFF offsets exclude the six-byte Exif identifier.
    uint8_t exif[120] = {'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8};
    uint8_t *tiff = exif + 6;
    tiff[8] = 3;
    exif_entry(tiff + 10, 0x0112, 3, 1, 1);
    exif_entry(tiff + 22, 0x0131, 2, 8, 50);
    exif_entry(tiff + 34, 0x8769, 4, 1, 58);
    memcpy(tiff + 50, "Film3DS", 8);
    tiff[58] = 4;
    exif_entry(tiff + 60, 0x9000, 7, 4, UINT32_C(0x31333230)); // "0231"
    exif_entry(tiff + 72, 0xa001, 3, 1, 1);
    exif_entry(tiff + 84, 0xa002, 4, 1, w);
    exif_entry(tiff + 96, 0xa003, 4, 1, h);
    jpeg_write_marker(jpeg, JPEG_APP0 + 1, exif, sizeof exif);
}

FilmStatus film_jpeg_write(const char *path, const FilmImage *image, unsigned quality)
{
    if (!image || !image->pixels || image->format != FILM_RGB24 || quality < 1 || quality > 100 ||
        !film_image_bytes(image->width, image->height, image->format)) return FILM_INVALID;
    Encoder *encoder = calloc(1, sizeof *encoder);
    if (!encoder) return FILM_NOMEM;
    FilmWrite write;
    FilmStatus status = film_write_begin(&write, path);
    if (status != FILM_OK) { free(encoder); return status; }
    j_compress_ptr jpeg = &encoder->compressor;
    jpeg->err = jpeg_std_error(&encoder->error);
    jpeg->err->error_exit = jpeg_failed;
    jpeg->client_data = encoder;
    if (setjmp(encoder->escape) == 0) {
        jpeg_create_compress(jpeg);
        jpeg_stdio_dest(jpeg, write.file);
        jpeg->image_width = image->width;
        jpeg->image_height = image->height;
        jpeg->input_components = 3;
        jpeg->in_color_space = JCS_RGB;
        jpeg_set_defaults(jpeg);
        jpeg->write_JFIF_header = FALSE;
        jpeg_set_quality(jpeg, quality, TRUE);
        jpeg_start_compress(jpeg, TRUE);
        write_exif(jpeg, image->width, image->height);
        while (jpeg->next_scanline < jpeg->image_height) {
            JSAMPROW row = image->pixels + (size_t)jpeg->next_scanline * image->width * 3;
            jpeg_write_scanlines(jpeg, &row, 1);
        }
        jpeg_finish_compress(jpeg);
    }
    jpeg_destroy_compress(jpeg);
    status = encoder->status;
    free(encoder);
    return film_write_end(&write, status);
}
