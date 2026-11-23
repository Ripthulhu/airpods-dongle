// SPDX-License-Identifier: GPL-3.0-only
#include <stddef.h>
#include <string.h>
#include "pico/time.h"

#include "head_motion.h"
#include "../tinyusb/motion_usb.h"

static const uint8_t prefix[] = {4,0,4,0,0x17,0,0,0,0x10,0};
static const uint8_t motion_descriptor[] = {
    0x06,0x0c,0xff,0x09,0x11,0xa1,0x01,0x06,0x00,0xff,0x09,0x23,0xa1,0x00,0x85,0x01,
    0x06,0x15,0xff,0x09,0x04,0x95,0x01,0x75,0x40,0x81,0x02,0x06,0x0c,0xff,0x09,0x0d,
    0x76,0xb0,0x05,0x81,0x02,0x85,0x02,0x09,0x0e,0x81,0x02,0xc0,0x85,0x01,0x05,0x20,
    0x0a,0x0e,0x03,0x75,0x20,0xb1,0x02,0x85,0x02,0x0a,0x1b,0x03,0xb1,0x02,0x06,0x15,
    0xff,0x0a,0x01,0x00,0x85,0x03,0xb1,0x02,0x85,0x04,0x05,0x20,0x0a,0x1a,0x03,0xb1,
    0x02,0x85,0xff,0x06,0x15,0xff,0x0a,0x20,0x00,0x75,0x08,0x95,0xe0,0xb1,0x02,0xc0,
};

// shortcut: bound the envelope to 1 KiB, raise it only if a supported capture needs it.
static uint8_t frame[1024];
static uint16_t used;
static uint32_t fragment_ms, generation, active_lease, probe_ms, last_sample_ms;
static uint64_t announcement, last_timestamp;
static uint16_t tx_sequence;
static uint8_t role[4], service, probes, applied_rate;
static bool receive_enabled, timed_out, retried;
static realtime_metric_t rx_work;
static enum { STOPPED, START_FIFO_SET, RUNNING, STOP_REQUESTED, STOP_INTERVAL_SET } stream;

static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}

static void new_source(void) {
    ++generation;
    last_timestamp = 0;
    probes = 0;
    timed_out = false;
    retried = false;
    usb_motion_invalidate();
}

static void forget_service(void) {
    service = 0;
    stream = STOPPED;
    new_source();
}

void head_motion_reset(void) {
    receive_enabled = false;
    used = 0;
    active_lease = 0;
    announcement = 0;
    tx_sequence = 0;
    memset(role, 0, sizeof(role));
    forget_service();
}

void head_motion_role(const uint8_t next[4]) {
    if (next[0] > 2) return;
    if (!memcmp(role, next, sizeof(role))) return;
    bool changed_bud = role[0] != next[0];
    memcpy(role, next, sizeof(role));
    used = 0;
    if (changed_bud) {
        announcement = 0;
        forget_service();
    } else {
        // Wear flags can change while the same bud still owns the motion service.
        ++generation;
        usb_motion_invalidate();
    }
}

typedef struct { const uint8_t *p; size_t size; } bytes_t;
typedef struct { bytes_t bytes; uint64_t number; unsigned id, wire; } field_t;

static bool varint(bytes_t *b, uint64_t *value) {
    *value = 0;
    for (unsigned shift = 0; b->size && shift < 64; shift += 7) {
        uint8_t byte = *b->p++;
        --b->size;
        if (shift == 63 && byte > 1) return false;
        *value |= (uint64_t)(byte & 127) << shift;
        if (!(byte & 128)) return true;
    }
    return false;
}

static bool field(bytes_t *b, field_t *f) {
    uint64_t key, len;
    if (!varint(b, &key) || key < 8 || (key >> 3) > 4096) return false;
    *f = (field_t){.id = (unsigned)(key >> 3), .wire = key & 7};
    switch (f->wire) {
        case 0: return varint(b, &f->number);
        case 1: len = 8; break;
        case 2: if (!varint(b, &len)) return false; break;
        case 5: len = 4; break;
        default: return false;
    }
    if (len > b->size) return false;
    f->bytes = (bytes_t){b->p, (size_t)len};
    b->p += len;
    b->size -= len;
    return true;
}

static bool item(bytes_t *b, unsigned *kind, bytes_t *value) {
    if (b->size < 4) return false;
    uint32_t header = le32(b->p);
    unsigned count = header & 0xffffffu;
    *kind = (header >> 24) & 127;
    unsigned size;
    if (*kind == 4 && count <= 64) size = 8;
    else if (*kind >= 8 && *kind <= 10) size = count;
    else if (*kind == 11 && count <= 1) size = 0;
    else return false;
    unsigned total = 4 + ((size + 3) & ~3u);
    if (total > b->size) return false;
    *value = (bytes_t){b->p + 4, size};
    b->p += total;
    b->size -= total;
    return true;
}

