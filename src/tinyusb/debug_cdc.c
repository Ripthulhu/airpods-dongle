// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
//
// Debug logging over USB CDC — see debug_cdc.h.
//
// printf() may be called from both thread context (BTstack run loop on Core 0)
// and IRQ context (the 500us USB timer that drives tud_task and its callbacks).
// To keep the TinyUSB CDC FIFO single-writer, all printf output first lands in
// an interrupt-safe ring buffer (producers briefly disable IRQs), and only the
// USB task/callback context ever touches tud_cdc_*.
//

#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>

#include "pico/stdio/driver.h"
#include "hardware/sync.h"
#include "tusb.h"

#include "debug_cdc.h"

// Diagnostic history is expendable; reserve scarce SRAM for live duplex audio.
#define DBG_BUF_SIZE 2048u                 // must be a power of two
#define DBG_BUF_MASK (DBG_BUF_SIZE - 1u)

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "Diagnostic requests require lock-free IRQ publication");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "CDC epochs require lock-free IRQ publication");
#endif

static uint8_t           dbg_buf[DBG_BUF_SIZE];
static volatile uint32_t dbg_head = 0;     // producer index
static volatile uint32_t dbg_tail = 0;     // consumer index (USB task only)
static volatile uint32_t dbg_dropped = 0;  // bytes discarded while the ring was full
static atomic_bool timing_requested, work_requested;
static atomic_bool bootsel_requested, terminal_connected;
static atomic_uint terminal_generation, volume_trace_requested;
static uint8_t command_length;
static bool command_invalid;

// Keep the inherited bare 't' request, but never interpret the 't' inside a
// fragmented BOOTSEL command as a diagnostics request. Other lines are ignored.
static void command_byte(uint8_t c) {
    static const char command[] = "bootsel";
    if (c == '\n') {
        if (!command_invalid && (command_length == sizeof(command) - 1u ||
                                 command_length == sizeof(command)))
            atomic_store_explicit(&bootsel_requested, true, memory_order_release);
        command_length = 0;
        command_invalid = false;
    } else if (!command_invalid) {
        if (command_length == sizeof(command) - 1u && c == '\r') {
            ++command_length;
        } else if ((command_length == 0 || command_length == UINT8_MAX) &&
                   (c == 't' || c == 'w' || c == 'v' || c == 'V')) {
            if (c == 't') atomic_store_explicit(&timing_requested, true, memory_order_release);
            else if (c == 'w') atomic_store_explicit(&work_requested, true, memory_order_release);
            else atomic_store_explicit(&volume_trace_requested,
                c == 'v' ? DEBUG_CDC_VOLUME_TRACE_ARM : DEBUG_CDC_VOLUME_TRACE_DUMP, memory_order_release);
            command_length = UINT8_MAX; // bare diagnostics remain repeatable without LF
        } else if (command_length < sizeof(command) - 1u && c == command[command_length]) {
            ++command_length;
        } else {
            command_invalid = true;
        }
    }
}

// Producer side. Runs with IRQs disabled so thread- and IRQ-context printf
// calls can't interleave, and so the USB-task consumer can't preempt a push.
static void dbg_push_locked(uint8_t c) {
    uint32_t next = (dbg_head + 1u) & DBG_BUF_MASK;
    if (next == dbg_tail) {                // full: drop (debug output is best-effort)
        dbg_dropped++;                     // ...but make the gap self-evident later
        return;
    }
    dbg_buf[dbg_head] = c;
    dbg_head = next;
}

static void cdc_out_chars(const char *buf, int len) {
    uint32_t save = save_and_disable_interrupts();
    for (int i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n') dbg_push_locked('\r');  // make output terminal-friendly
        dbg_push_locked((uint8_t) c);
    }
    restore_interrupts(save);
}

static stdio_driver_t debug_cdc_driver = {
    .out_chars = cdc_out_chars,
    .out_flush = NULL,
    .in_chars  = NULL,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = false,   // we insert CR ourselves above
#endif
};

void debug_cdc_init(void) {
    atomic_store_explicit(&timing_requested, false, memory_order_relaxed);
    atomic_store_explicit(&work_requested, false, memory_order_relaxed);
    atomic_store_explicit(&volume_trace_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&bootsel_requested, false, memory_order_relaxed);
    atomic_store_explicit(&terminal_connected, false, memory_order_relaxed);
    atomic_store_explicit(&terminal_generation, 0, memory_order_relaxed);
    command_length = 0;
    command_invalid = false;
    stdio_set_driver_enabled(&debug_cdc_driver, true);
}

