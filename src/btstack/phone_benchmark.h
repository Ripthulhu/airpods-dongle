// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PHONE_BENCHMARK_H
#define AIRPODS_PHONE_BENCHMARK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t max_decode_us;
    uint16_t frames;
    bool cleanup;
} phone_benchmark_result_t;

// Core0 startup only, after core1 starts and before Bluetooth/audio. idle services
// USB and yields to core1; false cancels. Cleanup still services idle until detach.
// cleanup=false requires aborting startup: a busy worker was deliberately not freed.
bool phone_benchmark_lhdc(uint32_t (*clock_us)(void), bool (*idle)(void),
                          phone_benchmark_result_t *result);

#endif
