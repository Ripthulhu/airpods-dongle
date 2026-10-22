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
        pcm->read += drop;
        if (pcm->read >= pcm->capacity) pcm->read -= pcm->capacity;
        pcm->used -= drop;
    }
    uint32_t write = pcm->read + pcm->used;
    if (write >= pcm->capacity) write -= pcm->capacity;
    uint32_t first = pcm->capacity - write;
    if (first > frames) first = frames;
    memcpy(pcm->samples + write * 2, src, (size_t)first * 2 * sizeof(*src));
    if (frames > first)
        memcpy(pcm->samples, src + first * 2, (size_t)(frames - first) * 2 * sizeof(*src));
    pcm->used += frames;
}

static inline uint32_t playback_pcm_read(playback_pcm_t *pcm, int16_t *dst,
                                         uint32_t frames) {
    uint32_t prefix = pcm->priming < frames ? pcm->priming : frames;
    pcm->priming -= prefix;
    if (prefix) memset(dst, 0, (size_t)prefix * 2 * sizeof(*dst));
    uint32_t copied = frames - prefix;
    if (copied > pcm->used) copied = pcm->used;
    if (copied) {
        uint32_t first = pcm->capacity - pcm->read;
        if (first > copied) first = copied;
        memcpy(dst + prefix * 2, pcm->samples + pcm->read * 2,
               (size_t)first * 2 * sizeof(*dst));
        if (copied > first)
            memcpy(dst + (prefix + first) * 2, pcm->samples,
                   (size_t)(copied - first) * 2 * sizeof(*dst));
        pcm->read += copied;
        if (pcm->read >= pcm->capacity) pcm->read -= pcm->capacity;
    }
    uint32_t filled = prefix + copied;
    if (filled < frames)
        memset(dst + filled * 2, 0, (size_t)(frames - filled) * 2 * sizeof(*dst));
    pcm->used -= copied;
    return copied;
}

#endif
