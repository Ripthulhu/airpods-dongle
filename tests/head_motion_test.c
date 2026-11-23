// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <string.h>

#include "../src/btstack/head_motion.h"
#include "../src/tinyusb/motion_usb.h"
#include "head_motion_announcement.inc"

static uint32_t now, writes, reports;
static bool hid_ready, hid_accepts, tx_ready;
static uint8_t sent[32], output[USB_MOTION_SIZE];
static uint16_t sent_size;

uint32_t save_and_disable_interrupts(void) { return 0; }
void restore_interrupts(uint32_t previous) { assert(previous == 0); }
uint64_t get_absolute_time(void) { return (uint64_t)now * 1000; }
uint32_t to_ms_since_boot(uint64_t time) { return time / 1000; }
bool tud_hid_n_ready(uint8_t instance) { assert(instance == USB_MOTION_INSTANCE); return hid_ready; }
bool tud_hid_n_report(uint8_t instance, uint8_t id, const void *report, uint16_t length) {
    assert(instance == USB_MOTION_INSTANCE && id == USB_MOTION_INPUT_ID && length == sizeof(output));
    if (!hid_accepts) return false;
    memcpy(output, report, length);
    ++reports;
    return true;
}

static bool send(uint8_t *packet, uint16_t size) {
    if (!tx_ready) return false;
    assert(size <= sizeof(sent) && (unsigned)packet[10] + 12 == size);
    assert(memcmp(packet, motion_announcement, 10) == 0);
    memcpy(sent, packet, size);
    sent_size = size;
    ++writes;
    return true;
}

static void feature(uint8_t id, uint8_t number, uint16_t value) {
    assert(sent_size == 27 && sent[14] == 0x42 && sent[17] == id);
    assert(sent[22] == number && sent[23] == (value & 255) && sent[24] == (value >> 8));
}

static void setup(size_t split) {
    static const uint8_t role[] = {1,0,1,0};
    now = writes = reports = 0;
    hid_ready = hid_accepts = tx_ready = true;
    usb_motion_reset();
    head_motion_reset();
    head_motion_role(role);
    head_motion_poll(now, 100, send);
    assert(writes == 0 && usb_motion_rate() == 0);
    usb_motion_set_rate(25);
    assert(usb_motion_rate() == 0);
    usb_motion_set_rate(50);
    head_motion_poll(now, 100, send);
    assert(writes == 1 && sent_size == 16 && sent[13] == 1 && sent[14] == 0x22 && !sent[15]);
    head_motion_receive(motion_announcement, split, now);
    head_motion_receive(motion_announcement + split, sizeof(motion_announcement) - split, now);
    tx_ready = false;
    head_motion_poll(++now, 100, send);
    assert(writes == 1);
    tx_ready = true;
    head_motion_poll(++now, 100, send);
    assert(writes == 2 && sent[13] == 2);
    feature(16, 4, 1);
    head_motion_poll(++now, 100, send);
    assert(writes == 3 && sent[13] == 3);
    feature(16, 1, 20000);
}

static size_t make_report(uint8_t *frame, uint8_t log, uint8_t service, uint8_t timestamp) {
    size_t len = log == 5 ? 56 : 58;
    uint8_t body[] = {8,1,16,log,0x1a,(uint8_t)(len + 2),service,0};
    memcpy(frame, motion_announcement, 10);
    frame[10] = (uint8_t)(sizeof(body) + len + (log == 5 ? 2 : 0));
    frame[11] = 0;
    memcpy(frame + 12, body, sizeof(body));
    uint8_t *rec = frame + 12 + sizeof(body);
    memset(rec, 0, len);
    rec[0] = 1;
    rec[1] = timestamp;
    rec[20] = 0x45;
    rec[26] = 0xb2;
    rec[38] = 0x74;
    rec[44] = 0x6d;
    if (log == 5) rec[56] = rec[57] = 0xff;
    else memset(rec + 54, 0xff, 4);
    return 12 + frame[10];
}

