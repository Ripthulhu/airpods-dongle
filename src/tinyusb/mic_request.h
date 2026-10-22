// SPDX-License-Identifier: GPL-3.0-only
#ifndef USB_MIC_REQUEST_H
#define USB_MIC_REQUEST_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    _Atomic uint32_t desired;
    uint32_t handled;  // main loop only
} usb_mic_request_t;

// The USB callbacks are the sole producer; the last open/close wins.
static inline void usb_mic_request_set(usb_mic_request_t *state, bool wanted) {
    uint32_t old = atomic_load_explicit(&state->desired, memory_order_relaxed);
    if ((old & 1u) == (uint32_t)wanted) return;
    uint32_t next = ((old + 2u) & ~1u) | (uint32_t)wanted;
    atomic_store_explicit(&state->desired, next, memory_order_release);
}

static inline bool usb_mic_request_take(usb_mic_request_t *state, bool *wanted) {
    uint32_t desired = atomic_load_explicit(&state->desired, memory_order_acquire);
    if (desired == state->handled) return false;
    state->handled = desired;
    *wanted = (desired & 1u) != 0;
    return true;
}

static inline bool usb_mic_request_wanted(const usb_mic_request_t *state) {
    return (atomic_load_explicit(&state->desired, memory_order_acquire) & 1u) != 0;
}

#endif
