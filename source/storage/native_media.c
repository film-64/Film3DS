#include "native_media.h"
#include "film_runtime_contract.h"
#include <string.h>

typedef struct { size_t end, mpf, mpf_bytes; unsigned width, height; } Jpeg;

static unsigned be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned t16(const uint8_t *p, bool little)
{
    return little ? ((unsigned)p[1] << 8) | p[0] : be16(p);
}
static uint32_t t32(const uint8_t *p, bool little)
{
    if (little) return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Structural marker scan only. Entropy correctness is the decoder's job.
 * Embedded Exif thumbnails are not mistaken for extra top-level JPEG images. */
static bool jpeg(const uint8_t *p, size_t bytes, Jpeg *out)
{
    *out = (Jpeg){0};
    if (bytes < 4 || p[0] != 0xff || p[1] != 0xd8) return false;
    size_t pos = 2;
    bool scanned = false, sof = false;
    while (pos < bytes) {
        if (p[pos++] != 0xff) return false;
        while (pos < bytes && p[pos] == 0xff) ++pos;
        if (pos == bytes) return false;
        unsigned marker = p[pos++];
        if (marker == 0xd9) {
            if (!sof || !scanned) return false;
            out->end = pos;
            return true;
        }
        if (marker == 0 || marker == 0xd8 || marker == 1 ||
            (marker >= 0xd0 && marker <= 0xd7) || bytes-pos < 2) return false;
        unsigned length = be16(p+pos);
        if (length < 2 || length > bytes-pos) return false;
        const uint8_t *body = p+pos+2;
        size_t payload = length-2;
        if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
            if (sof || (marker != 0xc0 && marker != 0xc1 && marker != 0xc2) || payload < 6) return false;
            unsigned components = body[5];
            if (body[0] != 8 || !components || components > 4 || payload != 6+3*components) return false;
            out->height = be16(body+1); out->width = be16(body+3);
            if (!out->width || !out->height || out->width > FILM_NATIVE_MEDIA_MAX_DIMENSION ||
                out->height > FILM_NATIVE_MEDIA_MAX_DIMENSION) return false;
            sof = true;
        }
        if (marker == 0xe2 && payload >= 4 && !memcmp(body,"MPF\0",4)) {
            if (out->mpf || scanned) return false;
            out->mpf = pos+6; /* TIFF header after the four-byte MPF identifier. */
            out->mpf_bytes = payload-4;
        }
        pos += length;
        if (marker == 0xda) {
            if (!sof || payload < 4 || !body[0] || body[0] > 4 || payload != 4u+2u*body[0]) return false;
            scanned = true;
            while (pos < bytes) {
                if (p[pos] != 0xff) { ++pos; continue; }
                size_t start = pos++;
                while (pos < bytes && p[pos] == 0xff) ++pos;
                if (pos == bytes) return false;
                unsigned next = p[pos];
                if (next == 0 || (next >= 0xd0 && next <= 0xd7)) { ++pos; continue; }
                pos = start;
                break;
            }
        }
    }
    return false;
}

static bool tiff_header(const uint8_t *p, size_t bytes, bool *little)
{
    if (bytes < 8) return false;
    if (!memcmp(p,"II",2)) *little = true;
    else if (!memcmp(p,"MM",2)) *little = false;
    else return false;
    return t16(p+2,*little) == 42;
}

static bool mp_attributes(const uint8_t *p, size_t bytes, bool secondary)
{
    bool little;
    if (!tiff_header(p,bytes,&little)) return false;
    size_t ifd = t32(p+4,little), seen[FILM_NATIVE_MEDIA_MAX_MPF_IFDS];
    unsigned count = 0;
    static const unsigned widths[] = {0,1,1,2,4,8,1,1,2,4,8,4,8};
    if (!ifd) return false;
    while (ifd) {
        if (count == FILM_NATIVE_MEDIA_MAX_MPF_IFDS || ifd < 8 || ifd > bytes || bytes-ifd < 6) return false;
        for (unsigned i = 0; i < count; ++i) if (seen[i] == ifd) return false;
        seen[count++] = ifd;
        unsigned tags = t16(p+ifd,little);
        if (tags > (bytes-ifd-6)/12) return false;
        for (unsigned i = 0; i < tags; ++i) {
            const uint8_t *tag = p+ifd+2+12*i;
            unsigned id = t16(tag,little), type = t16(tag+2,little);
            uint32_t n = t32(tag+4,little), value = t32(tag+8,little);
            if (!type || type >= sizeof widths / sizeof *widths || n > bytes/widths[type]) return false;
            size_t length = (size_t)n*widths[type];
            if (length > 4 && (value > bytes || length > bytes-value)) return false;
            if ((secondary || count > 1) && (id == 0xb001 || id == 0xb002)) return false;
        }
        ifd = t32(p+ifd+2+12*tags,little);
    }
    return true;
}