int main(void) {
    for (size_t split = 0; split <= sizeof(motion_announcement); ++split) setup(split);
    uint8_t frame[160];
    size_t length = make_report(frame, 5, 14, 1);
    head_motion_receive(frame, length, now);
    usb_motion_task();
    assert(reports == 0);
    length = make_report(frame, 5, 16, 1);
    frame[length - 1] = 0;
    head_motion_receive(frame, length, now);
    usb_motion_task();
    assert(reports == 0);
    frame[length - 1] = 0xff;
    for (size_t split = 0; split <= length; ++split) {
        frame[21] = split + 1;
        head_motion_receive(frame, split, now);
        usb_motion_task();
        assert(reports == split + (split == length));
        head_motion_receive(frame + split, length - split, now);
        usb_motion_task();
        assert(reports == split + 1 && output[0] == 1 && !output[1] && output[2] == 1);
    }
    assert(output[14] == length + 1 && output[26] == 0x45 && output[32] == 0xb2 &&
           output[38] == 0x74 && output[44] == 0x6d);
    uint8_t old_generation = output[6];
    uint32_t before = reports;
    size_t first = make_report(frame, 3, 16, 90);
    size_t second = make_report(frame + first, 3, 16, 91);
    hid_ready = false;
    head_motion_receive(frame, first + second, now);
    usb_motion_task();
    assert(reports == before);
    hid_ready = true;
    usb_motion_task();
    assert(reports == ++before && output[1] == 1 && output[14] == 91 && output[25] == 0xff);

    frame[21] = 92;
    head_motion_receive(frame, first, now);
    now += 101;
    usb_motion_task();
    assert(reports == before);
    frame[21] = 93;
    head_motion_receive(frame, first, now);
    head_motion_role((const uint8_t[]){2,0,1,0});
    usb_motion_task();
    assert(reports == before);
    head_motion_poll(now, 100, send);
    assert(writes == 4 && sent[14] == 0x22);
    head_motion_receive(motion_announcement, sizeof(motion_announcement), now);
    head_motion_poll(now, 100, send);
    head_motion_poll(now, 100, send);
    feature(16, 1, 20000);
    frame[21] = 1;
    head_motion_receive(frame, first, now);
    usb_motion_task();
    assert(reports == ++before && output[2] == 2 && output[6] != old_generation && output[14] == 1);
    frame[21] = 2;
    head_motion_receive(frame, first, now);
    frame[21] = 3;
    frame[first] = 16;
    frame[first + 1] = 3;
    frame[10] += 2;
    head_motion_receive(frame, first + 2, now);
    usb_motion_task();
    assert(reports == ++before && output[14] == 2);
    frame[10] -= 2;
    head_motion_receive(frame, first, now);
    now = 2999;
    uint8_t rate;
    uint32_t lease = usb_motion_lease(&rate);
    usb_motion_set_rate(50);
    assert(usb_motion_lease(&rate) == lease && rate == 50);
    now += 3000;
    usb_motion_task();
    assert(!usb_motion_rate() && reports == before);
    head_motion_poll(now, 100, send);
    feature(16, 1, 0);
    head_motion_poll(now, 100, send);
    feature(16, 4, 10);

    setup(sizeof(motion_announcement));
    usb_motion_set_rate(100);
    assert(usb_motion_rate() == 100);
    head_motion_poll(now, 100, send);
    feature(16, 1, 0);
    head_motion_poll(now, 100, send);
    feature(16, 4, 10);
    head_motion_poll(now, 100, send);
    feature(16, 4, 1);
    head_motion_poll(now, 100, send);
    feature(16, 1, 10000);
    length = make_report(frame, 3, 16, 1);
    for (unsigned i = 1; i <= 100; ++i) {
        now += 10;
        frame[21] = i;
        head_motion_poll(now, 100, send);
        head_motion_receive(frame, length, now);
        usb_motion_task();
        assert(reports == i && output[14] == i);
    }
    now += 10;
    frame[21] = 101;
    hid_accepts = false;
    head_motion_receive(frame, length, now);
    usb_motion_task();
    assert(reports == 100);
    hid_accepts = true;
    usb_motion_task();
    assert(reports == 101 && output[14] == 101);
    uint8_t stable_generation = output[6];
    tx_ready = false;
    before = writes;
    head_motion_poll(now, 30, send);
    assert(writes == before);
    tx_ready = true;
    head_motion_poll(now, 30, send);
    feature(16, 1, 33333);
    assert(writes == before + 1 && usb_motion_rate() == 100);
    for (unsigned i = 102; i < 132; ++i) {
        now += 33;
        frame[21] = i;
        head_motion_poll(now, 30, send);
        head_motion_receive(frame, length, now);
        usb_motion_task();
        assert(reports == i && output[14] == i && output[6] == stable_generation);
    }
    assert(writes == before + 1);
    head_motion_poll(now, 100, send);
    feature(16, 1, 10000);
    assert(writes == before + 2);
    usb_motion_set_rate(100);
    now += 2500;
    usb_motion_set_rate(100);
    now += 500;
    head_motion_poll(now, 100, send);
    feature(16, 1, 0);
    head_motion_poll(++now, 100, send);
    feature(16, 4, 10);
    head_motion_poll(++now, 100, send);
    feature(16, 4, 1);
    head_motion_poll(++now, 100, send);
    feature(16, 1, 10000);
    now += 2000;
    usb_motion_set_rate(100);
    now += 1000;
    head_motion_poll(now, 100, send);
    feature(16, 1, 0);
    head_motion_poll(++now, 100, send);
    feature(16, 4, 10);
    before = writes;
    head_motion_poll(++now, 100, send);
    assert(writes == before);
}
