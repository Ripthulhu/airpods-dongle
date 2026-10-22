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

int main(void) {
    debug_cdc_init();
    assert(registered != NULL && registered->out_chars != NULL);
    reset();
    for (unsigned i = 0; i < 2000; ++i) debug_cdc_task();
    assert(connected_queries == 0 && write_calls == 0);
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
    assert(connected_queries == queries);

    reset();
    connected = true;
    dbg_head = dbg_tail = DBG_BUF_SIZE - 3;
    registered->out_chars("one\ntwo", 7);
    drain();
    assert(strcmp(output, "one\r\ntwo") == 0 && write_calls == 2);

    reset();
    connected = true;
    char input[DBG_BUF_SIZE + 3];
    memset(input, 'x', sizeof(input));
    registered->out_chars(input, sizeof(input));
    assert(dbg_dropped == 4);
    drain();
    assert(output_size == DBG_BUF_SIZE - 1 + strlen("\r\n[log dropped 4 bytes]\r\n"));
    for (unsigned i = 0; i < DBG_BUF_SIZE - 1; ++i) assert(output[i] == 'x');
    assert(strcmp(output + DBG_BUF_SIZE - 1, "\r\n[log dropped 4 bytes]\r\n") == 0);
    assert(!interrupts_disabled);
    puts("CDC bounded direct drain, partial writes, wrap, offline retention and overflow: passed");
    return 0;
}
