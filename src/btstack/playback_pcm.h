// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_DONGLE_PLAYBACK_PCM_H
#define AIRPODS_DONGLE_PLAYBACK_PCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// One bounded stereo pool shared by USB input and the encoder. Callers serialize it.
#define PLAYBACK_PCM_FRAMES 8192u
#define PLAYBACK_PRIMING_FRAMES 4800u
typedef struct {
    int16_t samples[PLAYBACK_PCM_FRAMES * 2];
    uint32_t read;
    uint32_t used;
    uint32_t capacity;
    uint32_t priming;
} playback_pcm_t;

static inline bool playback_pcm_reset(playback_pcm_t *pcm, uint32_t capacity,
                                      bool prime) {
    if (capacity == 0 || capacity > PLAYBACK_PCM_FRAMES ||
        (prime && capacity < PLAYBACK_PRIMING_FRAMES)) return false;
    pcm->read = pcm->used = 0;
    pcm->capacity = capacity;
    pcm->priming = prime ? PLAYBACK_PRIMING_FRAMES : 0;
    return true;
}

static inline void playback_pcm_push(playback_pcm_t *pcm, const int16_t *src,
                                     uint32_t frames) {
    if (pcm->capacity == 0 || src == NULL) return;
    // Overflow keeps the newest whole stereo frames, never stale audio after a stall.
    if (frames > pcm->capacity) {
        src += (size_t)(frames - pcm->capacity) * 2;
        frames = pcm->capacity;
    }
    if (frames > pcm->capacity - pcm->used) {
        uint32_t drop = frames - (pcm->capacity - pcm->used);
        pcm->read = (pcm->read + drop) % pcm->capacity;
        pcm->used -= drop;
    }
    uint32_t write = (pcm->read + pcm->used) % pcm->capacity;
    for (uint32_t i = 0; i < frames; ++i) {
        pcm->samples[write * 2] = src[i * 2];
        pcm->samples[write * 2 + 1] = src[i * 2 + 1];
        write = (write + 1) % pcm->capacity;
    }
    pcm->used += frames;
}

static inline uint32_t playback_pcm_read(playback_pcm_t *pcm, int16_t *dst,
                                         uint32_t frames) {
    memset(dst, 0, (size_t)frames * 2 * sizeof(*dst));
    uint32_t prefix = pcm->priming < frames ? pcm->priming : frames;
    pcm->priming -= prefix;
    uint32_t copied = frames - prefix;
    if (copied > pcm->used) copied = pcm->used;
    for (uint32_t i = 0; i < copied; ++i) {
        dst[(prefix + i) * 2] = pcm->samples[pcm->read * 2];
        dst[(prefix + i) * 2 + 1] = pcm->samples[pcm->read * 2 + 1];
        pcm->read = (pcm->read + 1) % pcm->capacity;
    }
    pcm->used -= copied;
    return copied;
}

#endif
