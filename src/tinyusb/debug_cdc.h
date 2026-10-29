// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
//
// Debug logging over USB CDC.
//
// Routes stdio (printf) into an interrupt-safe ring buffer, then drains it to
// the composite device's CDC-ACM IN endpoint from the USB task context. This
// lets firmware logs be read over the same USB cable as the audio device,
// with no UART adapter — useful when reporting issues.
//

#ifndef DEBUG_CDC_H
#define DEBUG_CDC_H

#include <stdbool.h>
#include <stdint.h>

// Register the CDC stdio driver so printf() output is captured. Call once,
// after stdio_init_all() and tinyusb_main().
void debug_cdc_init(void);

// Drain buffered stdout and read at most one 64-byte command input chunk.
// Call from the USB task
// (same context as tud_task), after tud_task().
void debug_cdc_task(void);

// Main context only: consumes a coalesced 't' request without printing in USB IRQ.
// USB service clears pending input and requests when the terminal drops DTR.
bool debug_cdc_take_timing_request(void);

enum { DEBUG_CDC_VOLUME_TRACE_ARM = 1, DEBUG_CDC_VOLUME_TRACE_DUMP = 2 };
// Bare v arms/reset a 60-second capture; V freezes/dumps. IRQ publishes only.
uint8_t debug_cdc_take_volume_trace_request(void);

// Exact lowercase "bootsel" followed by LF/CRLF; IRQ only publishes intent.
// Explicitly interrupts audio; main drains the reply then verifies the same DTR session.
bool debug_cdc_take_bootsel_request(void);
bool debug_cdc_terminal_connected(void);
uint32_t debug_cdc_terminal_generation(void);
bool debug_cdc_output_pending(void);

#endif // DEBUG_CDC_H
