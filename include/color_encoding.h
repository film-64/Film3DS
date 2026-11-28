#ifndef FILM_COLOR_ENCODING_H
#define FILM_COLOR_ENCODING_H

#include "film.h"

/* FilmTransfer's persisted numeric values remain unchanged. Resolve them to
 * an explicit curve AND gamut; FLOG2C is F-Log2 on F-Gamut C, not BT.709. */
typedef enum {
    FILM_CURVE_LINEAR, FILM_CURVE_SRGB, FILM_CURVE_REC709, FILM_CURVE_FLOG2
} FilmEncodingCurve;
typedef enum { FILM_GAMUT_BT709_D65, FILM_GAMUT_FGAMUT_C_D65 } FilmEncodingGamut;
typedef struct {
    const char *name;
    FilmEncodingCurve curve;
    FilmEncodingGamut gamut;
} FilmColorEncoding;

/* NULL for unknown IDs and the retired generic VENDOR_LOG placeholder. */
const FilmColorEncoding *film_color_encoding(FilmTransfer transfer);
bool film_color_encoding_parse(const char *name, FilmTransfer *out);

/* Pixel-loop primitives: caller validates the profile and finite input first.
 * Working RGB is scene-linear BT.709/D65. No gamut clipping, shoulder, WB,
 * exposure or display rendering is performed here. Unsupported IDs yield NaN.
 * Negative/superwhite values use formula extension, not a camera-range claim. */
FilmRgb film_color_encode_linear(FilmRgb linear, FilmTransfer to);
FilmRgb film_color_decode_linear(FilmRgb encoded, FilmTransfer from);

/* Checked boundary for tools/tests. On failure *out is unchanged. */
FilmStatus film_color_convert(FilmRgb value, FilmTransfer from,
                              FilmTransfer to, FilmRgb *out);

#endif
