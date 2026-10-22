// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/phone_control_protocol.h"
static unsigned calls;
static bool accept=true;
static bool receive(void *ctx,const uint8_t *frame){
    assert(ctx==&calls);assert(frame[6]==2 || frame[6]==3);++calls;return accept;
}
int main(void){
    uint8_t read[8]={'A','P','D','G',1,1,2,0};
    uint8_t write[72]={'A','P','D','G',1,2,3,64,3,'A','P','D','G',1,64};
    for(unsigned split=0;split<=72;split++){
        phone_control_parser_t p={0};unsigned before=calls;
        assert(phone_control_feed(&p,write,split,receive,&calls));
        assert(phone_control_feed(&p,write+split,72-split,receive,&calls));
        assert(calls==before+1 && !p.used);
    }
    phone_control_parser_t p={0};
    for(unsigned i=0;i<72;i++)assert(phone_control_feed(&p,write+i,1,receive,&calls));
    uint8_t batch[88];memcpy(batch,read,8);memcpy(batch+8,write,72);memcpy(batch+80,read,8);
    unsigned before=calls;assert(phone_control_feed(&p,batch,88,receive,&calls));assert(calls==before+3);
    const unsigned offsets[]={0,4,5,6,7,8,9,13,14};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);i++){
        uint8_t bad[72];memcpy(bad,write,72);bad[offsets[i]]^=0x80;
        memset(&p,0,sizeof(p));assert(!phone_control_feed(&p,bad,72,receive,&calls));
    }
    memset(&p,0,sizeof(p));accept=false;assert(!phone_control_feed(&p,read,8,receive,&calls));
    assert(!phone_control_feed(NULL,read,8,receive,&calls));
    puts("phone framing: every split, coalesced frames, validation and backpressure passed");
}
