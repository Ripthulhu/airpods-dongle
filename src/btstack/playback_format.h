// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_DONGLE_PLAYBACK_FORMAT_H
#define AIRPODS_DONGLE_PLAYBACK_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

static inline bool playback_format_supported(uint32_t sample_rate, uint32_t channels) {
    return sample_rate == 48000 && channels == 2;
}

static inline uint16_t playback_aac_frame_samples(uint32_t sample_rate, uint32_t channels,
                                                uint32_t frame_samples, bool eld) {
    return playback_format_supported(sample_rate, channels) &&
           frame_samples == (eld ? 480u : 1024u) ? (uint16_t)frame_samples : 0;
}

#endif