static bool string_is(bytes_t b, const char *s) {
    if (b.size && b.p[b.size - 1] == 0) --b.size;
    return b.size == strlen(s) && !memcmp(b.p, s, b.size);
}

static bool is_motion_descriptor(bytes_t b) {
    if (b.size < 8 || le32(b.p) != 0xd3) return false;
    uint32_t root = le32(b.p + 4);
    unsigned count = root & 0xffffffu;
    if (((root >> 24) & 127) != 1 || count > 96) return false;
    b.p += 8;
    b.size -= 8;
    unsigned seen = 0;
    for (unsigned i = 0; i < count; ++i) {
        bytes_t key, value;
        unsigned kind, match = 0;
        if (!item(&b, &kind, &key) || (kind != 8 && kind != 9) || key.size > 128 ||
            !item(&b, &kind, &value)) return false;
        if (string_is(key, "AccessoryService")) {
            if ((kind != 8 && kind != 9) || !string_is(value, "devmotion6")) return false;
            match = 1;
        } else if (string_is(key, "VendorID") || string_is(key, "MaxReportSize")) {
            bool vendor = string_is(key, "VendorID");
            if (kind != 4 || le32(value.p) != (vendor ? 1452u : 191u) || le32(value.p + 4)) return false;
            match = vendor ? 2 : 4;
        } else if (string_is(key, "ReportDescriptor")) {
            if (kind != 10 || value.size != sizeof(motion_descriptor) ||
                memcmp(value.p, motion_descriptor, sizeof(motion_descriptor))) return false;
            match = 8;
        }
        if (seen & match) return false;
        seen |= match;
    }
    return b.size == 0 && seen == 15;
}

static bool service_data(bytes_t b, unsigned data_id, uint8_t *id, bytes_t *data) {
    *id = 0;
    *data = (bytes_t){0};
    for (field_t f; b.size;) {
        if (!field(&b, &f)) return false;
        if (f.id == 1) {
            if (*id || f.wire != 0 || !f.number || f.number > 127) return false;
            *id = (uint8_t)f.number;
        } else if (f.id == data_id) {
            if (data->p || f.wire != 2) return false;
            *data = f.bytes;
        }
    }
    return *id && data->p;
}

static void sample(bytes_t record, uint32_t now) {
    if ((record.size != 56 && record.size != 58) || record.p[0] != 1) return;
    const uint8_t *p = record.p;
    uint64_t timestamp = le32(p + 1) | (uint64_t)le32(p + 5) << 32;
    if (timestamp <= last_timestamp || (timestamp >> 63)) return;
    last_timestamp = timestamp;
    last_sample_ms = now;
    uint8_t report[USB_MOTION_SIZE] = {1, record.size == 58};
    memcpy(report + 2, role, sizeof(role));
    put32(report + 6, generation);
    put32(report + 10, now);
    memcpy(report + 14, p + 1, 8);
    if (report[1]) memcpy(report + 22, p + 54, 4);
    memcpy(report + 26, p + 20, 12);
    memcpy(report + 38, p + 38, 12);
    usb_motion_publish(active_lease, now, report);
}

static void payload(bytes_t b, uint32_t now) {
    uint64_t sequence = 0, log = 0;
    uint8_t mapped = 0;
    unsigned seen = 0;
    bool descriptors = false, replaced = false;
    bytes_t packed = {0}, command = {0};
    bool trailer = false;
    for (field_t f; b.size;) {
        if (log == 5 && b.size == 2 && b.p[0] == 0xff && b.p[1] == 0xff) {
            trailer = true;
            break;
        }
        if (!field(&b, &f)) return;
        unsigned bit = f.id == 1 || f.id == 2 || f.id == 3 || f.id == 7 ? 1u << f.id : 0;
        if (seen & bit) return;
        seen |= bit;
        if ((f.id == 1 || f.id == 2) && f.wire != 0) return;
        if ((f.id == 3 || f.id == 5 || f.id == 7) && f.wire != 2) return;
        switch (f.id) {
            case 1: sequence = f.number; break;
            case 2: log = f.number; break;
            case 3: packed = f.bytes; break;
            case 7: command = f.bytes; break;
            case 5: {
                bytes_t dictionary;
                uint8_t id;
                if (!service_data(f.bytes, 2, &id, &dictionary)) return;
                descriptors = true;
                if (is_motion_descriptor(dictionary)) {
                    if (mapped) return;
                    mapped = id;
                } else if (id == service) {
                    replaced = true;
                }
                break;
            }
            default: break;
        }
    }
    if (trailer ? log != 5 : (log != 1 && log != 3)) return;
    if (descriptors) {
        if (trailer || !sequence || packed.p || command.p) return;
        if ((sequence == 1 && announcement > 1) || replaced || (mapped && mapped != service))
            forget_service();
        announcement = sequence;
        if (mapped && !replaced) service = mapped;
        return;
    }
    if (stream != RUNNING || timed_out || !service || !role[0] || !!packed.p == !!command.p) return;
    if (packed.p) {
        if ((packed.size != 58 && packed.size != 60) || (trailer && packed.size != 58) ||
            packed.p[0] != service || packed.p[1]) return;
        sample((bytes_t){packed.p + 2, packed.size - 2}, now);
    } else if (!trailer) {
        bytes_t record;
        uint8_t id;
        if (service_data(command, 3, &id, &record) && id == service) sample(record, now);
    }
}

