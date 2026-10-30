// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_REALTIME_METRICS_H
#define AIRPODS_REALTIME_METRICS_H

#include <stdint.h>
#include <stdbool.h>

#define REALTIME_USB_SERVICE_BUDGET_US 500u
#define REALTIME_USB_IDLE_INTERVAL_US 4000u

// Pace idle passes without adding a millisecond to an already busy codec pass.
static inline uint32_t realtime_loop_idle_us(uint32_t started_us, uint32_t now_us,
                                            bool fully_idle) {
    uint32_t elapsed = now_us - started_us;
    uint32_t interval = fully_idle ? 5000u : 1000u;
    return elapsed < interval ? interval - elapsed : 0u;
}

// Single-writer counters. Read on the owner thread, or copy while that writer
// is excluded; volatile alone does not synchronize an IRQ or another core.
typedef struct {
    uint32_t count, max_us, overruns, last_started_us, max_gap_us, total_us;
} realtime_metric_t;

// Intervals must be shorter than one uint32_t timer period. UINT32_MAX disables
// the overrun threshold; an interval exactly equal to its budget is permitted.
static inline void realtime_metric_record(realtime_metric_t *metric,
                                         uint32_t started_us, uint32_t finished_us,
                                         uint32_t budget_us) {
    uint32_t elapsed = finished_us - started_us;
    metric->total_us += elapsed; // Modulo total: subtract snapshots over bounded intervals.
    if (metric->count != 0u) {
        uint32_t gap = started_us - metric->last_started_us;
        if (gap > metric->max_gap_us) metric->max_gap_us = gap;
    }
    metric->last_started_us = started_us;
    if (metric->count != UINT32_MAX) ++metric->count;
    if (elapsed > metric->max_us) metric->max_us = elapsed;
    if (elapsed > budget_us && metric->overruns != UINT32_MAX) ++metric->overruns;
}


#endif
