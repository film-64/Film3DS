#include "hardware_transport.h"
#include <assert.h>
#include <stdio.h>

/* Native service behavior from Batch28 manifest/identity records:
 * cam:u / cam:s -> d8a053ea; cam:c, register0000 -> 2280.
 * Exercise the production transport, including service selection and packets. */
typedef struct {
    unsigned opens, exchanges, closes;
    bool denied, transport_error, service_error, malformed;
    uint32_t last_request[4];
} Native;
static const int32_t denied = (int32_t)UINT32_C(0xd8a053ea);
static int32_t open_service(void *ctx, const char *service, uint32_t *handle)
{
    Native *n = ctx;
    ++n->opens;
    if (n->denied || strcmp(service, "cam:c")) return denied;
    *handle = 0xcafe;
    return 0;
}
static int32_t exchange(void *ctx, uint32_t handle, const uint32_t request[4], uint32_t reply[3])
{
    Native *n = ctx;
    ++n->exchanges;
    assert(handle == 0xcafe);
    memcpy(n->last_request, request, sizeof n->last_request);
    if (n->transport_error) return denied;
    unsigned command = request[0] >> 16;
    bool writing = command == 0x2d || command == 0x2e;
    assert(request[1] == 1);
    assert((request[0] & 65535) == ((writing ? 3u : 2u) << 6));
    reply[0] = (command << 16) | ((writing ? 1u : 2u) << 6);
    reply[1] = n->service_error || request[2] == 0x3000 ? (uint32_t)denied : 0;
    reply[2] = request[2] == 0 ? 0xa5a52280 : 128;
    if (n->malformed) reply[0] = 0;
    return 0;
}
static void close_service(void *ctx, uint32_t handle)
{
    assert(handle == 0xcafe);
    ++((Native *)ctx)->closes;
}
static FilmRegisterIo io(Native *native)
{
    return (FilmRegisterIo){native, open_service, exchange, close_service};
}
int main(void)
{
    assert(film_runtime_source_valid("film-camera-native-preview/v1"));
    assert(film_runtime_source_valid("film-camera-native-exposure/v1"));
    assert(film_runtime_source_valid("film-camera-native-capture/v1"));
    assert(film_runtime_source_valid("film-camera-manual-setup/v1"));
    assert(!film_runtime_source_valid("film-camera-native-preview/unknown"));
    bool decoded=true;uint32_t reply[3]={0x30080,0,0xa5a50000};
    assert(film_camera_bool_reply(3,reply,&decoded)==FILM_OK&&!decoded);
    reply[2]=0xa5a50001;assert(film_camera_bool_reply(3,reply,&decoded)==FILM_OK&&decoded);
    reply[2]=0xa5a50002;assert(film_camera_bool_reply(3,reply,&decoded)==FILM_CORRUPT&&decoded);
    reply[0]=0x30040;assert(film_camera_bool_reply(3,reply,&decoded)==FILM_CORRUPT);
    reply[0]=0x80080;assert(film_camera_bool_reply(3,reply,&decoded)==FILM_CORRUPT);
    assert(film_camera_bool_reply(9,reply,&decoded)==FILM_STATE);
    reply[1]=0xffffffff;assert(film_camera_bool_reply(8,reply,&decoded)==FILM_DEVICE);
    Native native = {0};
    FilmRegisterTransport transport = {0};
    assert(film_register_open(&transport, io(&native)) == FILM_OK);
    assert(native.opens == 1);
    assert(film_register_open(&transport, io(&native)) == FILM_STATE);
    uint16_t value = 0;
    assert(film_register_access(&transport, false, false, FILM_HW_CHIP_ADDRESS, &value) == FILM_OK);
    assert(native.last_request[2] == 0x0000 && value == 0x2280);
    assert(film_register_access(&transport, false, false, 0x3000, &value) == FILM_DEVICE);
    assert(transport.native_result == (uint32_t)denied);
    value = 32;
    assert(film_register_access(&transport, true, true, 0xa355, &value) == FILM_OK);
    assert(native.last_request[0] >> 16 == 0x2e && native.last_request[3] == 32);
    assert(film_register_access(&transport, false, true, 0xa34e, &value) == FILM_OK && value == 128);
    value = 64;
    assert(film_register_access(&transport, true, false, 0x3012, &value) == FILM_OK);
    assert(native.last_request[0] >> 16 == 0x2d);
    native.transport_error = true;
    assert(film_register_access(&transport, false, false, 0, &value) == FILM_DEVICE);
    assert(transport.native_result == (uint32_t)denied);
    native.transport_error = false; native.service_error = true;
    assert(film_register_access(&transport, false, false, 0, &value) == FILM_DEVICE);
    native.service_error = false; native.malformed = true;
    assert(film_register_access(&transport, false, false, 0, &value) == FILM_CORRUPT);
    film_register_close(&transport);
    film_register_close(&transport);
    assert(native.closes == 1);
    assert(film_register_access(&transport, false, false, 0, &value) == FILM_STATE);
    native = (Native){.denied = true};
    assert(film_register_open(&transport, io(&native)) == FILM_DEVICE);
    assert(native.opens == 1 && native.exchanges == 0 && !transport.register_handle);
    puts("PASS CAM register service, chip address, exact IPC, 16-bit reply, denied/malformed/closed boundaries");
    return 0;
}
