// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_VOLUME_TRACE_H
#define AIRPODS_VOLUME_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VOLUME_TRACE_EVENTS 16u
#define VOLUME_TRACE_LEASE_US 60000000u
enum {
    VOLUME_TRACE_NOTIFY = 1, VOLUME_TRACE_ACK, VOLUME_TRACE_TX,
    VOLUME_TRACE_HOST, VOLUME_TRACE_HID, VOLUME_TRACE_REPAIR
};

// Words: time_us; kind:4/ctype:4/label:4/flags:12/status:8;
// incoming/before/desired/sent bytes; completed/expected low 16-bit counters.
typedef struct {
    uint32_t events[VOLUME_TRACE_EVENTS][4];
    uint32_t started_us, total;
    uint8_t head, count, next, remaining;
    bool armed;
} volume_trace_t;
_Static_assert(sizeof(((volume_trace_t *)0)->events) == 256u,
               "Volume trace records must stay within 256 bytes");

// All calls are serialized by the Bluetooth context; no allocation or printing.
static inline void volume_trace_arm(volume_trace_t *s, uint32_t now_us) {
    memset(s, 0, sizeof(*s));
    s->started_us = now_us;
    s->armed = true;
}

static inline void volume_trace_record(volume_trace_t *s, uint32_t now_us,
                                       uint32_t meta, uint32_t levels, uint32_t receipts) {
    if (!s->armed) return;
    if ((uint32_t)(now_us - s->started_us) >= VOLUME_TRACE_LEASE_US) {
        s->armed = false;
        return;
    }
    uint32_t *event = s->events[s->head];
    event[0] = now_us; event[1] = meta; event[2] = levels; event[3] = receipts;
    s->head = (uint8_t)((s->head + 1u) % VOLUME_TRACE_EVENTS);
    if (s->count < VOLUME_TRACE_EVENTS) ++s->count;
    ++s->total;
}

static inline uint32_t volume_trace_freeze(volume_trace_t *s) {
    s->armed = false;
    s->next = (uint8_t)((s->head + VOLUME_TRACE_EVENTS - s->count) % VOLUME_TRACE_EVENTS);
    s->remaining = s->count;
    return s->total;
}

static inline bool volume_trace_take(volume_trace_t *s, uint32_t out[4]) {
    if (out == NULL || s->remaining == 0) return false;
    memcpy(out, s->events[s->next], sizeof(s->events[0]));
    s->next = (uint8_t)((s->next + 1u) % VOLUME_TRACE_EVENTS);
    --s->remaining;
    return true;
}

#endif
