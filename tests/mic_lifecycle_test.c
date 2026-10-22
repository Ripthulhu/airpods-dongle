// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/mic_decoder_lifecycle.h"

static bool ready, init_succeeds, release_ack = true;
static unsigned attempts, releases;

bool aacp_mic_dec_init(void) {
    ++attempts;
    ready = init_succeeds;
    return ready;
}
bool aacp_mic_dec_release(void) {
    ++releases;
    if (release_ack) ready = false;
    return release_ack;
}

static mic_decoder_prepare_t poll(mic_decoder_lifecycle_t *state, bool wanted,
                                   bool phone_connected, bool phone_released) {
    mic_decoder_lifecycle_release(state, phone_connected && !wanted);
    return mic_decoder_lifecycle_prepare(state, wanted, phone_released);
}

int main(void) {
    mic_decoder_lifecycle_t state = {0};
    assert(poll(&state, false, false, true) == MIC_DECODER_WAITING && attempts == 0);

    // A failed allocation consumes this request, not every subsequent main-loop tick.
    mic_decoder_lifecycle_request(&state, true, ready);
    assert(poll(&state, true, false, true) == MIC_DECODER_FAILED && attempts == 1);
    init_succeeds = true;
    for (unsigned i = 0; i < 1000; ++i)
        assert(poll(&state, true, false, true) == MIC_DECODER_WAITING);
    assert(attempts == 1);

    // Close/reopen authorizes a fresh attempt even when the previous open failed.
    mic_decoder_lifecycle_request(&state, false, ready);
    assert(poll(&state, false, false, true) == MIC_DECODER_WAITING);
    mic_decoder_lifecycle_request(&state, true, ready);
    assert(poll(&state, true, false, true) == MIC_DECODER_READY && attempts == 2);
    assert(poll(&state, true, false, true) == MIC_DECODER_WAITING && attempts == 2);

    // A worker failure must detach before its handle can be initialized again.
    ready = false;
    release_ack = false;
    mic_decoder_lifecycle_request(&state, true, ready);
    assert(poll(&state, true, false, true) == MIC_DECODER_WAITING && attempts == 2);
    assert(state.releasing);
    release_ack = true;
    assert(poll(&state, true, false, true) == MIC_DECODER_READY && attempts == 3);

    // Phone takeover retains its storage until detachment; closing cancels the pending open.
    release_ack = false;
    mic_decoder_lifecycle_release(&state, true);
    mic_decoder_lifecycle_request(&state, true, false);
    assert(poll(&state, true, true, false) == MIC_DECODER_WAITING && attempts == 3);
    mic_decoder_lifecycle_request(&state, false, false);
    release_ack = true;
    assert(poll(&state, false, true, true) == MIC_DECODER_WAITING && attempts == 3);
    assert(!state.start_pending && !state.releasing);

    // A new open may wait for phone storage; it still gets only one attempt.
    mic_decoder_lifecycle_request(&state, true, false);
    assert(poll(&state, true, true, false) == MIC_DECODER_WAITING && attempts == 3);
    assert(poll(&state, true, false, true) == MIC_DECODER_READY && attempts == 4);
    assert(releases > 0);
    puts("microphone lifecycle: bounded failed allocation, fresh-open recovery, detach and cancellation passed");
}
