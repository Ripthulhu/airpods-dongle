// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool usb, usb_mic, phone, phone_start_pending, call, mic_wanted, mic_active, transition;
static bool terminal = true, requested, output_pending, irq_disabled;
static unsigned ready_replies, busy_replies, resets, guard_queries;
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
    if (strcmp(text, "[BOOTSEL] ready\n") == 0) ++ready_replies;
    else { assert(strcmp(text, "[BOOTSEL] busy\n") == 0); ++busy_replies; }
    return 0;
}
static void reset_usb_boot(unsigned mask, unsigned interfaces) {
    assert(irq_disabled && mask == 0 && interfaces == 0);
    assert(!usb && !usb_mic && !phone && !phone_start_pending && !call && !mic_wanted && !mic_active && !transition);
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
        *owners[i] = true; requested = true;
        assert(!bootsel_request_poll(&state, 1000) && !state.pending);
        *owners[i] = false;
    }
    assert(busy_replies == 8 && !ready_replies);
    requested = true; output_pending = true;
    assert(!bootsel_request_poll(&state, 2000) && state.pending);
    assert(ready_replies == 1);
    assert(!bootsel_request_poll(&state, 2019));
    assert(!bootsel_request_poll(&state, 2099)); // bounded drain, no main-loop sleep
    assert(bootsel_request_poll(&state, 2100) && !state.pending);
    // An IRQ can start capture after the first guard: the final IRQ-off check wins.
    usb_mic = true; bootsel_enter_if_idle(state.terminal_generation); usb_mic = false;
    assert(resets == 0 && busy_replies == 9 && !irq_disabled);
    requested = true;
    assert(!bootsel_request_poll(&state, UINT32_MAX - 9u));
    output_pending = false;
    assert(bootsel_request_poll(&state, 10u)); // wrap-safe 20ms acceptance drain
    bootsel_enter_if_idle(state.terminal_generation);
    assert(resets == 1 && !irq_disabled);
    requested = true;
    assert(!bootsel_request_poll(&state, 3000));
    call = true;
    assert(!bootsel_request_poll(&state, 3020) && !state.pending);
    call = false;
    requested = true;
    assert(!bootsel_request_poll(&state, 4000));
    terminal = false;
    assert(!bootsel_request_poll(&state, 4020) && !state.pending);
    bootsel_enter_if_idle(state.terminal_generation);
    assert(resets == 1 && !irq_disabled);
    terminal = true;
    assert(!bootsel_request_poll(&state, 5000)); // disconnect cannot leave latent intent
    requested = true;
    assert(!bootsel_request_poll(&state, 6000));
    ++terminal_generation; // DTR closed/reopened entirely between main-loop polls
    assert(!bootsel_request_poll(&state, 6020) && !state.pending);
    requested = true;
    assert(!bootsel_request_poll(&state, 7000));
    assert(bootsel_request_poll(&state, 7020));
    ++terminal_generation; // same race after deferred acceptance, before ROM call
    bootsel_enter_if_idle(state.terminal_generation);
    assert(resets == 1 && !irq_disabled);
    puts("BOOTSEL: deferred bounded drain, every audio owner, final IRQ-safe guard, DTR cancellation and wrap passed");
    return 0;
}