bool head_motion_receive(const uint8_t *p, uint16_t n, uint32_t now) {
    if (!receive_enabled || !n) return false;
    uint32_t started = time_us_32();
    if (used && (uint32_t)(now - fragment_ms) > 100u) used = 0;
    if (n >= sizeof(prefix) && !memcmp(p, prefix, sizeof(prefix))) used = 0;
    else if (n >= 6 && !memcmp(p, prefix, 4) && memcmp(p, prefix, n < sizeof(prefix) ? n : sizeof(prefix)))
        return false;
    bool consumed = used != 0;
    while (n) {
        if (used < sizeof(prefix) && p[0] != prefix[used]) {
            used = 0;
            break;
        }
        if (used < 12) {
            frame[used++] = *p++;
            --n;
            if (used == 12 && !frame[10] && !frame[11]) used = 0;
        } else {
            unsigned end = 12u + frame[10] + ((unsigned)frame[11] << 8);
            if (end > sizeof(frame)) {
                used = 0;
                break;
            }
            unsigned count = end - used;
            if (count > n) count = n;
            memcpy(frame + used, p, count);
            used += count;
            p += count;
            n -= count;
            if (used == end) {
                payload((bytes_t){frame + 12, end - 12}, now);
                used = 0;
            }
        }
        consumed = true;
    }
    fragment_ms = now;
    // Wall time through parsing and mailbox publication includes any IRQ preemption.
    // Controller reception and the later USB transfer are outside this interval.
    if (consumed) realtime_metric_record(&rx_work, started, time_us_32(), UINT32_MAX);
    return consumed;
}

realtime_metric_t head_motion_receive_work(void) {
    return rx_work;
}

static bool request(uint8_t report, uint32_t value, bool (*send)(uint8_t *, uint16_t)) {
    uint8_t packet[30];
    memcpy(packet, prefix, sizeof(prefix));
    unsigned end = 12;
    packet[end++] = 8;
    uint16_t seq = tx_sequence == 16383 ? 1 : tx_sequence + 1;
    if (seq >= 128) packet[end++] = (seq & 127) | 128;
    packet[end++] = seq < 128 ? seq : seq >> 7;
    if (report) {
        const uint8_t command[] = {0x42,11,8,service,0x10,2,0x1a,5,report};
        memcpy(packet + end, command, sizeof(command));
        end += sizeof(command);
        put32(packet + end, value);
        end += 4;
    } else {
        packet[end++] = 0x22;
        packet[end++] = 0;
    }
    packet[10] = end - 12;
    packet[11] = 0;
    if (!send(packet, end)) return false;
    tx_sequence = seq;
    return true;
}

void head_motion_poll(uint32_t now, uint8_t max_rate, bool (*send)(uint8_t *, uint16_t)) {
    uint8_t rate = 0;
    uint32_t lease = max_rate ? usb_motion_lease(&rate) : 0;
    if (lease != active_lease) {
        active_lease = lease;
        used = 0;
        new_source();
        if (stream == RUNNING || stream == START_FIFO_SET) stream = STOP_REQUESTED;
    }
    receive_enabled = lease != 0;
    if (rate > max_rate) rate = max_rate;
    if (stream == RUNNING && (uint32_t)(now - last_sample_ms) >= 3000u) {
        usb_motion_invalidate();
        ++generation;
        last_timestamp = 0;
        used = 0;
        timed_out = retried;
        retried = true;
        stream = STOP_REQUESTED;
    }
    if (!lease || timed_out || stream == STOP_REQUESTED || stream == STOP_INTERVAL_SET) {
        if (stream == RUNNING || stream == START_FIFO_SET || stream == STOP_REQUESTED) {
            if (request(1, 0, send)) stream = STOP_INTERVAL_SET;
        } else if (stream == STOP_INTERVAL_SET && request(4, 10, send)) {
            stream = STOPPED;
        }
        return;
    }
    if (!service) {
        if (probes < 3 && (!probes || (uint32_t)(now - probe_ms) >= 1000u) && request(0, 0, send)) {
            ++probes;
            probe_ms = now;
        }
    } else if (role[0] && stream == STOPPED) {
        if (request(4, 1, send)) stream = START_FIFO_SET;
    } else if (stream == START_FIFO_SET || (stream == RUNNING && rate != applied_rate)) {
        if (request(1, 1000000u / rate, send)) {
            if (stream == START_FIFO_SET) last_sample_ms = now;
            stream = RUNNING;
            applied_rate = rate;
        }
    }
}
