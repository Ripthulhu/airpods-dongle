// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PLAYBACK_DUCK_H
#define AIRPODS_PLAYBACK_DUCK_H

#include <stdbool.h>
#include <stdint.h>
#include "playback_pcm.h"

#define PLAYBACK_DUCK_UNITY (INT32_C(1) << 30)
#define PLAYBACK_DUCK_ATTENUATED (PLAYBACK_DUCK_UNITY / 4)

typedef struct {
    int32_t gain, target, step;
    uint32_t remaining, duration, remainder, error, rate;
} playback_duck_t;

static inline void playback_duck_reset(playback_duck_t *s) {
    *s = (playback_duck_t){.gain=PLAYBACK_DUCK_UNITY, .target=PLAYBACK_DUCK_UNITY};
}

static inline void playback_duck_target(playback_duck_t *s, bool duck, uint32_t rate) {
    int32_t target = duck ? PLAYBACK_DUCK_ATTENUATED : PLAYBACK_DUCK_UNITY;
    if (target == s->target && (!s->remaining || rate == s->rate)) return;
    uint32_t duration = (uint32_t)((uint64_t)rate * (target < s->gain ? 150u : 500u) / 1000u);
    if (!duration) duration = 1;
    int32_t delta = target - s->gain;
    s->target = target;
    s->rate = rate;
    s->duration = s->remaining = duration;
    s->step = delta / (int32_t)duration;
    s->remainder = (uint32_t)(delta < 0 ? -delta : delta) % duration;
    s->error = 0;
}

static inline int32_t playback_duck_next(playback_duck_t *s) {
    if (s->remaining) {
        // Distribute the division remainder across frames, with no per-sample division.
        s->gain += s->step;
        s->error += s->remainder;
        if (s->error >= s->duration) {
            s->error -= s->duration;
            s->gain += s->target < s->gain ? -1 : 1;
        }
        if (!--s->remaining) s->gain = s->target;
    }
    return s->gain;
}

static inline int32_t playback_duck_sample(int32_t value, int32_t gain) {
    int64_t product = (int64_t)value * gain;
    return (int32_t)(product < 0 ? -((-product + (INT64_C(1) << 29)) >> 30) :
                                 (product + (INT64_C(1) << 29)) >> 30);
}

static inline void playback_duck_q31(playback_duck_t *s, playback_q31_t *samples,
                                      uint32_t frames, uint32_t rate, bool duck) {
    if (!samples || !frames || !rate) return;
    playback_duck_target(s, duck, rate);
    if (s->gain == PLAYBACK_DUCK_UNITY && !s->remaining) return;
    for (uint32_t frame=0; frame<frames; ++frame) {
        int32_t gain = playback_duck_next(s);
        samples[2u*frame] = playback_duck_sample(samples[2u*frame], gain);
        samples[2u*frame+1] = playback_duck_sample(samples[2u*frame+1], gain);
    }
}

static inline void playback_duck_s16(playback_duck_t *s, int16_t *samples,
                                      uint32_t frames, uint32_t rate, bool duck) {
    if (!samples || !frames || !rate) return;
    playback_duck_target(s, duck, rate);
    if (s->gain == PLAYBACK_DUCK_UNITY && !s->remaining) return;
    for (uint32_t frame=0; frame<frames; ++frame) {
        int32_t gain = playback_duck_next(s);
        samples[2u*frame] = (int16_t)playback_duck_sample(samples[2u*frame], gain);
        samples[2u*frame+1] = (int16_t)playback_duck_sample(samples[2u*frame+1], gain);
    }
}

#endif
