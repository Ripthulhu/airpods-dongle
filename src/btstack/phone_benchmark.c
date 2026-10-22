// SPDX-License-Identifier: GPL-3.0-only
#include "phone_benchmark.h"
#include "phone_worker.h"
#include "../../3rd-party/lhdcv5/lhdc48_bench_fixture.h"

#define PREPARE_TIMEOUT_US 100000u
#define FRAME_TIMEOUT_US 20000u
#define CLEANUP_TIMEOUT_US 100000u
#define WAIT_LIMIT 1000u
#define DECODE_LIMIT_US 4000u
#define PCM_FRAMES 240u

typedef struct { uint32_t started; unsigned polls; } wait_t;
typedef struct { bool received; uint16_t frames, activity[2]; } output_t;

// A broken/stalled clock must not turn a startup failure into an infinite wait.
static bool within(wait_t *wait, uint32_t (*clock_us)(void), uint32_t timeout) {
    return ++wait->polls <= WAIT_LIMIT && clock_us() - wait->started < timeout;
}

static void receive(const int16_t *pcm, uint16_t frames, void *context) {
    output_t *output = context;
    output->received = true;
    output->frames = pcm != NULL ? frames : 0;
    if (pcm == NULL || frames != PCM_FRAMES) return;
    for (unsigned i = 0; i < frames; ++i) {
        output->activity[0] |= (uint16_t)pcm[i * 2u];
        output->activity[1] |= (uint16_t)pcm[i * 2u + 1u];
    }
}

bool phone_benchmark_lhdc(uint32_t (*clock_us)(void), bool (*idle)(void),
                          phone_benchmark_result_t *result) {
    phone_benchmark_result_t measured = {0};
    uint16_t activity[2] = {0};
    bool passed = false;
    phone_codec_enable_lhdc(false);
    if (!phone_worker_released()) goto done;
    if (clock_us == NULL || idle == NULL) goto cleanup;
    uint32_t started = clock_us();
    if (started == 0 || !phone_worker_configure(PHONE_CODEC_LHDCV5, NULL)) goto cleanup;
    wait_t wait = {clock_us(), 0};
    for (;;) {
        if (!within(&wait, clock_us, PREPARE_TIMEOUT_US)) goto cleanup;
        phone_worker_status_t status = phone_worker_prepare();
        if (status == PHONE_WORKER_READY) break;
        if (status == PHONE_WORKER_FAILED || !idle()) goto cleanup;
    }
    for (unsigned i = 0; i < LHDC48_BENCH_FRAME_COUNT; ++i) {
        const uint8_t *frame = lhdc48_bench_data + i * LHDC48_BENCH_FRAME_BYTES;
        if (!phone_worker_submit(frame, LHDC48_BENCH_FRAME_BYTES, 1)) goto cleanup;
        output_t output = {0};
        wait = (wait_t){clock_us(), 0};
        while (!output.received) {
            if (!within(&wait, clock_us, FRAME_TIMEOUT_US) ||
                !phone_worker_poll(receive, &output)) goto cleanup;
            if (!output.received && !idle()) goto cleanup;
        }
        uint32_t elapsed = phone_worker_last_decode_us();
        if (elapsed > measured.max_decode_us) measured.max_decode_us = elapsed;
        if (output.frames != PCM_FRAMES) goto cleanup;
        activity[0] |= output.activity[0];
        activity[1] |= output.activity[1];
        ++measured.frames;
        if (elapsed == 0 || elapsed >= DECODE_LIMIT_US) goto cleanup;
    }
    // The synthetic music contains both channels; a muted decoder is not ready.
    passed = clock_us() != started && activity[0] != 0 && activity[1] != 0;

cleanup:
    measured.cleanup = phone_worker_close();
    if (!measured.cleanup && clock_us != NULL && idle != NULL) {
        wait_t closing = {clock_us(), 0};
        while (!measured.cleanup && within(&closing, clock_us, CLEANUP_TIMEOUT_US)) {
            (void)idle();
            measured.cleanup = phone_worker_close();
        }
    }
done:
    passed = passed && measured.cleanup;
    phone_codec_enable_lhdc(passed);
    if (result != NULL) *result = measured;
    return passed;
}
