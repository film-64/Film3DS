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
bool film_raw_capture_qa(const char *token,const char *recover,bool recover_only);
void film_hardware_qa_camera(FILE *output, bool send_frames);
bool film_app_evidence_serve(const char *run,const char *const paths[4]);

#endif
