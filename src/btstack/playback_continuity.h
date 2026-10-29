// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PLAYBACK_CONTINUITY_H
#define AIRPODS_PLAYBACK_CONTINUITY_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t last_us, deficit_us;
    bool armed;
} playback_continuity_t;

static inline void playback_continuity_reset(playback_continuity_t *state) {
    *state = (playback_continuity_t){0};
}

// Short stalls can cumulatively exhaust the receiver even without one long gap.
// Extra packets repay the deficit; timely steady playback cannot repay lost time.
static inline bool playback_continuity_sent(playback_continuity_t *state,
    uint32_t now_us, uint32_t packet_us, uint32_t reserve_us) {
    if (!state->armed) {
        state->armed = true;
        state->last_us = now_us;
        return false;
    }
    uint64_t elapsed = (uint64_t)state->deficit_us + (uint32_t)(now_us - state->last_us);
    state->last_us = now_us;
    elapsed = elapsed > packet_us ? elapsed - packet_us : 0;
    state->deficit_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    return state->deficit_us > reserve_us;
}
#endif
