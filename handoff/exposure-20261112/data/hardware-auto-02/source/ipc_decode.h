/* Response widths are explicit; an absent/malformed field is never a value. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
static inline bool probe_reply_value(uint32_t header, uint16_t command, uint32_t word,
                              unsigned width, uint32_t *value)
{
    unsigned normal=(header>>6)&63;
    if ((header>>16)!=command || normal<2 || (width!=8 && width!=16 && width!=32)) return false;
    uint32_t mask=width==32?UINT32_MAX:((UINT32_C(1)<<width)-1);
    *value=word&mask;
    return true;
}
