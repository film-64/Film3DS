#include "roll.h"
#include "film_runtime_contract.h"

FilmStatus film_roll_decode_toggle(FilmRollDecode *batch,unsigned id)
{
    if(!batch||batch->decode_active||!id||id>FILM_ROLL_MAX_PHOTOS)return FILM_STATE;
    batch->decode_selection[id]=!batch->decode_selection[id];return FILM_OK;
}
FilmStatus film_roll_decode_begin(FilmRollDecode *batch,const uint16_t *ids,unsigned count,unsigned current)
{
    if(!film_runtime_source_valid("film-roll-decode/v1"))return FILM_STATE;
    if(!batch||batch->decode_active||!ids||!count||count>FILM_ROLL_MAX_PHOTOS||current>=count)return FILM_STATE;
    for(unsigned i=0;i<count;++i)if(!ids[i]||ids[i]>FILM_ROLL_MAX_PHOTOS||(i&&ids[i]<=ids[i-1]))return FILM_INVALID;
    batch->decode_count=batch->decode_position=0;
    for(unsigned i=0;i<count;++i)if(i!=current&&batch->decode_selection[ids[i]])batch->decode_ids[batch->decode_count++]=ids[i];
    if(batch->decode_selection[ids[current]]||!batch->decode_count)batch->decode_ids[batch->decode_count++]=ids[current];
    batch->decode_active=true;return FILM_OK;
}
void film_roll_decode_cancel(FilmRollDecode *batch)
{if(batch){batch->decode_active=false;batch->decode_count=batch->decode_position=0;}}
void film_roll_decode_advance(FilmRollDecode *batch,bool success)
{
    if(!batch||!batch->decode_active)return;
    if(!success){film_roll_decode_cancel(batch);return;}
    batch->decode_selection[batch->decode_ids[batch->decode_position]]=false;
    if(++batch->decode_position==batch->decode_count)film_roll_decode_cancel(batch);
}
