// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "../src/tinyusb/media_control.h"

void dongle_control_available(bool available) { (void)available; }
void dongle_rpc_available(bool available) { (void)available; }
uint16_t dongle_rpc_get(uint8_t *buffer, uint16_t size) { (void)buffer; (void)size; return 0; }
void dongle_rpc_set(const uint8_t *buffer, uint16_t size) { (void)buffer; (void)size; }
uint16_t dongle_control_get(uint8_t *buffer, uint16_t size) { (void)buffer; (void)size; return 0; }
void dongle_control_set(const uint8_t *buffer, uint16_t size) { (void)buffer; (void)size; }

static bool ready = true;
static bool reject_report;
static uint16_t reports[128];
static unsigned count;
static unsigned ready_queries;
static uint32_t test_time_us;
static bool transfer_pending;
static uint8_t transfer_report[3];
void tud_hid_report_complete_cb(uint8_t, uint8_t const *, uint16_t);
void tud_hid_report_failed_cb(uint8_t, hid_report_type_t, uint8_t const *, uint16_t);
uint32_t time_us_32(void) { return test_time_us; }

bool tud_hid_ready(void) { ready_queries++; return ready; }
bool tud_hid_report(uint8_t report_id, const void *report, uint16_t length) {
    assert(report_id == 1 && length == 2 && ready);
    if (reject_report) return false;
    const uint8_t *bytes = report;
    assert(count < sizeof(reports) / sizeof(reports[0]));
    reports[count++] = bytes[0] | (uint16_t)(bytes[1] << 8);
    transfer_report[0] = report_id;
    transfer_report[1] = bytes[0];
    transfer_report[2] = bytes[1];
    transfer_pending = true;
    ready = false;
    return true;
}
uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t *, uint16_t);

static void complete_transfer(bool success) {
    if (transfer_pending) {
        transfer_pending = false;
        if (success) tud_hid_report_complete_cb(0, transfer_report, sizeof(transfer_report));
        else tud_hid_report_failed_cb(0, HID_REPORT_TYPE_INPUT, transfer_report, 0);
    }
    ready = true;
}
static void tick(void) { complete_transfer(true); usb_media_task(); }

static void fresh_host(void) {
    usb_media_set_available(false);
    assert(!(usb_media_generation() & 1u));
    usb_media_set_available(true);
    assert(usb_media_generation() & 1u);
    reject_report = false;
    transfer_pending = false;
    count = 0;
    tick();
    assert(count == 1 && reports[0] == 0);
    complete_transfer(true);
    count = 0;
}

