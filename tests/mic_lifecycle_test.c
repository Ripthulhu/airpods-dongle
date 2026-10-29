// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/mic_decoder_lifecycle.h"
#include "../src/tinyusb/mic_request.h"

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


static usb_mic_request_t usb_request;
static unsigned request_notifications, stopped;
static bool usb_mic_take_request(bool *wanted) {return usb_mic_request_take(&usb_request,wanted);}
bool aacp_mic_dec_ready(void) {return ready;}
static void dongle_playback_mic_request_changed(void) {++request_notifications;}
static void aacp_mic_stop(void) {++stopped;}
static bool aacp_mic_requested(void) {return usb_mic_request_wanted(&usb_request);}
static bool aacp_mic_update_owner(bool changed) {
    if(changed) {dongle_playback_mic_request_changed();aacp_mic_stop();}
    return changed;
}
#include "main_mic_request.inc"
static bool microphone_active, phone_connected;
static unsigned phone_yields, phone_polls;
static bool aacp_mic_active(void) {return microphone_active;}
static bool phone_sink_connected(void) {return phone_connected;}
static void phone_sink_yield(void) {++phone_yields;}
static void phone_sink_poll(void) {++phone_polls;}
enum { WATCH_PHONE };
static void runtime_watch_set(unsigned phase) {assert(phase==WATCH_PHONE);}
#include "main_phone_handover.inc"

static void closed_microphone_drains_before_phone_prepare(void) {
    mic_decoder_lifecycle_t state={0};ready=true;
    usb_mic_request_set(&usb_request,false);
    phone_connected=microphone_active=true;
    unsigned before=releases;
    main_phone_handover(&state);
    assert(!phone_yields && releases==before && phone_polls==1 && ready);
    microphone_active=false;release_ack=false;
    main_phone_handover(&state);
    assert(state.releasing && !phone_yields && phone_polls==1 && ready);
    release_ack=true;main_phone_handover(&state);
    assert(!state.releasing && !ready && phone_polls==2 && !phone_yields);
    ready=true;microphone_active=true;usb_mic_request_set(&usb_request,true);
    before=releases;main_phone_handover(&state);
    assert(phone_yields==1 && releases==before && ready); // A current USB call still wins.
}
static void coalesced_request_reaches_playback(void) {
    mic_decoder_lifecycle_t state={0}; ready=true;
    usb_mic_request_set(&usb_request,true); main_mic_request(&state);
    assert(request_notifications==1 && stopped==1 && state.start_pending);
    state.start_pending=false;
    usb_mic_request_set(&usb_request,false); usb_mic_request_set(&usb_request,true);
    main_mic_request(&state);
    assert(request_notifications==2 && stopped==2 && state.start_pending);
    main_mic_request(&state); assert(request_notifications==2 && stopped==2);
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
    coalesced_request_reaches_playback();
    closed_microphone_drains_before_phone_prepare();
    puts("microphone lifecycle: bounded failed allocation, fresh-open recovery, detach and cancellation passed");
}
