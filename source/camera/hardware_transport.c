#include "hardware_transport.h"

/* run05 receiver_decode_bool: validate the reply before narrowing to u8. */
FilmStatus film_camera_bool_reply(unsigned command,const uint32_t reply[3],bool *value)
{
    if(!film_runtime_source_valid("film-camera-bool-reply/v1"))return FILM_STATE;
    if(!reply||!value)return FILM_INVALID;
    if(command!=3&&command!=8&&command!=0x1a&&command!=0x1c)return FILM_STATE;
    if((int32_t)reply[1]<0)return FILM_DEVICE;
    if((reply[0]>>16)!=command||((reply[0]>>6)&63)<2)return FILM_CORRUPT;
    uint32_t decoded=reply[2]&255u;
    if(decoded>1)return FILM_CORRUPT;
    *value=decoded!=0;return FILM_OK;
}

FilmStatus film_register_open(FilmRegisterTransport *t, FilmRegisterIo io)
{
    if (!t || !io.open || !io.exchange || !io.close) return FILM_INVALID;
    if (t->register_handle) return FILM_STATE;
    *t = (FilmRegisterTransport){.io = io};
    int32_t rc = io.open(io.context, FILM_HW_REGISTER_SERVICE, &t->register_handle);
    t->native_result = (uint32_t)rc;
    if (rc < 0) { t->register_handle = 0; return FILM_DEVICE; }
    return t->register_handle ? FILM_OK : FILM_DEVICE;
}

FilmStatus film_register_access(FilmRegisterTransport *t, bool writing, bool mcu,
                                uint16_t address, uint16_t *value)
{
    if (!t || !value) return FILM_INVALID;
    if (!t->register_handle) return FILM_STATE;
    unsigned command = writing ? (mcu ? 0x2e : 0x2d) : (mcu ? 0x30 : 0x2f);
    uint32_t request[4] = {(command << 16) | ((writing ? 3u : 2u) << 6),
        FILM_HW_SENSOR_SELECT, address, writing ? *value : 0};
    uint32_t reply[3] = {0};
    int32_t rc = t->io.exchange(t->io.context, t->register_handle, request, reply);
    if (rc >= 0) rc = (int32_t)reply[1];
    t->native_result = (uint32_t)rc;
    if (rc < 0) return FILM_DEVICE;
    if ((reply[0] >> 16) != command || ((reply[0] >> 6) & 63) < (writing ? 1u : 2u))
        return FILM_CORRUPT;
    if (!writing) *value = reply[2] & 65535;
    return FILM_OK;
}

void film_register_close(FilmRegisterTransport *t)
{
    if (t && t->register_handle) {
        t->io.close(t->io.context, t->register_handle);
        t->register_handle = 0;
    }
}
