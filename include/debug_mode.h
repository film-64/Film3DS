#ifndef FILM_DEBUG_MODE_H
#define FILM_DEBUG_MODE_H
#include <stdbool.h>
enum {FILM_DEBUG_A=1,FILM_DEBUG_B=2,FILM_DEBUG_UP=4,FILM_DEBUG_DOWN=8,FILM_DEBUG_OTHER=16};
typedef struct {unsigned debug_sequence;bool debug_mode;} FilmDebugMode;
bool film_debug_mode_key(FilmDebugMode *,unsigned);
#endif
