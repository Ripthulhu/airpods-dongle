// SPDX-License-Identifier: GPL-3.0-only
#include "media_control.h"

#include <stdatomic.h>
#include <stddef.h>
#include "tusb.h"

#define MEDIA_QUEUE_LENGTH 8u

typedef struct {
    uint32_t generation;
    uint16_t usage;
} media_event_t;

static media_event_t queue[MEDIA_QUEUE_LENGTH];
static _Atomic uint32_t written;
static _Atomic uint32_t read;
static _Atomic uint32_t host_state;
static bool release_pending;
static uint16_t last_usage;

bool usb_media_enqueue(enum usb_media_command command) {
    uint16_t usage;
    switch (command) {
        case USB_MEDIA_PLAY: usage = 0x00b0; break;
        case USB_MEDIA_PAUSE: usage = 0x00b1; break;
        case USB_MEDIA_NEXT: usage = 0x00b5; break;
        case USB_MEDIA_PREVIOUS: usage = 0x00b6; break;
        case USB_MEDIA_STOP: usage = 0x00b7; break;
        default: return false;
    }
    uint32_t state = atomic_load_explicit(&host_state, memory_order_acquire);
    if (!(state & 1u)) return false;
    uint32_t w = atomic_load_explicit(&written, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&read, memory_order_acquire);
    if (w - r >= MEDIA_QUEUE_LENGTH) return false;
    queue[w % MEDIA_QUEUE_LENGTH] = (media_event_t){state, usage};
    atomic_store_explicit(&written, w + 1u, memory_order_release);
    return atomic_load_explicit(&host_state, memory_order_acquire) == state;
}

void usb_media_set_available(bool available) {
    uint32_t old = atomic_load_explicit(&host_state, memory_order_relaxed);
    uint32_t next = ((old + 2u) & ~1u) | (uint32_t)available;
    atomic_store_explicit(&host_state, next, memory_order_release);
    atomic_store_explicit(&read, atomic_load_explicit(&written, memory_order_acquire),
                          memory_order_release);
    last_usage = 0;
    // A suspended bus cannot transmit; release first when the host returns.
    release_pending = true;
}

static bool send_usage(uint16_t usage) {
    const uint8_t report[] = {(uint8_t)usage, (uint8_t)(usage >> 8)};
    if (!tud_hid_report(0, report, sizeof(report))) return false;
    last_usage = usage;
    return true;
}

void usb_media_task(void) {
    uint32_t state = atomic_load_explicit(&host_state, memory_order_acquire);
    if (!(state & 1u)) return;
    if (release_pending) {
        if (tud_hid_ready() && send_usage(0)) release_pending = false;
        return;
    }
    // Stale events can be published by a Bluetooth callback interrupted by reset.
    for (unsigned i = 0; i < MEDIA_QUEUE_LENGTH; ++i) {
        uint32_t r = atomic_load_explicit(&read, memory_order_relaxed);
        uint32_t w = atomic_load_explicit(&written, memory_order_acquire);
        if (r == w) return;
        media_event_t event = queue[r % MEDIA_QUEUE_LENGTH];
        if (event.generation != state) {
            atomic_store_explicit(&read, r + 1u, memory_order_release);
            continue;
        }
        if (tud_hid_ready() && send_usage(event.usage)) {
            release_pending = true;
            atomic_store_explicit(&read, r + 1u, memory_order_release);
        }
        return;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t requested) {
    if (instance != 0 || report_id != 0 || report_type != HID_REPORT_TYPE_INPUT ||
        buffer == NULL || requested < 2) return 0;
    buffer[0] = (uint8_t)last_usage;
    buffer[1] = (uint8_t)(last_usage >> 8);
    return 2;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t size) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)size;
}
