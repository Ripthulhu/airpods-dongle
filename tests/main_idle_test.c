// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/realtime_metrics.h"

typedef struct { bool start_pending, releasing; } mic_decoder_lifecycle_t;
enum { OUTPUT, USB_PLAY, USB_MIC, PHONE, PHONE_START, CALL, MIC_REQUEST, MIC_ACTIVE, TRANSITION, CDC, BUSY_COUNT };
static bool busy[BUSY_COUNT];
static bool check_is_streaming(void) { return busy[OUTPUT]; }
static bool get_usb_streaming(void) { return busy[USB_PLAY]; }
static bool usb_mic_requested(void) { return busy[USB_MIC]; }
static bool phone_sink_active(void) { return busy[PHONE]; }
static bool phone_sink_start_pending(void) { return busy[PHONE_START]; }
static bool phone_call_active(void) { return busy[CALL]; }
static bool aacp_mic_requested(void) { return busy[MIC_REQUEST]; }
static bool aacp_mic_active(void) { return busy[MIC_ACTIVE]; }
static bool dongle_playback_transitioning(void) { return busy[TRANSITION]; }
static bool debug_cdc_terminal_connected(void) { return busy[CDC]; }
#include "main_idle_policy.inc"

int main(void) {
    mic_decoder_lifecycle_t decoder={0};
    assert(main_fully_idle(decoder));
    for(unsigned i=0;i<BUSY_COUNT;++i) {
        busy[i]=true;
        assert(!main_fully_idle(decoder));
        assert(realtime_loop_idle_us(0,100,main_fully_idle(decoder))==900);
        busy[i]=false;
    }
    decoder.start_pending=true;assert(!main_fully_idle(decoder));
    decoder.start_pending=false;decoder.releasing=true;assert(!main_fully_idle(decoder));
    decoder.releasing=false;
    bool snapshot=main_fully_idle(decoder);
    busy[USB_PLAY]=true; // USB IRQ can start buffering just after the idle snapshot.
    assert(realtime_loop_idle_us(0,100,snapshot)==4900); // First preparation waits at most 5 ms.
    assert(!main_fully_idle(decoder));
    assert(realtime_loop_idle_us(5000,5100,main_fully_idle(decoder))==900);
    assert(realtime_loop_idle_us(5000,7000,main_fully_idle(decoder))==0);
    puts("main idle: every audio/transition/decoder/console owner keeps fast pacing; first wake bounded then fast");
}
