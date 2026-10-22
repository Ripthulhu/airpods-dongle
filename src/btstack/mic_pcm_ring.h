// SPDX-License-Identifier: GPL-3.0-only
#ifndef MIC_PCM_RING_H
#define MIC_PCM_RING_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define MIC_RING_SAMPLES 8192u
#define MIC_RING_MASK (MIC_RING_SAMPLES - 1u)

typedef struct {
    int16_t samples[MIC_RING_SAMPLES];
    _Atomic uint32_t written;
    _Atomic uint32_t read;
    _Atomic uint32_t reset;
    uint32_t seen_reset;  // only the consumer writes this and read
} mic_pcm_ring_t;

static inline uint32_t mic_pcm_ring_write(mic_pcm_ring_t *ring, const int16_t *src,
                                          uint32_t count) {
    uint32_t w = atomic_load_explicit(&ring->written, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&ring->read, memory_order_acquire);
    uint32_t space = MIC_RING_SAMPLES - (w - r);
    if (count > space) count = space;
    uint32_t at = w & MIC_RING_MASK;
    uint32_t first = MIC_RING_SAMPLES - at;
    if (first > count) first = count;
    if (first != 0) memcpy(ring->samples + at, src, first * sizeof(*src));
    if (count > first) memcpy(ring->samples, src + first, (count - first) * sizeof(*src));
    atomic_store_explicit(&ring->written, w + count, memory_order_release);
    return count;
}

// Reset never writes the consumer's cursor: an interrupt must not roll it back.
static inline void mic_pcm_ring_reset(mic_pcm_ring_t *ring) {
    atomic_fetch_add_explicit(&ring->reset, 1, memory_order_release);
}

static inline uint32_t mic_pcm_ring_read(mic_pcm_ring_t *ring, int16_t *dst,
                                         uint32_t count) {
    uint32_t reset = atomic_load_explicit(&ring->reset, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&ring->read, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&ring->written, memory_order_acquire);
    if (ring->seen_reset != reset) {
        ring->seen_reset = reset;
        atomic_store_explicit(&ring->read, w, memory_order_release);
        return 0;
    }
    uint32_t available = w - r;
    if (count > available) count = available;
    uint32_t at = r & MIC_RING_MASK;
    uint32_t first = MIC_RING_SAMPLES - at;
    if (first > count) first = count;
    if (first != 0) memcpy(dst, ring->samples + at, first * sizeof(*dst));
    if (count > first) memcpy(dst + first, ring->samples, (count - first) * sizeof(*dst));
    uint32_t after = atomic_load_explicit(&ring->reset, memory_order_acquire);
    if (after != reset) {
        ring->seen_reset = after;
        r = atomic_load_explicit(&ring->written, memory_order_acquire);
        count = 0;
    } else {
        r += count;
    }
    atomic_store_explicit(&ring->read, r, memory_order_release);
    return count;
}

#endif
