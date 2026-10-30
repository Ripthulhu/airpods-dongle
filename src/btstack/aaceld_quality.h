// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_AACELD_QUALITY_H
#define AIRPODS_AACELD_QUALITY_H

#include <stdint.h>

#ifndef AACELD_CBR_BANDWIDTH
#define AACELD_CBR_BANDWIDTH 20000
#endif
#if AACELD_CBR_BANDWIDTH != 0 && AACELD_CBR_BANDWIDTH != 16000 && \
    AACELD_CBR_BANDWIDTH != 18000 && AACELD_CBR_BANDWIDTH != 20000
#error Unsupported AAC-ELD CBR bandwidth
#endif

// Keep the existing VBR ceiling policy; CBR is explicit for controlled comparisons.
static inline uint32_t aaceld_bandwidth(uint32_t ceiling, uint8_t rate_mode) {
    return rate_mode ? (ceiling <= 128000 ? 15500u : ceiling <= 160000 ? 17500u : 20000u)
                     : AACELD_CBR_BANDWIDTH;
}

#endif
