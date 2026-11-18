#include <assert.h>
#include <stdio.h>
#include "../probes/exposure/ipc_decode.h"
int main(void)
{
    uint32_t value=99;
    assert(probe_reply_value(0x001a0080,0x1a,0xa5a50000,8,&value) && value==0);
    assert(probe_reply_value(0x001a0080,0x1a,0xa5a50001,8,&value) && value==1);
    assert(probe_reply_value(0x002f0080,0x2f,0xbeef2280,16,&value) && value==0x2280);
    assert(!probe_reply_value(0x001a0040,0x1a,1,8,&value));
    assert(!probe_reply_value(0x001b0080,0x1a,1,8,&value));
    assert(!probe_reply_value(0x001a0080,0x1a,1,7,&value));
    puts("PASS raw IPC widths, missing response field and wrong command");
}
