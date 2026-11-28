#include "color_encoding.h"
#include <math.h>
#include <string.h>

static const FilmColorEncoding encodings[] = {
    {"SRGB", FILM_CURVE_SRGB, FILM_GAMUT_BT709_D65},
    {"LINEAR_SRGB", FILM_CURVE_LINEAR, FILM_GAMUT_BT709_D65},
    {NULL, FILM_CURVE_LINEAR, FILM_GAMUT_BT709_D65},
    {"REC709", FILM_CURVE_REC709, FILM_GAMUT_BT709_D65},
    {"FLOG2C", FILM_CURVE_FLOG2, FILM_GAMUT_FGAMUT_C_D65}
};

const FilmColorEncoding *film_color_encoding(FilmTransfer transfer)
{
    if ((unsigned)transfer >= sizeof encodings / sizeof *encodings ||
        !encodings[transfer].name) return NULL;
    return &encodings[transfer];
}

bool film_color_encoding_parse(const char *name, FilmTransfer *out)
{
    if (!name || !out) return false;
    for (unsigned i = 0; i < sizeof encodings / sizeof *encodings; ++i) {
        if (encodings[i].name && !strcmp(name, encodings[i].name)) {
            *out = (FilmTransfer)i;
            return true;
        }
    }
    return false;
}

static float encode(float v, FilmEncodingCurve curve)
{
    switch (curve) {
    case FILM_CURVE_LINEAR: return v;
    case FILM_CURVE_SRGB:
        return v <= .0031308f ? 12.92f * v : 1.055f * powf(v, 1.f / 2.4f) - .055f;
    case FILM_CURVE_REC709:
        return v < .018f ? 4.5f * v : 1.099f * powf(v, .45f) - .099f;
    case FILM_CURVE_FLOG2:
        /* Fujifilm F-Log2 C Data Sheet v1.0, sections 2-3 and 3.
         * https://dl.fujifilm-x.com/technical-data/F-Log2C_DataSheet_E_Ver.1.0.pdf */
        return v < .000889f ? 8.799461f * v + .092864f :
            .245281f * log10f(5.555556f * v + .064829f) + .384316f;
    }
    return NAN;
}

static float decode(float v, FilmEncodingCurve curve)
{
    switch (curve) {
    case FILM_CURVE_LINEAR: return v;
    case FILM_CURVE_SRGB:
        return v <= .04045f ? v / 12.92f : powf((v + .055f) / 1.055f, 2.4f);
    case FILM_CURVE_REC709:
        return v < .081f ? v / 4.5f : powf((v + .099f) / 1.099f, 1.f / .45f);
    case FILM_CURVE_FLOG2:
        return v < .100686685370811f ? (v - .092864f) / 8.799461f :
            (powf(10, (v - .384316f) / .245281f) - .064829f) / 5.555556f;
    }
    return NAN;
}

FilmRgb film_color_encode_linear(FilmRgb rgb, FilmTransfer to)
{
    const FilmColorEncoding *e = film_color_encoding(to);
    if (!e) return (FilmRgb){NAN, NAN, NAN};
    if (e->gamut == FILM_GAMUT_FGAMUT_C_D65) {
        /* Preserve the existing BT.709 -> F-Gamut C coefficients. Both D65. */
        float r = rgb.r, g = rgb.g, b = rgb.b;
        rgb = (FilmRgb){.517069024f*r + .412934676f*g + .069996300f*b,
                        .088617162f*r + .809263150f*g + .102119689f*b,
                        .017750040f*r + .109447621f*g + .872802339f*b};
    }
    return (FilmRgb){encode(rgb.r, e->curve), encode(rgb.g, e->curve), encode(rgb.b, e->curve)};
}

FilmRgb film_color_decode_linear(FilmRgb rgb, FilmTransfer from)
{
    const FilmColorEncoding *e = film_color_encoding(from);
    if (!e) return (FilmRgb){NAN, NAN, NAN};
    float r = decode(rgb.r, e->curve), g = decode(rgb.g, e->curve), b = decode(rgb.b, e->curve);
    if (e->gamut == FILM_GAMUT_FGAMUT_C_D65) {
        /* Inverse of the forward matrix, not a per-channel gamma reversal. */
        return (FilmRgb){2.1198514678f*r - 1.0757050522f*g - .0441464146f*b,
                        -.2303358554f*r + 1.3724421505f*g - .1421062965f*b,
                        -.0142274332f*r - .1502249877f*g + 1.1644524210f*b};
    }
    return (FilmRgb){r, g, b};
}

static bool finite_rgb(FilmRgb rgb)
{
    return isfinite(rgb.r) && isfinite(rgb.g) && isfinite(rgb.b);
}

FilmStatus film_color_convert(FilmRgb value, FilmTransfer from,
                              FilmTransfer to, FilmRgb *out)
{
    if (!out || !finite_rgb(value)) return FILM_INVALID;
    if (!film_color_encoding(from) || !film_color_encoding(to)) return FILM_UNSUPPORTED;
    if (from == to) { *out = value; return FILM_OK; }
    FilmRgb converted = film_color_encode_linear(film_color_decode_linear(value, from), to);
    if (!finite_rgb(converted)) return FILM_INVALID;
    *out = converted;
    return FILM_OK;
}
