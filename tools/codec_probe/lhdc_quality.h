// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PROBE_LHDC_QUALITY_H
#define AIRPODS_PROBE_LHDC_QUALITY_H
#include <stdbool.h>
#include <stdint.h>
#include "../../tests/lhdc/music_reference.h"

#define LHDC_QUALITY_PAIRS (sizeof(lhdc_music_reference) / sizeof(*lhdc_music_reference) / 2u)

typedef struct {
    unsigned samples, compared;
    int64_t signal[2], error[2], cross[2];
} lhdc_quality_t;

static inline void lhdc_quality_collect(lhdc_quality_t *quality, const int16_t *pcm,
                                        unsigned frames) {
    for (unsigned n = 0; n < frames; ++n, ++quality->samples) {
        // Skip encoder warm-up and align the independently retained source PCM.
        if (quality->samples < 960u + 168u || (quality->samples - 960u - 168u) % 8u) continue;
        unsigned index = (quality->samples - 960u - 168u) / 8u;
        if (index >= LHDC_QUALITY_PAIRS) continue;
        for (unsigned c = 0; c < 2; ++c) {
            int64_t expected = lhdc_music_reference[index * 2u + c];
            int64_t actual = pcm[n * 2u + c];
            quality->signal[c] += expected * expected;
            quality->error[c] += (actual - expected) * (actual - expected);
            quality->cross[c] += actual * expected;
        }
        ++quality->compared;
    }
}

static inline bool lhdc_quality_passed(const lhdc_quality_t *quality) {
    if (quality->compared != LHDC_QUALITY_PAIRS) return false;
    for (unsigned c = 0; c < 2; ++c) {
        // SNR > 30 dB and source-relative gain within 0.5%, without floating math.
        if (quality->signal[c] <= 0 || quality->error[c] * 1000 >= quality->signal[c] ||
            quality->cross[c] * 1000 < quality->signal[c] * 995 ||
            quality->cross[c] * 1000 > quality->signal[c] * 1005) return false;
    }
    return true;
}
#endif
