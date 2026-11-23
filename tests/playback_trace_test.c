#include <assert.h>
#include <stdio.h>
#include "../src/btstack/playback_trace.h"
int main(void) {
    playback_trace_t trace={0}; uint8_t data[512];
    assert(playback_trace_pack(&trace,data,512,0,0,0,0,0)==512);
    assert(data[28]==0x80 && !data[29] && !data[30] && !data[31]);
    playback_trace_usb_recovery(&trace,true,true,false);
    playback_trace_usb_recovery(&trace,false,true,true);
    for(unsigned i=0;i<70;++i)playback_trace_record(&trace,i*100,TRACE_ZERO,i,70-i);
    trace.stats[TRACE_ACTIVE_ZERO]=70;
    assert(!playback_trace_pack(&trace,data,511,0,0,0,0,0));
    assert(playback_trace_pack(&trace,data,512,7000,960,480,3,1234)==512);
    assert(data[0]==1 && data[1]==16 && data[2]==24);
    assert(data[128+8]==46 && data[128+23*16+8]==69);
    assert(data[32+4*TRACE_ACTIVE_ZERO]==70);
    assert(data[28]==0x86 && data[29]==1 && data[30]==2 && data[31]==1);
    assert(trace.serial==70); // Reading never resets counters or consumes history.
    for(unsigned i=0;i<255;++i) playback_trace_usb_recovery(&trace,true,false,false);
    assert(playback_trace_pack(&trace,data,512,0,0,0,0,0)==512);
    assert(data[28]==0x81 && data[29]==0 && data[30]==2 && data[31]==1);
    puts("playback trace: bounded history, chronological snapshot and capacity passed");
}
