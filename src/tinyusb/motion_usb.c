// SPDX-License-Identifier: GPL-3.0-only
#include <string.h>

#include "hardware/sync.h"
#include "pico/time.h"
#include "tusb.h"
#include "motion_usb.h"

// The USB task and BTstack both run on core 0. Keep this mailbox newest-only.
static uint32_t serial, lease, renewed, arrived, revision;
static uint8_t requested_rate;
static bool pending;
static uint8_t latest[USB_MOTION_SIZE];

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void expire(uint32_t now) {
    if (lease && (uint32_t)(now - renewed) >= 3000u) {
        lease = 0;
        pending = false;
    }
}

void usb_motion_reset(void) {
    uint32_t irq = save_and_disable_interrupts();
    lease = 0;
    pending = false;
    restore_interrupts(irq);
}

void usb_motion_set_rate(uint8_t rate) {
    if (rate != 0 && rate != 30 && rate != 50 && rate != 100) return;
    if (rate == 0) {
        usb_motion_reset();
        return;
    }
    uint32_t irq = save_and_disable_interrupts();
    uint32_t now = now_ms();
    expire(now);
    if (!lease || requested_rate != rate) {
        if (!++serial) ++serial;
        lease = serial;
        requested_rate = rate;
        pending = false;
    }
    renewed = now;
    restore_interrupts(irq);
}

uint32_t usb_motion_lease(uint8_t *rate) {
    uint32_t irq = save_and_disable_interrupts();
    expire(now_ms());
    uint32_t current = lease;
    *rate = current ? requested_rate : 0;
    restore_interrupts(irq);
    return current;
}

uint8_t usb_motion_rate(void) {
    uint8_t rate;
    usb_motion_lease(&rate);
    return rate;
}

void usb_motion_invalidate(void) {
    uint32_t irq = save_and_disable_interrupts();
    pending = false;
    restore_interrupts(irq);
}

void usb_motion_publish(uint32_t owner, uint32_t now, const uint8_t report[USB_MOTION_SIZE]) {
    uint32_t irq = save_and_disable_interrupts();
    expire(now_ms());
    if (owner && owner == lease) {
        memcpy(latest, report, sizeof(latest));
        arrived = now;
        ++revision;
        pending = true;
    }
    restore_interrupts(irq);
}

void usb_motion_task(void) {
    if (!tud_hid_n_ready(USB_MOTION_INSTANCE)) return;
    uint8_t report[USB_MOTION_SIZE];
    uint32_t irq = save_and_disable_interrupts();
    uint32_t now = now_ms();
    expire(now);
    bool fresh = pending && (uint32_t)(now - arrived) <= 100u;
    uint32_t copied = revision;
    if (fresh) memcpy(report, latest, sizeof(report));
    else pending = false;
    restore_interrupts(irq);
    if (fresh && tud_hid_n_report(USB_MOTION_INSTANCE, USB_MOTION_INPUT_ID, report, sizeof(report))) {
        irq = save_and_disable_interrupts();
        if (revision == copied) pending = false;
        restore_interrupts(irq);
    }
}
