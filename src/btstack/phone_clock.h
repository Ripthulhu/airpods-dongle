// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PHONE_CLOCK_H
#define AIRPODS_PHONE_CLOCK_H
#include <stdint.h>

#define PHONE_PCM_CAPACITY 8192u // Use the existing PCM pool for phone transport stalls.
#define PHONE_JITTER_RESERVE 3840u // 80 ms covers observed phone ingress gaps.
#define PHONE_PACKET_FRAMES 960u // Allow a 20 ms ingress burst at 48 kHz.

typedef struct { int32_t fill_q8, target_q8; } phone_clock_t;

static inline uint32_t phone_clock_prefill(uint32_t frames) {
    uint32_t minimum = frames + PHONE_JITTER_RESERVE;
    return minimum < 1440u ? 1440u : minimum;
}

static inline void phone_clock_init(phone_clock_t *clock, uint32_t frames) {
    uint32_t a = PHONE_PACKET_FRAMES, b = frames;
    while (b != 0) { uint32_t next = a % b; a = b; b = next; }
    // Keep jitter reserve separate from the normal packet-arrival sawtooth.
    // The remainder is the mean sawtooth after reads of this codec's frame size.
    clock->target_q8 = (int32_t)(PHONE_JITTER_RESERVE + (PHONE_PACKET_FRAMES - a) / 2u) * 256;
    clock->fill_q8 = clock->target_q8;
}

// Weight by consumed samples: AAC/SBC and ELD have the same 320 ms averaging time.
static inline uint32_t phone_clock_update(phone_clock_t *clock, uint32_t remaining,
                                          uint32_t frames) {
    clock->fill_q8 += ((int32_t)remaining * 256 - clock->fill_q8) * (int32_t)frames / (480 * 32);
    if (clock->fill_q8 < clock->target_q8 - 120 * 256) return 0x10000u - 32u;
    if (clock->fill_q8 > clock->target_q8 + 120 * 256) return 0x10000u + 32u;
    return 0x10000u;
}
#endif
