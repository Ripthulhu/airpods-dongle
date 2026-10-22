// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/phone_benchmark.h"
#include "../src/btstack/phone_worker.h"
#include "../3rd-party/lhdcv5/lhdc48_bench_fixture.h"
#include "../src/btstack/phone_benchmark.c"

static bool enabled, allocated, pending, output_ready, detach, acknowledged;
static bool fail_configuration, fail_prepare, fail_submit, fail_decode, stall_decode, stall_detach;
static bool zero_clock, frozen_clock, cancel, wrong_frames;
static unsigned submitted, delivered, idle_calls, closes, prepare_waits;
static uint32_t now, decode_us;
static unsigned silent_channels;

void phone_codec_enable_lhdc(bool value) {
    if (value) assert(!allocated && acknowledged && delivered == LHDC48_BENCH_FRAME_COUNT);
    enabled = value;
}
bool phone_worker_released(void) { return !allocated; }
bool phone_worker_configure(phone_codec_t codec, const phone_sbc_config_t *sbc) {
    assert(codec == PHONE_CODEC_LHDCV5 && sbc == NULL && !enabled && !allocated);
    return !fail_configuration;
}
phone_worker_status_t phone_worker_prepare(void) {
    assert(!enabled);
    if (prepare_waits) { --prepare_waits; return PHONE_WORKER_WAITING; }
    if (fail_prepare) return PHONE_WORKER_FAILED;
    allocated = true;
    return PHONE_WORKER_READY;
}
bool phone_worker_submit(const uint8_t *payload, size_t length, uint8_t frames) {
    assert(allocated && !enabled && !pending && !output_ready && !detach);
    assert(length == LHDC48_BENCH_FRAME_BYTES && frames == 1);
    assert(submitted < LHDC48_BENCH_FRAME_COUNT);
    for (unsigned i = 0; i < length; ++i)
        assert(payload[i] == lhdc48_bench_data[submitted * length + i]);
    if (fail_submit) return false;
    ++submitted;
    pending = true;
    return true;
}
bool phone_worker_poll(phone_codec_pcm_callback callback, void *context) {
    assert(allocated && !detach && !enabled);
    if (!output_ready) return true;
    output_ready = false;
    if (fail_decode) return false;
    int16_t pcm[480] = {0};
    if (!(silent_channels & 1u)) pcm[0] = 100;
    if (!(silent_channels & 2u)) pcm[1] = -100;
    callback(pcm, wrong_frames ? 239 : 240, context);
    ++delivered;
    return true;
}
uint32_t phone_worker_last_decode_us(void) { return decode_us; }
bool phone_worker_close(void) {
    assert(!enabled);
    ++closes;
    if (!allocated) return true;
    detach = true;
    if (!acknowledged) return false;
    assert(!pending); // Never free while the simulated core1 is decoding.
    allocated = false;
    return true;
}
static uint32_t clock_us(void) { return zero_clock ? 0 : frozen_clock ? 100 : now; }
static bool idle(void) {
    ++idle_calls;
    now += 1000;
    if (detach) {
        if (!stall_detach) { pending = false; output_ready = false; acknowledged = true; }
    } else if (pending && !stall_decode) {
        pending = false;
        output_ready = true;
    }
    return !cancel;
}
static void reset(void) {
    enabled = true; // Entry must revoke any prior successful benchmark.
    allocated = pending = output_ready = detach = acknowledged = false;
    fail_configuration = fail_prepare = fail_submit = fail_decode = stall_decode = stall_detach = false;
    zero_clock = frozen_clock = cancel = wrong_frames = false;
    submitted = delivered = idle_calls = closes = prepare_waits = 0;
    now = 100;
    decode_us = 1250;
    silent_channels = 0;
}
static phone_benchmark_result_t run(bool expected) {
    phone_benchmark_result_t result = {9999, 999, false};
    assert(phone_benchmark_lhdc(clock_us, idle, &result) == expected);
    assert(enabled == expected && idle_calls <= 2 * 1000 + LHDC48_BENCH_FRAME_COUNT);
    return result;
}
int main(void) {
    reset(); prepare_waits = 2;
    phone_benchmark_result_t result = run(true);
    assert(result.cleanup && !allocated && result.frames == LHDC48_BENCH_FRAME_COUNT);
    assert(result.max_decode_us == 1250 && submitted == delivered && closes >= 2);
    for (unsigned muted = 1; muted <= 3; ++muted) {
        reset(); silent_channels = muted;
        result = run(false);
        assert(result.cleanup && result.frames == LHDC48_BENCH_FRAME_COUNT && !allocated);
    }
    reset(); decode_us = 3999;
    assert(run(true).max_decode_us == 3999);
    reset(); decode_us = 4000;
    result = run(false);
    assert(result.cleanup && result.frames == 1 && result.max_decode_us == 4000 && !allocated);
    reset(); decode_us = 0;
    result = run(false);
    assert(result.cleanup && result.frames == 1 && result.max_decode_us == 0);
    reset(); zero_clock = true;
    result = run(false);
    assert(result.cleanup && !submitted && !allocated);
    reset(); frozen_clock = true;
    result = run(false);
    assert(result.cleanup && result.frames == LHDC48_BENCH_FRAME_COUNT);
    reset(); fail_configuration = true;
    assert(run(false).cleanup && !allocated);
    reset(); fail_prepare = true;
    assert(run(false).cleanup && !allocated);
    reset(); fail_submit = true;
    assert(run(false).cleanup && !submitted && !allocated);
    reset(); fail_decode = true;
    result = run(false);
    assert(result.cleanup && !result.frames && !allocated);
    reset(); wrong_frames = true;
    result = run(false);
    assert(result.cleanup && !result.frames && !allocated);
    reset(); stall_decode = true;
    result = run(false);
    assert(result.cleanup && !result.frames && !allocated && idle_calls >= 20);
    reset(); cancel = true; stall_decode = true;
    result = run(false);
    assert(result.cleanup && !result.frames && !allocated && acknowledged);
    reset(); stall_decode = stall_detach = true;
    result = run(false);
    assert(!result.cleanup && allocated && pending && detach && !acknowledged);
    reset(); frozen_clock = stall_decode = stall_detach = true;
    result = run(false);
    assert(!result.cleanup && allocated && pending && idle_calls == 2000);
    reset(); prepare_waits = 1000;
    result = run(false);
    assert(result.cleanup && !submitted && !allocated && idle_calls == 100);
    reset(); allocated = true;
    result = run(false);
    assert(!result.cleanup && !closes && !submitted); // Do not take another worker's ownership.
    reset();
    assert(!phone_benchmark_lhdc(NULL, idle, &result) && result.cleanup && !enabled);
    reset();
    assert(!phone_benchmark_lhdc(clock_us, NULL, &result) && result.cleanup && !enabled);
    reset();
    assert(phone_benchmark_lhdc(clock_us, idle, NULL) && enabled && !allocated);
    puts("phone benchmark: full frames, strict actual timing, bounded waits, cancellation and safe detach passed");
}