static bool mp_entries(const uint8_t *p, size_t bytes, size_t *table, bool *little)
{
    if (!tiff_header(p,bytes,little) || !mp_attributes(p,bytes,false)) return false;
    size_t ifd = t32(p+4,*little);
    if (ifd < 8 || ifd > bytes || bytes-ifd < 6) return false;
    unsigned entries = t16(p+ifd,*little);
    if (entries > (bytes-ifd-6)/12) return false;
    bool count = false, members = false;
    for (unsigned i = 0; i < entries; ++i) {
        const uint8_t *tag = p+ifd+2+12*i;
        unsigned id = t16(tag,*little), type = t16(tag+2,*little);
        uint32_t n = t32(tag+4,*little), value = t32(tag+8,*little);
        if (id == 0xb001) {
            if (count || type != 4 || n != 1 || value != 2) return false;
            count = true;
        } else if (id == 0xb002) {
            if (members || type != 7 || n != 32 || value < 8 || value > bytes || bytes-value < 32) return false;
            *table = value;
            members = true;
        }
    }
    size_t ifd_end = ifd+6+12*entries;
    size_t next = t32(p+ifd_end-4,*little);
    if ((next && (next > bytes || bytes-next < 6)) || !count || !members) return false;
    return *table+32 <= ifd || *table >= ifd_end;
}

FilmStatus film_native_media_inspect(const char *source, const uint8_t *data,
                                    size_t bytes, FilmNativeMedia *out)
{
    if (out) *out = (FilmNativeMedia){0};
    if (!source || strcmp(source,"film-native-media-inspect/v1") || !film_runtime_source_valid(source)) return FILM_STATE;
    if (!data || !out || bytes > FILM_NATIVE_MEDIA_MAX_BYTES) return FILM_INVALID;
    Jpeg first;
    if (!jpeg(data,bytes,&first)) return FILM_CORRUPT;
    FilmNativeMedia result = {0};
    if (!first.mpf) {
        if (first.end != bytes) return FILM_CORRUPT;
        result.count = 1;
        result.images[0] = (FilmNativeJpeg){0,bytes,first.width,first.height};
    } else {
        size_t table = 0;
        bool little = false;
        const uint8_t *tiff = data+first.mpf;
        if (!mp_entries(tiff,first.mpf_bytes,&table,&little)) return FILM_CORRUPT;
        size_t prior_end = 0;
        for (unsigned i = 0; i < 2; ++i) {
            const uint8_t *entry = tiff+table+16*i;
            uint32_t attributes = t32(entry,little), length = t32(entry+4,little), relative = t32(entry+8,little);
            if ((attributes & UINT32_C(0x07000000)) || t16(entry+12,little) > 2 || t16(entry+14,little) > 2) return FILM_CORRUPT;
            if ((i == 0 && relative != 0) || (i == 1 && relative == 0)) return FILM_CORRUPT;
            if (i && relative > bytes-first.mpf) return FILM_CORRUPT;
            size_t offset = i ? first.mpf+relative : 0;
            if (!length || offset != prior_end || offset > bytes || length > bytes-offset) return FILM_CORRUPT;
            Jpeg member;
            if (!jpeg(data+offset,length,&member) ||
                member.width != first.width || member.height != first.height ||
                (!i && member.end != first.end)) return FILM_CORRUPT;
            size_t padding = length-member.end;
            if ((i && padding) || padding > FILM_NATIVE_MEDIA_MAX_PADDING) return FILM_CORRUPT;
            for (size_t j = member.end; j < length; ++j) if (data[offset+j]) return FILM_CORRUPT;
            if (i && member.mpf && !mp_attributes(data+offset+member.mpf,member.mpf_bytes,true)) return FILM_CORRUPT;
            result.images[i] = (FilmNativeJpeg){offset,member.end,member.width,member.height};
            prior_end = offset+length;
        }
        if (prior_end != bytes) return FILM_CORRUPT;
        result.count = 2;
    }
    *out = result;
    return FILM_OK;
}
