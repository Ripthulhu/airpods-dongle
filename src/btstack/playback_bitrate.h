// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PLAYBACK_BITRATE_H
#define AIRPODS_PLAYBACK_BITRATE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { PLAYBACK_RATE_OFF, PLAYBACK_RATE_STARTUP, PLAYBACK_RATE_CLEAN,
       PLAYBACK_RATE_PRESSURE, PLAYBACK_RATE_RECOVER, PLAYBACK_RATE_FROZEN, PLAYBACK_RATE_FAULT };
typedef struct {
    uint32_t ceiling, current, target;
    uint32_t window_since, stable_since, clean_since, last_change;
    uint32_t last_flushes, last_pressure, window_flushes, window_pressure, window_elapsed;
    uint32_t changes, errors;
    uint8_t bad_windows, reason;
    bool active, frozen, fault, variable, urgent;
} playback_bitrate_t;

static inline void playback_bitrate_reset(playback_bitrate_t *s, uint32_t ceiling, bool variable) {
    memset(s, 0, sizeof(*s));
    s->ceiling = s->current = s->target = ceiling;
    s->variable = variable;
    s->frozen = true;
}
static inline uint32_t playback_bitrate_rung(const playback_bitrate_t *s, bool increase) {
    static const uint32_t rates[] = {128000, 174000, 192000, 220000, 256000};
    uint32_t result = increase ? s->ceiling : 128000;
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        uint32_t rate = s->variable ? rates[i] / 800 * 800 : rates[i];
        if (rate > s->ceiling) continue;
        if (increase && rate > s->current && rate < result) result = rate;
        if (!increase && rate < s->current && rate > result) result = rate;
    }
    return result;
}
static inline void playback_bitrate_stalled(playback_bitrate_t *s) {
    if (!s->ceiling || s->fault) return;
    uint32_t lower = playback_bitrate_rung(s, false);
    if (lower < s->current && (!s->urgent || lower < s->target)) {
        s->target = lower; s->urgent = true; s->reason = PLAYBACK_RATE_PRESSURE;
    }
}
static inline void playback_bitrate_observe(playback_bitrate_t *s, uint32_t now,
                                           bool eligible, uint32_t flushes, uint32_t pressure) {
    if (!s->ceiling) return;
    if (!eligible || s->fault) {
        s->active = false; s->frozen = true; s->bad_windows = 0;
        if (!s->urgent || s->target >= s->current || s->fault) s->target = s->current;
        s->stable_since = s->clean_since = s->window_since = now;
        s->last_flushes = flushes; s->last_pressure = pressure;
        s->reason = s->fault ? PLAYBACK_RATE_FAULT : s->urgent ? PLAYBACK_RATE_PRESSURE : PLAYBACK_RATE_FROZEN;
        return;
    }
    if (!s->active) {
        s->active = true; s->frozen = true;
        s->stable_since = s->clean_since = s->window_since = now;
    }
    if (s->frozen) {
        s->last_flushes = flushes; s->last_pressure = pressure;
        s->window_since = s->clean_since = now;
        s->reason = s->urgent ? PLAYBACK_RATE_PRESSURE : PLAYBACK_RATE_STARTUP;
        if ((uint32_t)(now - s->stable_since) >= 3000u) {
            s->frozen = false; s->reason = PLAYBACK_RATE_CLEAN;
        }
        return;
    }
    uint32_t elapsed = now - s->window_since;
    if (elapsed < 1000u || s->target != s->current) return;
    s->window_elapsed = elapsed;
    s->window_flushes = flushes - s->last_flushes;
    s->window_pressure = pressure - s->last_pressure;
    s->last_flushes = flushes; s->last_pressure = pressure; s->window_since = now;
    if (elapsed > 2500u) { s->bad_windows = 0; s->clean_since = now; return; }
    if (s->window_flushes || s->window_pressure >= 2u) {
        s->clean_since = now;
        if (s->bad_windows < 2u) ++s->bad_windows;
        if (s->window_flushes >= 3u || s->bad_windows >= 2u) {
            s->target = playback_bitrate_rung(s, false);
            s->reason = PLAYBACK_RATE_PRESSURE;
            s->bad_windows = 0;
        }
    } else {
        s->bad_windows = 0;
        if (s->current < s->ceiling && (uint32_t)(now - s->clean_since) >= 30000u) {
            s->target = playback_bitrate_rung(s, true);
            s->reason = PLAYBACK_RATE_RECOVER;
        }
    }
}
static inline void playback_bitrate_applied(playback_bitrate_t *s, uint32_t now, bool success) {
    s->urgent = false;
    if (success) {
        if (s->current != s->target) ++s->changes;
        s->current = s->target; s->last_change = s->clean_since = now;
    } else {
        ++s->errors; s->fault = true; s->target = s->current;
        s->reason = PLAYBACK_RATE_FAULT;
    }
}
#endif
