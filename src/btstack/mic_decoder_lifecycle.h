// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_MIC_DECODER_LIFECYCLE_H
#define AIRPODS_MIC_DECODER_LIFECYCLE_H

#include <stdbool.h>
#include "aacp_mic_dec.h"

typedef struct {
    bool start_pending;
    bool releasing;
} mic_decoder_lifecycle_t;

typedef enum {
    MIC_DECODER_WAITING,
    MIC_DECODER_READY,
    MIC_DECODER_FAILED,
} mic_decoder_prepare_t;

static inline void mic_decoder_lifecycle_request(mic_decoder_lifecycle_t *state,
                                                  bool wanted, bool ready) {
    state->start_pending = wanted;
    // A failed worker still owns its handle until core1 acknowledges detachment.
    if (!ready) state->releasing = true;
}

static inline void mic_decoder_lifecycle_release(mic_decoder_lifecycle_t *state,
                                                  bool yield_to_phone) {
    if (state->releasing || yield_to_phone)
        state->releasing = !aacp_mic_dec_release();
}

// Core0 thread under the Bluetooth lock; at most one allocation per USB request.
static inline mic_decoder_prepare_t mic_decoder_lifecycle_prepare(
    mic_decoder_lifecycle_t *state, bool wanted, bool phone_released) {
    if (!wanted) state->start_pending = false;
    if (!state->start_pending || state->releasing || !phone_released)
        return MIC_DECODER_WAITING;
    state->start_pending = false;
    return aacp_mic_dec_init() ? MIC_DECODER_READY : MIC_DECODER_FAILED;
}

#endif
