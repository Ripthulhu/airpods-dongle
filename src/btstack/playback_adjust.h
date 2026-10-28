// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PLAYBACK_ADJUST_H
#define AIRPODS_PLAYBACK_ADJUST_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool pending, running, failed;
    uint8_t interval;
    uint16_t target;
    uint32_t goal_frames, started, sent_frames;
} playback_adjust_t;

static inline bool playback_adjust_busy(const playback_adjust_t *s) {
    return s->pending || s->running;
}
static inline void playback_adjust_cancel(playback_adjust_t *s) {
    *s = (playback_adjust_t){0};
}
static inline uint8_t playback_adjust_interval(const playback_adjust_t *s) {
    return s->running ? s->interval : 10;
}
// Match the captured finite +/-10% interval change, with 10 ms of entry headroom.
static inline void playback_adjust_request(playback_adjust_t *s, uint16_t delay,
                                            uint16_t target) {
    playback_adjust_cancel(s);
    if (!delay || delay == UINT16_MAX) return;
    uint32_t ms = delay / 10u;
    uint32_t drift = ms > target ? ms - target : target - ms;
    if (target != 150 && drift > 10) drift -= 10;
    if (!drift) return;
    s->pending = true; s->target = target;
    s->interval = ms > target ? 11 : 9;
    s->goal_frames = drift * 48u;
}
static inline void playback_adjust_start(playback_adjust_t *s, uint32_t now) {
    if (!s->pending) return;
    s->pending = false; s->running = true;
    s->started = now; s->sent_frames = 0;
}
static inline void playback_adjust_poll(playback_adjust_t *s, uint32_t now) {
    if (s->running && (uint32_t)(now - s->started) >= 10000) {
        s->running = false; s->failed = true;
    }
}
static inline void playback_adjust_sent(playback_adjust_t *s, uint32_t frames,
                                         uint32_t now) {
    if (!s->running) return;
    playback_adjust_poll(s, now);
    if (!s->running) return;
    s->sent_frames += frames;
    int32_t drift = (int32_t)((uint32_t)(now - s->started) * 48u) -
                    (int32_t)s->sent_frames;
    if (s->interval == 9) drift = -drift;
    if (drift >= (int32_t)s->goal_frames) s->running = false;
}
// A common denominator preserves fractional frame debt when the interval changes.
static inline uint32_t playback_adjust_samples(uint8_t interval, uint32_t elapsed,
                                                uint32_t rate, uint32_t *remainder) {
    uint32_t scale = interval == 11 ? 90 : interval == 9 ? 110 : 99;
    uint64_t numerator = (uint64_t)elapsed * rate * scale + *remainder;
    *remainder = (uint32_t)(numerator % 99000u);
    return (uint32_t)(numerator / 99000u);
}
#endif
