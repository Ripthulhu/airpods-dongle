// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/tinyusb/debug_cdc.c"

static stdio_driver_t *registered;
static bool connected, interrupts_disabled;
static uint32_t available, accept_limit, write_calls, flush_calls;
static unsigned connected_queries;
static char output[20000];
static uint32_t output_size;
static uint8_t input[256];
static uint32_t input_size, input_offset, read_calls, read_flush_calls;

void stdio_set_driver_enabled(stdio_driver_t *driver, bool enabled) {
    assert(enabled);
    registered = driver;
}
uint32_t save_and_disable_interrupts(void) {
    uint32_t previous = interrupts_disabled;
    interrupts_disabled = true;
    return previous;
}
void restore_interrupts(uint32_t previous) { interrupts_disabled = previous; }
bool tud_cdc_connected(void) { connected_queries++; return connected; }
uint32_t tud_cdc_read(void *buffer, uint32_t count) {
    assert(connected && !interrupts_disabled && count <= 64u);
    ++read_calls;
    if (count > input_size - input_offset) count = input_size - input_offset;
    memcpy(buffer, input + input_offset, count);
    input_offset += count;
    return count;
}
void tud_cdc_read_flush(void) {
    assert(!connected && !interrupts_disabled);
    ++read_flush_calls;
    input_offset = input_size;
}
uint32_t tud_cdc_write_available(void) { return available; }
uint32_t tud_cdc_write(const void *buffer, uint32_t count) {
    assert(!interrupts_disabled && count <= available && count <= 64);
    write_calls++;
    if (count > accept_limit) count = accept_limit;
    assert(output_size + count < sizeof(output));
    memcpy(output + output_size, buffer, count);
    output_size += count;
    output[output_size] = 0;
    return count;
}
uint32_t tud_cdc_write_flush(void) { flush_calls++; return 0; }

static void reset(void) {
    dbg_head = dbg_tail = dbg_dropped = 0;
    connected = interrupts_disabled = false;
    available = accept_limit = UINT32_MAX;
    write_calls = flush_calls = output_size = 0;
    connected_queries = 0;
    input_size = input_offset = read_calls = read_flush_calls = 0;
    atomic_store(&timing_requested, false);
    atomic_store(&bootsel_requested, false);
    atomic_store(&terminal_connected, false);
    atomic_store(&terminal_generation, 0);
    command_length = 0;
    command_invalid = false;
    memset(output, 0, sizeof(output));
}

static void drain(void) {
    unsigned ticks = 0;
    while (dbg_head != dbg_tail || dbg_dropped != 0) {
        unsigned before = write_calls;
        debug_cdc_task();
        assert(write_calls - before <= 1);
        assert(++ticks < 1000);
    }
}

static void receive(const char *text) {
    input_size = (uint32_t)strlen(text);
    input_offset = 0;
    memcpy(input, text, input_size);
    while (input_offset < input_size) debug_cdc_task();
}

