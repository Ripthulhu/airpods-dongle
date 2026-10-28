// SPDX-License-Identifier: GPL-3.0-only
#include "media_control.h"
#include "dongle_control.h"
#include "dongle_rpc.h"

#include <stdatomic.h>
#include <stddef.h>
#include "tusb.h"
#include "pico/time.h"

#define MEDIA_QUEUE_LENGTH 8u

typedef struct {
    uint32_t generation;
    uint32_t volume_epoch, deadline;
    uint16_t usage;
} media_event_t;

static media_event_t queue[MEDIA_QUEUE_LENGTH];
static _Atomic uint32_t written;
static _Atomic uint32_t read;
static _Atomic uint32_t host_state;
static _Atomic uint32_t volume_epoch;
static _Atomic uint32_t volume_sent;
static bool release_pending;
static uint16_t last_usage;
// One HID IN transfer at a time; these fields belong to the USB task/callbacks.
// RP2350 queues completions before reset/setup; tud_task drains them before
// usb_media_task can submit a report in the next configuration.
static bool volume_pending;
static uint16_t volume_usage;
static uint32_t volume_generation, volume_transfer_epoch;

bool usb_media_enqueue(enum usb_media_command command) {
    uint16_t usage;
    switch (command) {
        case USB_MEDIA_PLAY: case USB_MEDIA_PAUSE: usage = 0x00cd; break;
        case USB_MEDIA_NEXT: usage = 0x00b5; break;
        case USB_MEDIA_PREVIOUS: usage = 0x00b6; break;
        case USB_MEDIA_STOP: usage = 0x00b7; break;
        case USB_MEDIA_VOLUME_UP: usage = 0x00e9; break;
        case USB_MEDIA_VOLUME_DOWN: usage = 0x00ea; break;
        default: return false;
    }
    uint32_t state = atomic_load_explicit(&host_state, memory_order_acquire);
    if (!(state & 1u)) return false;
    uint32_t w = atomic_load_explicit(&written, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&read, memory_order_acquire);
    if (w - r >= MEDIA_QUEUE_LENGTH) return false;
    queue[w % MEDIA_QUEUE_LENGTH] = (media_event_t){
        .generation=state, .usage=usage,
        .volume_epoch=atomic_load_explicit(&volume_epoch, memory_order_acquire),
        .deadline=time_us_32()+5000000u};
    atomic_store_explicit(&written, w + 1u, memory_order_release);
    return atomic_load_explicit(&host_state, memory_order_acquire) == state;
}

uint32_t usb_media_generation(void) {
    return atomic_load_explicit(&host_state, memory_order_acquire);
}

uint32_t usb_media_volume_sent(void) {
    return atomic_load_explicit(&volume_sent, memory_order_acquire);
}

void usb_media_cancel_volume(void) {
    atomic_fetch_add_explicit(&volume_epoch, 1u, memory_order_acq_rel);
}

void usb_media_set_available(bool available) {
    dongle_control_available(available);
    dongle_rpc_available(available);
    uint32_t old = atomic_load_explicit(&host_state, memory_order_relaxed);
    uint32_t next = ((old + 2u) & ~1u) | (uint32_t)available;
    atomic_store_explicit(&host_state, next, memory_order_release);
    usb_media_cancel_volume();
    volume_pending = false;
    atomic_store_explicit(&read, atomic_load_explicit(&written, memory_order_acquire),
                          memory_order_release);
    last_usage = 0;
    // A suspended bus cannot transmit; release first when the host returns.
    release_pending = true;
}

static bool send_usage(uint16_t usage) {
    const uint8_t report[] = {(uint8_t)usage, (uint8_t)(usage >> 8)};
    volume_pending = usage == 0x00e9 || usage == 0x00ea;
    volume_usage = usage;
    volume_generation = usb_media_generation();
    volume_transfer_epoch = atomic_load_explicit(&volume_epoch, memory_order_acquire);
    if (!tud_hid_report(1, report, sizeof(report))) {
        volume_pending = false;
        return false;
    }
    last_usage = usage;
    return true;
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t length) {
    // TinyUSB includes the report ID in its completed endpoint buffer.
    if (instance != 0 || report == NULL || length != 3 || report[0] != 1 ||
        !volume_pending || (uint16_t)(report[1] | (uint16_t)report[2] << 8) != volume_usage) return;
    volume_pending = false;
    if (volume_generation == usb_media_generation() &&
        volume_transfer_epoch == atomic_load_explicit(&volume_epoch, memory_order_acquire))
        atomic_fetch_add_explicit(&volume_sent, 1u, memory_order_release);
}

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t type,
                              uint8_t const *report, uint16_t length) {
    (void)report;
    (void)length;
    if (instance == 0 && type == HID_REPORT_TYPE_INPUT) volume_pending = false;
    // An accepted press still gets a release before any later queued key.
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
        bool volume = event.usage==0x00e9 || event.usage==0x00ea;
        if (event.generation != state || (volume &&
            (event.volume_epoch != atomic_load_explicit(&volume_epoch, memory_order_acquire) ||
             (int32_t)(time_us_32()-event.deadline)>=0))) {
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
    if (instance == 0 && report_id == 2 && report_type == HID_REPORT_TYPE_FEATURE)
        return dongle_control_get(buffer, requested);
    if (instance == 0 && report_id == 3 && report_type == HID_REPORT_TYPE_FEATURE)
        return dongle_rpc_get(buffer, requested);
    if (instance != 0 || report_id != 1 || report_type != HID_REPORT_TYPE_INPUT ||
        buffer == NULL || requested < 2) return 0;
    buffer[0] = (uint8_t)last_usage;
    buffer[1] = (uint8_t)(last_usage >> 8);
    return 2;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t size) {
    if (instance == 0 && report_id == 2 && report_type == HID_REPORT_TYPE_FEATURE)
        dongle_control_set(buffer, size);
    if (instance == 0 && report_id == 3 && report_type == HID_REPORT_TYPE_FEATURE)
        dongle_rpc_set(buffer, size);
}
