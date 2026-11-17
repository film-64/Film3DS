#include "debug_mode.h"
bool film_debug_mode_key(FilmDebugMode *state,unsigned key)
{
    static const unsigned sequence[]={FILM_DEBUG_A,FILM_DEBUG_A,FILM_DEBUG_B,FILM_DEBUG_B,FILM_DEBUG_UP,FILM_DEBUG_UP,FILM_DEBUG_DOWN,FILM_DEBUG_DOWN};
    if(!state||!key)return false;
    if(state->debug_sequence>=8)state->debug_sequence=0;
    if(key==sequence[state->debug_sequence])++state->debug_sequence;
    else state->debug_sequence=key==FILM_DEBUG_A?(state->debug_sequence==2?2:1):0;
    if(state->debug_sequence!=8)return false;
    state->debug_sequence=0;state->debug_mode=!state->debug_mode;return true;
}