bool debug_cdc_take_timing_request(void) {
    return atomic_exchange_explicit(&timing_requested, false, memory_order_acq_rel);
}

bool debug_cdc_take_work_request(void) {
    return atomic_exchange_explicit(&work_requested, false, memory_order_acq_rel);
}

uint8_t debug_cdc_take_volume_trace_request(void) {
    return (uint8_t)atomic_exchange_explicit(&volume_trace_requested, 0, memory_order_acq_rel);
}

bool debug_cdc_take_bootsel_request(void) {
    return atomic_exchange_explicit(&bootsel_requested, false, memory_order_acq_rel);
}

bool debug_cdc_terminal_connected(void) {
    return atomic_load_explicit(&terminal_connected, memory_order_acquire);
}

uint32_t debug_cdc_terminal_generation(void) {
    return atomic_load_explicit(&terminal_generation, memory_order_acquire);
}

bool debug_cdc_output_pending(void) {
    uint32_t save = save_and_disable_interrupts();
    bool pending = dbg_head != dbg_tail || dbg_dropped != 0;
    restore_interrupts(save);
    return pending;
}

static void terminal_closed(void) {
    if (atomic_exchange_explicit(&terminal_connected, false, memory_order_acq_rel))
        atomic_fetch_add_explicit(&terminal_generation, 1, memory_order_release);
    atomic_store_explicit(&timing_requested, false, memory_order_release);
    atomic_store_explicit(&work_requested, false, memory_order_release);
    atomic_store_explicit(&volume_trace_requested, 0, memory_order_release);
    atomic_store_explicit(&bootsel_requested, false, memory_order_release);
    command_length = 0;
    command_invalid = false;
    tud_cdc_read_flush();
}

// TinyUSB can dispatch a DTR close and reopen within one tud_task() pass.
// Record the close edge rather than relying only on the later connected poll.
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)rts;
    if (itf == 0 && !dtr) terminal_closed();
}

void debug_cdc_task(void) {
    // Drain only while a terminal is actually attached (DTR asserted). Until
    // then, output accumulates in the 2KB ring, so recent boot/pairing logs are still
    // delivered when the user opens the terminal late.
    if (!tud_cdc_connected()) {
        terminal_closed();
        return;
    }
    atomic_store_explicit(&terminal_connected, true, memory_order_release);

    // Publish requests only; main context checks idle state before rebooting.
    // Other bytes never reach the inherited pairing/storage console commands.
    uint8_t input[64];
    uint32_t received = tud_cdc_read(input, sizeof(input));
    for (uint32_t i = 0; i < received; ++i) command_byte(input[i]);
    if (dbg_tail == dbg_head && dbg_dropped == 0) return;

    // If bytes were discarded while the ring was full, report the gap once the
    // ring has room again, so a log gap is never silent. (We run in the USB
    // timer IRQ; producers push with IRQs disabled, so guard the same way.)
    if (dbg_dropped != 0 && ((dbg_head - dbg_tail) & DBG_BUF_MASK) < DBG_BUF_SIZE / 2u) {
        char msg[48];
        int n = snprintf(msg, sizeof(msg), "\r\n[log dropped %lu bytes]\r\n",
                         (unsigned long) dbg_dropped);
        uint32_t save = save_and_disable_interrupts();
        dbg_dropped = 0;
        for (int i = 0; i < n; i++) dbg_push_locked((uint8_t) msg[i]);
        restore_interrupts(save);
    }

    if (dbg_tail == dbg_head) return;

    // Bounded work per call: at most ONE 64-byte chunk per tick. This runs in
    // the 500us USB timer IRQ; an unbounded drain loop under heavy printf
    // traffic could hog the IRQ.
    uint32_t avail = tud_cdc_write_available();
    if (avail == 0) return;

    uint32_t tail = dbg_tail;
    uint32_t n = (dbg_head - tail) & DBG_BUF_MASK;
    if (n > avail) n = avail;
    if (n > 64u) n = 64u;
    if (n > DBG_BUF_SIZE - tail) n = DBG_BUF_SIZE - tail;
    // Keep this span reserved until TinyUSB has copied the bytes it accepted.
    uint32_t sent = tud_cdc_write(dbg_buf + tail, n);
    dbg_tail = (tail + sent) & DBG_BUF_MASK;
    if (sent != 0) tud_cdc_write_flush();
}