static void test_volume_queue(void) {
    fresh_host();test_time_us=100;
    uint32_t sent=usb_media_volume_sent();
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));
    assert(usb_media_volume_sent()==sent); // Enqueue is not a delivered key.
    reject_report=true;tick();assert(!count && usb_media_volume_sent()==sent);
    reject_report=false;tick();assert(count==1 && reports[0]==0xe9 && usb_media_volume_sent()==sent);
    tick();assert(count==2 && reports[1]==0 && usb_media_volume_sent()==sent+1);
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_DOWN));tick();tick();
    assert(reports[2]==0xea && reports[3]==0 && usb_media_volume_sent()==sent+2);

    // Cancellation removes only unsent volume keys, preserving ordinary media.
    fresh_host();sent=usb_media_volume_sent();
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));
    assert(usb_media_enqueue(USB_MEDIA_NEXT));
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_DOWN));
    usb_media_cancel_volume();tick();tick();tick();
    assert(count==2 && reports[0]==0xb5 && reports[1]==0 && usb_media_volume_sent()==sent);
    fresh_host();sent=usb_media_volume_sent();
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));usb_media_cancel_volume();
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_DOWN));tick();tick();
    assert(count==2 && reports[0]==0xea && reports[1]==0 && usb_media_volume_sent()==sent+1);

    // An already sent key must still be released even if its epoch is cancelled.
    fresh_host();sent=usb_media_volume_sent();
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));
    assert(usb_media_enqueue(USB_MEDIA_PLAY));tick();
    assert(count==1 && reports[0]==0xe9 && usb_media_volume_sent()==sent);
    usb_media_cancel_volume();reject_report=true;tick();assert(count==1);
    reject_report=false;tick();assert(count==2 && reports[1]==0);
    tick();tick();assert(count==4 && reports[2]==0xcd && reports[3]==0);

    // Opposite and repeated gestures retain their press/release order.
    fresh_host();sent=usb_media_volume_sent();
    const enum usb_media_command burst[]={USB_MEDIA_VOLUME_UP,USB_MEDIA_VOLUME_DOWN,
        USB_MEDIA_VOLUME_UP,USB_MEDIA_VOLUME_UP,USB_MEDIA_VOLUME_DOWN,
        USB_MEDIA_VOLUME_DOWN,USB_MEDIA_VOLUME_UP,USB_MEDIA_VOLUME_DOWN};
    for(unsigned i=0;i<8;++i)assert(usb_media_enqueue(burst[i]));
    assert(!usb_media_enqueue(USB_MEDIA_VOLUME_UP));
    for(unsigned i=0;i<16;++i)tick();
    assert(count==16 && usb_media_volume_sent()==sent+8);
    for(unsigned i=0;i<8;++i)
        assert(reports[2*i]==(burst[i]==USB_MEDIA_VOLUME_UP?0xe9:0xea) && reports[2*i+1]==0);

    // An endpoint stall expires volume intent without expiring ordinary media.
    fresh_host();sent=usb_media_volume_sent();test_time_us=1000;
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));
    assert(usb_media_enqueue(USB_MEDIA_PREVIOUS));
    ready=false;usb_media_task();assert(!count);
    test_time_us+=5000000u;tick();tick();
    assert(count==2 && reports[0]==0xb6 && reports[1]==0 && usb_media_volume_sent()==sent);

    // Deadline comparison remains exact across the uint32 microsecond wrap.
    fresh_host();test_time_us=UINT32_MAX-100u;
    uint32_t deadline=test_time_us+5000000u;
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));
    test_time_us=deadline-1u;tick();tick();assert(count==2 && reports[0]==0xe9 && reports[1]==0);
    fresh_host();sent=usb_media_volume_sent();test_time_us=UINT32_MAX-100u;
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_DOWN));
    test_time_us=deadline;tick();assert(!count && usb_media_volume_sent()==sent);

    // Bus ownership changes invalidate queued volume and force a clean release.
    fresh_host();test_time_us=0;sent=usb_media_volume_sent();
    uint32_t generation=usb_media_generation();
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));
    usb_media_set_available(false);tick();assert(!count);
    usb_media_set_available(true);assert(usb_media_generation()!=generation);
    tick();tick();assert(count==1 && reports[0]==0 && usb_media_volume_sent()==sent);
    fresh_host();sent=usb_media_volume_sent();
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));tick();assert(reports[0]==0xe9);
    usb_media_set_available(false);tick();assert(count==1);
    usb_media_set_available(true);tick();
    assert(count==2 && reports[1]==0 && usb_media_volume_sent()==sent);
}

