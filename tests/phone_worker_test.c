// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/btstack/phone_worker.h"

static pthread_t controller;
static bool fail_allocation;
static unsigned allocations, frees;
static atomic_bool codec_busy, hold_decode, decode_entered, quit_worker;
static bool codec_configured;
static unsigned delivered, callbacks;
static int16_t expected_sample;
static uint32_t fake_time;
#define PHONE_WORKER_TIME_US() fake_time

static void *test_calloc(size_t count, size_t size) {
    assert(pthread_equal(pthread_self(), controller));
    if (fail_allocation) return NULL;
    ++allocations;
    return calloc(count, size);
}
static void test_free(void *pointer) {
    assert(pthread_equal(pthread_self(), controller));
    assert(pointer != NULL);
    ++frees;
    free(pointer);
}
#define calloc test_calloc
#define free test_free
#include "../src/btstack/phone_worker.c"
#undef calloc
#undef free

void phone_worker_wake(void) {}
bool phone_codec_configuration_valid(phone_codec_t codec, const phone_sbc_config_t *sbc) {
    return codec == PHONE_CODEC_APTX || (codec == PHONE_CODEC_SBC && sbc != NULL);
}
bool phone_codec_configure(phone_codec_t codec, const phone_sbc_config_t *sbc) {
    assert(pthread_equal(pthread_self(), controller));
    assert(!atomic_load(&codec_busy));
    codec_configured = phone_codec_configuration_valid(codec, sbc);
    return codec_configured;
}
bool phone_codec_prepare(void) {
    assert(pthread_equal(pthread_self(), controller));
    return codec_configured;
}
void phone_codec_close(void) {
    assert(pthread_equal(pthread_self(), controller));
    assert(!atomic_load(&codec_busy));
    codec_configured = false;
}
void phone_codec_reset(void) {
    assert(!atomic_exchange(&codec_busy, true));
    assert(codec_configured);
    atomic_store(&codec_busy, false);
}
bool phone_codec_decode(const uint8_t *payload, size_t length, uint8_t count,
                        phone_codec_pcm_callback callback, void *context) {
    (void)count;
    fake_time += 1250u;
    assert(!atomic_exchange(&codec_busy, true));
    assert(codec_configured && length != 0);
    atomic_store(&decode_entered, true);
    while (atomic_load(&hold_decode)) sched_yield();
    int16_t pcm[1024];
    for (unsigned i = 0; i < 1024; ++i) pcm[i] = payload[0];
    callback(pcm, 4, context);
    if (payload[0] == 0xfe) {
        // A faulty decoder must not overrun the output mailbox.
        for (unsigned i = 0; i < 4; ++i) callback(pcm, 512, context);
    }
    atomic_store(&codec_busy, false);
    return payload[0] != 0xff; // Failure after producing a partial prefix.
}

static void receive(const int16_t *pcm, uint16_t frames, void *context) {
    assert(pthread_equal(pthread_self(), controller));
    assert(context == &delivered);
    for (unsigned i = 0; i < frames * 2u; ++i) assert(pcm[i] == expected_sample);
    delivered += frames;
    ++callbacks;
}
static void start(void) {
    assert(phone_worker_configure(PHONE_CODEC_APTX, NULL));
    assert(phone_worker_prepare() == PHONE_WORKER_READY);
    assert(phone_worker_ready() && !phone_worker_released());
}
static void stop(void) {
    assert(!phone_worker_close());
    assert(!phone_worker_ready() && !phone_worker_released());
    assert(phone_worker_step());
    assert(phone_worker_close());
    assert(phone_worker_released());
    assert(!phone_worker_step());
}
static void deterministic(void) {
    fail_allocation = true;
    assert(phone_worker_configure(PHONE_CODEC_APTX, NULL));
    assert(phone_worker_prepare() == PHONE_WORKER_FAILED);
    assert(phone_worker_released());
    fail_allocation = false;
    start();
    uint8_t data[] = {7, 0, 0, 0};
    assert(!phone_worker_submit(NULL, sizeof(data), 0));
    assert(!phone_worker_submit(data, 0, 0));
    assert(!phone_worker_submit(data, PHONE_CODEC_MAX_PAYLOAD + 1u, 0));
    assert(phone_worker_submit(data, sizeof(data), 0));
    assert(phone_worker_submit(data, sizeof(data), 0));
    assert(!phone_worker_submit(data, sizeof(data), 0));
    assert(phone_worker_step());
    assert(!phone_worker_step()); // Main-loop backpressure, no decode spinning.
    phone_worker_reset();
    assert(phone_worker_poll(receive, &delivered)); // Stale mailbox discarded.
    assert(delivered == 0);
    assert(phone_worker_step()); // Stale queued input discarded.
    assert(!phone_worker_step());
    expected_sample = 7;
    assert(phone_worker_submit(data, sizeof(data), 0));
    data[0] = 10; // Input is copied before the Bluetooth callback returns.
    assert(phone_worker_step());
    assert(phone_worker_poll(receive, &delivered));
    assert(delivered == 4 && callbacks == 1);
    assert(phone_worker_last_decode_us() == 1250u);
    for (unsigned bad = 0xfe; bad <= 0xff; ++bad) {
        data[0] = bad;
        assert(phone_worker_submit(data, sizeof(data), 0));
        assert(phone_worker_step());
        assert(!phone_worker_poll(receive, &delivered));
        assert(delivered == 4 && callbacks == 1);
        phone_worker_reset();
    }
    // Configuration changes wait for worker detachment; they cannot free or
    // reset a codec underneath its decode operation.
    assert(phone_worker_configure(PHONE_CODEC_APTX, NULL));
    assert(phone_worker_prepare() == PHONE_WORKER_WAITING);
    assert(phone_worker_step());
    assert(phone_worker_prepare() == PHONE_WORKER_READY);
    stop();
    assert(allocations == frees);
}

static void *thread_entry(void *unused) {
    (void)unused;
    while (!atomic_load(&quit_worker)) if (!phone_worker_step()) sched_yield();
    return NULL;
}
static void threaded(void) {
    pthread_t worker;
    assert(pthread_create(&worker, NULL, thread_entry, NULL) == 0);
    uint8_t data[] = {12, 0, 0, 0};
    start();
    atomic_store(&hold_decode, true);
    atomic_store(&decode_entered, false);
    assert(phone_worker_submit(data, sizeof(data), 0));
    while (!atomic_load(&decode_entered)) sched_yield();
    phone_worker_reset(); // Cancel an in-flight decode without touching codec.
    assert(!phone_worker_close());
    unsigned before = frees;
    for (unsigned i = 0; i < 1000; ++i) assert(!phone_worker_close());
    assert(frees == before);
    atomic_store(&hold_decode, false);
    while (!phone_worker_close()) sched_yield();
    assert(phone_worker_released());

    expected_sample = 12;
    for (unsigned cycle = 0; cycle < 1000; ++cycle) {
        start();
        for (unsigned packet = 0; packet < 20; ++packet) {
            if (!phone_worker_submit(data, sizeof(data), 0)) phone_worker_reset();
            if (packet % 3u == 0) phone_worker_reset();
            assert(phone_worker_poll(receive, &delivered));
            sched_yield();
        }
        while (!phone_worker_close()) sched_yield();
    }
    atomic_store(&quit_worker, true);
    assert(pthread_join(worker, NULL) == 0);
    assert(allocations == frees);
}

int main(void) {
    controller = pthread_self();
    deterministic();
    threaded();
    printf("phone worker: bounds, cancellation, partial failure, reconfigure, threaded release passed; %zu-byte phone-only storage\n", sizeof(worker_t));
}
