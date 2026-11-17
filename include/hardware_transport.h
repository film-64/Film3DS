#ifndef FILM_HARDWARE_TRANSPORT_H
#define FILM_HARDWARE_TRANSPORT_H
#include "film.h"
#include "film_runtime_contract.h"

typedef struct {
    void *context;
    int32_t (*open)(void *, const char *, uint32_t *);
    int32_t (*exchange)(void *, uint32_t, const uint32_t[4], uint32_t[3]);
    void (*close)(void *, uint32_t);
} FilmRegisterIo;
typedef struct {
    FilmRegisterIo io;
    uint32_t register_handle, native_result;
} FilmRegisterTransport;

FilmStatus film_register_open(FilmRegisterTransport *transport, FilmRegisterIo io);
FilmStatus film_register_access(FilmRegisterTransport *transport, bool writing, bool mcu,
                                uint16_t address, uint16_t *value);
void film_register_close(FilmRegisterTransport *transport);
FilmStatus film_camera_bool_reply(unsigned command,const uint32_t reply[3],bool *value);
#endif