int main(void) {
    debug_cdc_init();
    assert(registered != NULL && registered->out_chars != NULL);
    reset();
    for (unsigned i = 0; i < 2000; ++i) debug_cdc_task();
    assert(connected_queries == 2000 && write_calls == 0 && read_calls == 0);
    registered->out_chars("boot\n", 5);
    debug_cdc_task();
    assert(write_calls == 0 && output_size == 0);
    connected = true;
    available = 0;
    debug_cdc_task();
    assert(write_calls == 0);
    available = 64;
    accept_limit = 0;
    debug_cdc_task();
    assert(output_size == 0 && flush_calls == 0);
    accept_limit = 2;
    debug_cdc_task();
    assert(output_size == 2 && memcmp(output, "bo", 2) == 0 && flush_calls == 1);
    accept_limit = 64;
    drain();
    assert(strcmp(output, "boot\r\n") == 0);
    unsigned queries = connected_queries;
    for (unsigned i = 0; i < 2000; ++i) debug_cdc_task();
    assert(connected_queries == queries + 2000u);

    // Input is serviced with an empty output ring and cannot run old commands.
    reset();
    connected = true;
    memcpy(input, "pdlr\r\nT\n", 8);
    input_size = 8;
    debug_cdc_task();
    assert(input_offset == 8u && !debug_cdc_take_timing_request());
    assert(output_size == 0u && write_calls == 0u);
    memset(input, 't', 128);
    input_offset = 0;
    input_size = 128;
    read_calls = 0;
    debug_cdc_task();
    assert(input_offset == 64u && read_calls == 1u);
    assert(debug_cdc_take_timing_request() && !debug_cdc_take_timing_request());
    debug_cdc_task();
    assert(input_offset == 128u && read_calls == 2u);
    assert(debug_cdc_take_timing_request() && !debug_cdc_take_timing_request());

    // A closed terminal discards both a pending request and unread old input.
    input_offset = 0;
    input_size = 128;
    debug_cdc_task();
    connected = false;
    debug_cdc_task();
    assert(!debug_cdc_take_timing_request());
    assert(input_offset == input_size && read_flush_calls == 1u);
    input_offset = 0;
    input_size = 1;
    debug_cdc_task();
    connected = true;
    debug_cdc_task();
    assert(!debug_cdc_take_timing_request());
    assert(output_size == 0u && write_calls == 0u);

    // Only the complete lowercase command line publishes a reboot request.
    // Every fragmentation point, including immediately before/after 't', works.
    reset();
    connected = true;
    const char command[] = "bootsel\n";
    for (unsigned split = 1; split < sizeof(command) - 1u; ++split) {
        char fragment[sizeof(command)];
        memcpy(fragment, command, split); fragment[split] = 0;
        receive(fragment);
        assert(!debug_cdc_take_bootsel_request() && !debug_cdc_take_timing_request());
        receive(command + split);
        assert(debug_cdc_take_bootsel_request() && !debug_cdc_take_bootsel_request());
        assert(!debug_cdc_take_timing_request());
    }
    receive("bootsel\r");
    assert(!debug_cdc_take_bootsel_request() && !debug_cdc_take_timing_request());
    receive("\n");
    assert(debug_cdc_take_bootsel_request() && !debug_cdc_take_timing_request());
    receive("Bootsel\nxbootsel\nbootselx\nboot\nsel\nbootselt\nbootsel\rx\n");
    assert(!debug_cdc_take_bootsel_request() && !debug_cdc_take_timing_request());
    memset(input, 'x', sizeof(input));
    input_offset = 0; input_size = sizeof(input);
    while (input_offset < input_size) debug_cdc_task();
    receive("bootsel\n"); // overlong/unknown line stays invalid until its terminator
    assert(!debug_cdc_take_bootsel_request());
    receive("t");
    assert(debug_cdc_take_timing_request());
    receive("bootsel\n");
    assert(!debug_cdc_take_bootsel_request()); // embedded after bare t is not a command line
    receive("boo");
    uint32_t generation = debug_cdc_terminal_generation();
    connected = false; debug_cdc_task();
    assert(!debug_cdc_terminal_connected());
    assert(debug_cdc_terminal_generation() == generation + 1u);
    debug_cdc_task();
    assert(debug_cdc_terminal_generation() == generation + 1u);
    connected = true; receive("tsel\n");
    assert(!debug_cdc_take_bootsel_request() && debug_cdc_take_timing_request());
    receive("bootsel\n");
    connected = false; debug_cdc_task();
    assert(!debug_cdc_take_bootsel_request());

    // A close/open pair dispatched inside one TinyUSB pass still revokes intent,
    // including command bytes which were buffered before the close.
    connected = true; debug_cdc_task();
    receive("bootsel\n");
    generation = debug_cdc_terminal_generation();
    connected = false; tud_cdc_line_state_cb(0, false, false);
    connected = true; tud_cdc_line_state_cb(0, true, false);
    debug_cdc_task();
    assert(!debug_cdc_take_bootsel_request());
    assert(debug_cdc_terminal_generation() == generation + 1u);
    memcpy(input, "bootsel\n", 8); input_size = 8; input_offset = 0;
    connected = false; tud_cdc_line_state_cb(0, false, false);
    connected = true; tud_cdc_line_state_cb(0, true, false);
    debug_cdc_task();
    assert(input_offset == input_size && !debug_cdc_take_bootsel_request());

    reset();
    connected = true;
    dbg_head = dbg_tail = DBG_BUF_SIZE - 3;
    registered->out_chars("one\ntwo", 7);
    assert(debug_cdc_output_pending());
    drain();
    assert(!debug_cdc_output_pending());
    assert(strcmp(output, "one\r\ntwo") == 0 && write_calls == 2);

    reset();
    connected = true;
    char log_input[DBG_BUF_SIZE + 3];
    memset(log_input, 'x', sizeof(log_input));
    registered->out_chars(log_input, sizeof(log_input));
    assert(dbg_dropped == 4);
    drain();
    assert(output_size == DBG_BUF_SIZE - 1 + strlen("\r\n[log dropped 4 bytes]\r\n"));
    for (unsigned i = 0; i < DBG_BUF_SIZE - 1; ++i) assert(output[i] == 'x');
    assert(strcmp(output + DBG_BUF_SIZE - 1, "\r\n[log dropped 4 bytes]\r\n") == 0);
    assert(!interrupts_disabled);
    puts("CDC bounded RX/TX, fragmented BOOTSEL commands, timing isolation, DTR cleanup, partial writes, wrap and overflow: passed");
    return 0;
}
