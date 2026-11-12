#ifndef FILM_PLATFORM_QA_H
#define FILM_PLATFORM_QA_H
#include <stdbool.h>
#include <stdio.h>

typedef struct {
    FILE *output;
    void *soc_buffer;
    int socket_fd;
} FilmHardwareQA;

bool film_hardware_qa_open(FilmHardwareQA *qa);
bool film_hardware_qa_confirm(FilmHardwareQA *qa);
void film_hardware_qa_close(FilmHardwareQA *qa);
void film_hardware_qa_camera(FILE *output, bool send_frames);

#endif