static void test_volume_completion(void) {
    fresh_host();uint32_t sent=usb_media_volume_sent();
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_DOWN));tick();
    assert(count==1 && reports[0]==0xea && usb_media_volume_sent()==sent);
    // A queued/stalled IN report must never authorize physical gain restoration.
    test_time_us+=200000u;usb_media_task();
    assert(count==1 && usb_media_volume_sent()==sent);
    const uint8_t wrong[]={1,0xe9,0};
    tud_hid_report_complete_cb(1,transfer_report,3);
    tud_hid_report_complete_cb(0,transfer_report,2);
    tud_hid_report_complete_cb(0,wrong,3);
    assert(usb_media_volume_sent()==sent);
    complete_transfer(false);usb_media_task();
    assert(count==2 && reports[1]==0 && usb_media_volume_sent()==sent);
    tick();assert(usb_media_enqueue(USB_MEDIA_PLAY));tick();tick();
    assert(reports[2]==0xcd && reports[3]==0 && usb_media_volume_sent()==sent);

    fresh_host();sent=usb_media_volume_sent();
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));tick();
    uint8_t old_report[3];memcpy(old_report,transfer_report,sizeof(old_report));
    usb_media_set_available(false);usb_media_set_available(true);
    tud_hid_report_complete_cb(0,old_report,3);
    assert(usb_media_volume_sent()==sent);
    transfer_pending=false;tick();complete_transfer(true);
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_DOWN));tick();
    tud_hid_report_complete_cb(0,old_report,3);
    assert(usb_media_volume_sent()==sent);
    complete_transfer(true);assert(usb_media_volume_sent()==sent+1);
    tud_hid_report_complete_cb(0,transfer_report,3);
    assert(usb_media_volume_sent()==sent+1); // Duplicate completion is not another key.
    usb_media_task();assert(reports[count-1]==0);tick();

    // The RP2350 DCD queues old completions before reset/setup; no new HID
    // transfer can overtake that queue. Repeat the same usage across reset.
    fresh_host();sent=usb_media_volume_sent();
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));tick();
    memcpy(old_report,transfer_report,sizeof(old_report));
    usb_media_set_available(false);usb_media_set_available(true);
    tud_hid_report_complete_cb(0,old_report,3);
    assert(usb_media_volume_sent()==sent);
    transfer_pending=false;tick();complete_transfer(true);
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));tick();
    assert(usb_media_volume_sent()==sent);
    complete_transfer(true);assert(usb_media_volume_sent()==sent+1);
    usb_media_task();tick();

    fresh_host();sent=usb_media_volume_sent();
    assert(usb_media_enqueue(USB_MEDIA_VOLUME_UP));tick();
    usb_media_cancel_volume();complete_transfer(true);usb_media_task();
    assert(usb_media_volume_sent()==sent && reports[1]==0);
    tick();
}

static void test_automatic_pause_is_not_a_toggle(void) {
    fresh_host();
    assert(usb_media_enqueue(USB_MEDIA_PAUSE_ONLY));tick();tick();
    assert(count==2 && reports[0]==0xb1 && reports[1]==0);
    // Repeated ownership loss still emits Pause, including when the host is already paused.
    assert(usb_media_enqueue(USB_MEDIA_PAUSE_ONLY));tick();tick();
    assert(count==4 && reports[2]==0xb1 && reports[3]==0);
    assert(usb_media_enqueue(USB_MEDIA_PAUSE));tick();tick();
    assert(usb_media_enqueue(USB_MEDIA_PLAY));tick();tick();
    assert(count==8 && reports[4]==0xcd && reports[6]==0xcd); // Intentional stems retain their toggle.
    fresh_host();assert(usb_media_enqueue(USB_MEDIA_PAUSE_ONLY));
    reject_report=true;tick();assert(!count);
    reject_report=false;tick();tick();assert(count==2 && reports[0]==0xb1 && reports[1]==0);
    fresh_host();assert(usb_media_enqueue(USB_MEDIA_PAUSE_ONLY));usb_media_set_available(false);
    usb_media_set_available(true);tick();tick();assert(count==1 && reports[0]==0);
    fresh_host();
    assert(usb_media_enqueue(USB_MEDIA_PAUSE_ONLY));
    assert(usb_media_enqueue(USB_MEDIA_PLAY));ready=false;
    usb_media_task();assert(!count);
    usb_media_cancel_pause();tick();tick();
    assert(count==2 && reports[0]==0xcd && reports[1]==0); // Return takeover discards only the obsolete automatic Pause.
    fresh_host();test_time_us=UINT32_MAX-100u;
    assert(usb_media_enqueue(USB_MEDIA_PAUSE_ONLY));test_time_us+=5000000u;tick();assert(!count);
    fresh_host();assert(usb_media_enqueue(USB_MEDIA_PAUSE_ONLY));tick();assert(reports[0]==0xb1);
    usb_media_cancel_pause();tick();assert(count==2 && reports[1]==0); // An accepted press still receives its release.
    usb_media_set_available(false);
}

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
    assert(reports[1] == 0xcd);
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
    assert(reports[count - 1] == 0xcd);
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
    assert(tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_INPUT, report, 2) == 2);
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
    test_volume_queue();
    test_volume_completion();
    test_automatic_pause_is_not_a_toggle();
    puts("media controls: bounded queue, ordered releases, completed volume receipts/epochs/expiry, suspend/reset and retry passed");
}
