// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "../src/btstack/volume_trace.h"
#include "../src/tinyusb/debug_cdc.h"

static volume_trace_t history;
static uint32_t now_us, terminal_generation;
static unsigned starts, freezes, lines;
static uint8_t request;
static bool terminal = true, pending_output;
static char output[2048];
static size_t used;
uint8_t debug_cdc_take_volume_trace_request(void) {
    uint8_t result = request; request = 0; return result;
}
uint32_t debug_cdc_terminal_generation(void) { return terminal_generation; }
bool debug_cdc_terminal_connected(void) { return terminal; }
bool debug_cdc_output_pending(void) { return pending_output; }
static uint32_t bt_volume_trace_control(bool arm) {
    if (arm) { ++starts; volume_trace_arm(&history, now_us); return 0; }
    ++freezes; return volume_trace_freeze(&history);
}
static bool bt_volume_trace_take(uint32_t out[4]) { return volume_trace_take(&history, out); }
static int reply(const char *format, ...) {
    assert(!pending_output);
    va_list args; va_start(args, format);
    int length = vsnprintf(output + used, sizeof(output) - used, format, args);
    va_end(args);
    assert(length > 0 && length < 64 && (size_t)length < sizeof(output) - used);
    used += (size_t)length; ++lines; pending_output = true;
    return length;
}
#define printf reply
#include "main_volume_trace.inc"
#undef printf

static void poll(volume_trace_console_t *state, uint32_t ms) {
    unsigned before = lines;
    now_us = ms * 1000u;
    volume_trace_console_poll(state, ms);
    assert(lines - before <= 1);
}
static void drain(volume_trace_console_t *state, uint32_t ms) {
    for (unsigned i = 0; i < 20 && state->mode != VOLUME_CONSOLE_OFF; ++i) {
        pending_output = false; poll(state, ms + i);
    }
    assert(state->mode == VOLUME_CONSOLE_OFF);
}

int main(void) {
    volume_trace_console_t state = {0};
    for (unsigned i = 0; i < 1000; ++i) poll(&state, i);
    assert(!starts && !freezes && !lines);
    pending_output = true; request = DEBUG_CDC_VOLUME_TRACE_ARM; poll(&state, 1000);
    assert(starts == 1 && history.armed && !lines);
    volume_trace_record(&history, now_us + 1u, 0x12, 0x34, 0x56);
    pending_output = false; poll(&state, 1001);
    assert(lines == 1 && state.mode == VOLUME_CONSOLE_ARMED);
    request = DEBUG_CDC_VOLUME_TRACE_DUMP; poll(&state, 1002);
    assert(!history.armed && lines == 1); // A pending CDC reply delays even the header.
    drain(&state, 1003);
    assert(strstr(output, "[VOLTRACE] v=1 total=1\n") != NULL);
    assert(strstr(output, "000f4241 00000012 00000034 00000056\n") != NULL);
    assert(strstr(output, "[VOLTRACE] end\n") != NULL);

    pending_output = false; request = DEBUG_CDC_VOLUME_TRACE_ARM; poll(&state, UINT32_MAX - 9u);
    assert(history.armed);
    poll(&state, state.started_ms + 59999u);assert(history.armed);
    poll(&state, state.started_ms + 60000u);assert(!history.armed && state.mode == VOLUME_CONSOLE_OFF);
    pending_output = false; request = DEBUG_CDC_VOLUME_TRACE_ARM; poll(&state, 5000);
    volume_trace_record(&history, now_us + 1u, 1, 2, 3);
    ++terminal_generation; poll(&state, 5001);
    assert(!history.armed && state.mode == VOLUME_CONSOLE_OFF);
    request = DEBUG_CDC_VOLUME_TRACE_DUMP; terminal = false; poll(&state, 5002);
    assert(!history.armed && state.mode == VOLUME_CONSOLE_OFF);
    terminal = true; pending_output = false; request = DEBUG_CDC_VOLUME_TRACE_ARM; poll(&state, 6000);
    assert(history.total == 0 && history.armed);
    volume_trace_record(&history, now_us + 1u, 7, 8, 9);
    request = DEBUG_CDC_VOLUME_TRACE_DUMP; pending_output = false; poll(&state, 6001);
    unsigned before = lines; terminal = false; poll(&state, 6002);
    assert(lines == before && state.mode == VOLUME_CONSOLE_OFF);
    terminal = true; pending_output = false; request = DEBUG_CDC_VOLUME_TRACE_ARM; poll(&state, 7000);
    for (unsigned i = 0; i < 21; ++i) volume_trace_record(&history, now_us + i, i, 2, 3);
    request = DEBUG_CDC_VOLUME_TRACE_DUMP; pending_output = false; poll(&state, 7001);
    before = lines; poll(&state, 7002);
    assert(lines == before && history.remaining == 16); // Backpressure cannot advance the dump cursor.
    drain(&state, 7003);
    assert(lines == before + 17 && strstr(output, "[VOLTRACE] v=1 total=21\n") != NULL);
    puts("volume trace console: explicit arm/dump, one short row per drained pass, lease, reset and DTR cancellation passed");
}
