// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "tusb.h"
#include "../src/tinyusb/media_control.h"

static bool ready = true;
static bool reject_report;
static uint16_t reports[128];
static unsigned count;
static unsigned ready_queries;

bool tud_hid_ready(void) { ready_queries++; return ready; }
bool tud_hid_report(uint8_t report_id, const void *report, uint16_t length) {
    assert(report_id == 0 && length == 2 && ready);
    if (reject_report) return false;
    const uint8_t *bytes = report;
    assert(count < sizeof(reports) / sizeof(reports[0]));
    reports[count++] = bytes[0] | (uint16_t)(bytes[1] << 8);
    ready = false;
    return true;
}
uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t *, uint16_t);

static void tick(void) { ready = true; usb_media_task(); }

int main(void) {
    assert(!usb_media_enqueue(USB_MEDIA_PLAY));
    usb_media_set_available(true);
    tick();
    assert(count == 1 && reports[0] == 0);
    unsigned queries = ready_queries;
    for (unsigned i = 0; i < 2000; ++i) tick();
    assert(ready_queries == queries && count == 1); // idle ticks do not poll the endpoint
    assert(!usb_media_enqueue((enum usb_media_command)99));
    assert(usb_media_enqueue(USB_MEDIA_PLAY));
    tick();
    assert(reports[1] == 0xb0);
    usb_media_task();
    assert(count == 2); // endpoint busy: never overwrite a press with its release
    tick();
    assert(reports[2] == 0);

    for (unsigned i = 0; i < 8; ++i) assert(usb_media_enqueue(USB_MEDIA_NEXT));
    assert(!usb_media_enqueue(USB_MEDIA_PAUSE));
    for (unsigned i = 0; i < 16; ++i) tick();
    for (unsigned i = 3; i < 19; i += 2) {
        assert(reports[i] == 0xb5 && reports[i + 1] == 0);
    }
    assert(usb_media_enqueue(USB_MEDIA_PAUSE));
    tick();
    assert(reports[count - 1] == 0xb1);
    assert(usb_media_enqueue(USB_MEDIA_STOP));
    unsigned before = count;
    usb_media_set_available(false);
    assert(!usb_media_enqueue(USB_MEDIA_PLAY));
    tick();
    assert(count == before);
    usb_media_set_available(true);
    tick();
    assert(count == before + 1 && reports[count - 1] == 0);
    tick();
    assert(count == before + 1); // queued pre-suspend Stop was discarded

    assert(usb_media_enqueue(USB_MEDIA_PREVIOUS));
    reject_report = true;
    tick();
    assert(count == before + 1);
    reject_report = false;
    tick();
    assert(reports[count - 1] == 0xb6); // rejected transfer retained the event
    uint8_t report[2];
    assert(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_INPUT, report, 2) == 2);
    assert(report[0] == 0xb6 && report[1] == 0);
    assert(tud_hid_get_report_cb(1, 0, HID_REPORT_TYPE_INPUT, report, 2) == 0);
    assert(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_OUTPUT, report, 2) == 0);
    assert(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_INPUT, report, 1) == 0);
    tick();
    assert(reports[count - 1] == 0);
    assert(usb_media_enqueue(USB_MEDIA_STOP));
    tick();
    assert(reports[count - 1] == 0xb7);
    before = count;
    reject_report = true;
    tick();
    assert(count == before); // a failed release must be retried before any next press
    reject_report = false;
    tick();
    assert(reports[count - 1] == 0);
    puts("media controls: bounded queue, ordered releases, suspend/reset and retry passed");
}
