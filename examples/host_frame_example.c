/* SPDX-License-Identifier: MIT
 * Wire-3 byte-layout example, not a transport/scheduler implementation. */
#include <stdio.h>
#include "../include/ultra_protocol.h"
int main(void)
{
    uint8_t frame[2u*ULTRA_SLOT_BYTES+ULTRA_HDR_BYTES];
    const uint16_t a[5]={65535,0,0,0,0},b[5]={65535,65535,65535,65535,65535};
    const uint8_t id=ULTRA_CFG_PX_SOURCE;
    const uint16_t source=ULTRA_SOURCE_INGRESS(2);
    unsigned i;
    /* Position 0, light 19; position 1, output 1. Validate capabilities first. */
    ultra_slot_set(frame,19,0x42,ULTRA_SET_SMOOTH,32768,a,b,1000,0);
    ultra_slot_config(frame+ULTRA_SLOT_BYTES,1,0x43,&id,&source,1);
    ultra_build_header(frame+2u*ULTRA_SLOT_BYTES,0x00abcdef);
    for(i=0;i<sizeof frame;++i)printf("%02x%s",frame[i],(i+1u)%32u?" ":"\n");
    printf("\n16-slot return window at 250 kbaud: %u us\n",(unsigned)ultra_rw_window_us(16,2));
    return ultra_slot_check(frame)||ultra_slot_check(frame+ULTRA_SLOT_BYTES);
}
