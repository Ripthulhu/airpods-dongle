// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool usb, usb_mic, phone, phone_start_pending, call, mic_wanted, mic_active, transition;
static bool terminal = true, requested, output_pending, irq_disabled;
static unsigned ready_replies, resets, guard_queries, reset_owners;
static uint32_t terminal_generation;
static bool get_usb_streaming(void) { ++guard_queries; return usb; }
static bool usb_mic_requested(void) { return usb_mic; }
static bool phone_sink_active(void) { return phone; }
static bool phone_sink_start_pending(void) { return phone_start_pending; }
static bool phone_call_active(void) { return call; }
static bool aacp_mic_requested(void) { return mic_wanted; }
static bool aacp_mic_active(void) { return mic_active; }
static bool dongle_playback_transitioning(void) { return transition; }
static bool debug_cdc_take_bootsel_request(void) {
    bool result = requested; requested = false; return result;
}
static bool debug_cdc_terminal_connected(void) { return terminal; }
static uint32_t debug_cdc_terminal_generation(void) { return terminal_generation; }
static bool debug_cdc_output_pending(void) { return output_pending; }
static unsigned save_and_disable_interrupts(void) {
    unsigned prior = irq_disabled; irq_disabled = true; return prior;
}
static void restore_interrupts(unsigned prior) { irq_disabled = prior; }
static int reply(const char *text) {
    assert(!irq_disabled);
    assert(strcmp(text, "[BOOTSEL] ready\n") == 0);
    ++ready_replies;
    return 0;
}
static void reset_usb_boot(unsigned mask, unsigned interfaces) {
    assert(irq_disabled && mask == 0 && interfaces == 0);
    assert(terminal);
    reset_owners=(unsigned)usb | (unsigned)usb_mic<<1 | (unsigned)phone<<2 |
        (unsigned)phone_start_pending<<3 | (unsigned)call<<4 | (unsigned)mic_wanted<<5 |
        (unsigned)mic_active<<6 | (unsigned)transition<<7;
    ++resets;
    // Production SDK is noreturn; this stub returns to check IRQ restoration.
}
#define printf reply
#include "main_bootsel.inc"
#undef printf

int main(void) {
    bootsel_request_t state = {0};
    for (unsigned i = 0; i < 1000; ++i) assert(!bootsel_request_poll(&state, i));
    assert(guard_queries == 0 && !ready_replies && !resets);
    bool *owners[] = {&usb, &usb_mic, &phone, &phone_start_pending, &call, &mic_wanted, &mic_active, &transition};
    for (unsigned i = 0; i < sizeof(owners) / sizeof(owners[0]); ++i) {
        uint32_t start=1000u+i*200u;
        *owners[i] = true; requested = true;
        assert(bootsel_busy()); // Audio still keeps the normal main loop at its active cadence.
        unsigned queried=guard_queries;
        assert(!bootsel_request_poll(&state, start) && state.pending);
        assert(!bootsel_request_poll(&state, start+19u));
        assert(bootsel_request_poll(&state, start+20u) && !state.pending);
        bootsel_enter_if_current(state.terminal_generation);
        assert(resets==i+1u && reset_owners==(1u<<i) && !irq_disabled);
        assert(guard_queries==queried); // An explicit update never waits for an audio owner to close.
        *owners[i] = false;
    }
    assert(ready_replies==8 && !bootsel_busy());
    for(unsigned i=0;i<sizeof(owners)/sizeof(*owners);++i)*owners[i]=true;
    unsigned queried=guard_queries;
    for(unsigned i=3000;i<4000;++i)assert(!bootsel_request_poll(&state,i));
    assert(resets==8 && guard_queries==queried); // Active audio alone cannot request a reboot.
    requested = true; output_pending = true;
    assert(!bootsel_request_poll(&state, 4000) && state.pending);
    requested=true;assert(!bootsel_request_poll(&state,4010)); // Duplicate cannot extend the drain.
    assert(state.requested_ms==4000 && ready_replies==9);
    assert(!bootsel_request_poll(&state, 4019));
    assert(!bootsel_request_poll(&state, 4099));
    assert(bootsel_request_poll(&state, 4100) && !state.pending);
    bootsel_enter_if_current(state.terminal_generation);
    assert(resets==9 && reset_owners==255 && !irq_disabled);
    for(unsigned i=0;i<sizeof(owners)/sizeof(*owners);++i)*owners[i]=false;
    // Host packets and a newly opened capture cannot veto an accepted update.
    requested=true;assert(!bootsel_request_poll(&state,5000));
    usb=true;assert(!bootsel_request_poll(&state,5050));
    usb=false;assert(!bootsel_request_poll(&state,5099));
    usb=true;assert(bootsel_request_poll(&state,5100));
    usb_mic=call=transition=true;
    bootsel_enter_if_current(state.terminal_generation);
    assert(resets==10 && reset_owners==(1u|2u|16u|128u) && !irq_disabled);
    usb=usb_mic=call=transition=false;
    requested = true;
    assert(!bootsel_request_poll(&state, UINT32_MAX - 9u));
    output_pending = false;
    assert(bootsel_request_poll(&state, 10u)); // wrap-safe 20ms acceptance drain
    bootsel_enter_if_current(state.terminal_generation);
    assert(resets == 11 && reset_owners==0 && !irq_disabled);
    requested = true;
    assert(!bootsel_request_poll(&state, 6000));
    terminal = false;
    assert(!bootsel_request_poll(&state, 6020) && !state.pending);
    bootsel_enter_if_current(state.terminal_generation);
    assert(resets == 11 && !irq_disabled);
    terminal = true;
    assert(!bootsel_request_poll(&state, 7000)); // disconnect cannot leave latent intent
    requested = true;
    assert(!bootsel_request_poll(&state, 8000));
    ++terminal_generation; // DTR closed/reopened entirely between main-loop polls
    assert(!bootsel_request_poll(&state, 8020) && !state.pending);
    assert(!bootsel_request_poll(&state, 8040));
    requested = true;
    assert(!bootsel_request_poll(&state, 9000));
    assert(bootsel_request_poll(&state, 9020));
    terminal=false; // DTR can close after the deferred decision, before the ROM call.
    bootsel_enter_if_current(state.terminal_generation);
    assert(resets==11 && !irq_disabled);
    terminal=true;requested=true;
    assert(!bootsel_request_poll(&state,10000));
    assert(bootsel_request_poll(&state,10020));
    ++terminal_generation; // same race after deferred acceptance, before ROM call
    bootsel_enter_if_current(state.terminal_generation);
    assert(resets == 11 && !irq_disabled);
    assert(!bootsel_request_poll(&state,10040));
    puts("BOOTSEL: active playback/capture/call update, bounded drain, IRQ-safe DTR fencing and wrap passed");
    return 0;
}
