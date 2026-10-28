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
static unsigned delivered, callbacks, codec_resets;
static int16_t expected_sample;
static uint32_t fake_time;
static uint32_t decode_time = 1250u;
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
    return codec == PHONE_CODEC_APTX || codec == PHONE_CODEC_LHDCV5 ||
           (codec == PHONE_CODEC_SBC && sbc != NULL);
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
    ++codec_resets;
    atomic_store(&codec_busy, false);
}
bool phone_codec_decode(const uint8_t *payload, size_t length, uint8_t count,
                        phone_codec_pcm_callback callback, void *context) {
    fake_time += decode_time;
    assert(!atomic_exchange(&codec_busy, true));
    assert(codec_configured && length != 0);
    atomic_store(&decode_entered, true);
    while (atomic_load(&hold_decode)) sched_yield();
    uint16_t frames = requested_codec == PHONE_CODEC_LHDCV5 ? count * 240u :
                      requested_codec == PHONE_CODEC_SBC ?
                          count * requested_sbc.blocks * requested_sbc.subbands : length;
    assert(frames != 0 && frames <= OUTPUT_FRAMES);
    int16_t pcm[OUTPUT_FRAMES * 2u];
    for (unsigned i = 0; i < OUTPUT_FRAMES; ++i) {
        pcm[2*i] = payload[0];pcm[2*i+1] = -(int16_t)payload[0];
    }
    callback(pcm, frames, context);
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
    for (unsigned i = 0; i < frames; ++i)
        assert(pcm[2*i] == expected_sample && pcm[2*i+1] == -expected_sample);
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
static void timed_phone_bursts(unsigned decode_us, unsigned poll_us) {
    // Relative HCI arrival times from the catch-up burst after a real radio gap.
    static const uint32_t arrivals[] = {0,29238,32735,44270,47735,56716,61290,
                                        64127,76901,80400,87814,109689,111329};
    assert(phone_worker_configure(PHONE_CODEC_LHDCV5, NULL));
    assert(phone_worker_prepare() == PHONE_WORKER_READY);
    uint8_t payload[PHONE_CODEC_MAX_PAYLOAD];
    unsigned packet = 0, received = 0, next_poll = 0, worker_done = 0;
    bool decoding = false;
    decode_time = decode_us;
    // Normal 15 ms packets follow the burst; the worker must drain its backlog.
    for (unsigned now = 0; received < 512;) {
        assert(now < 10000000u);
        while (packet < 512) {
            unsigned arrival = packet < 13 ? arrivals[packet] :
                               arrivals[12] + (packet - 12u) * 15000u;
            if (arrival > now) break;
            memset(payload, 1u + packet % 200u, sizeof(payload));
            assert(phone_worker_submit(payload, sizeof(payload), 3));
            ++packet;
        }
        if (decoding && now >= worker_done) {
            assert(phone_worker_step());
            decoding = false;
        }
        if (now >= next_poll) {
            if (phone_worker_queue_snapshot().output_ready) {
                expected_sample = (int16_t)(1u + received % 200u);
                unsigned before = delivered;
                assert(phone_worker_poll(receive, &delivered));
                assert(delivered == before + 720u);
                ++received;
            }
            next_poll += poll_us;
        }
        phone_worker_queue_snapshot_t queue = phone_worker_queue_snapshot();
        if (!decoding && queue.queued_packets && !queue.output_ready) {
            worker_done = now + decode_us;
            decoding = true;
        }
        if (received == 512) break;
        unsigned next = next_poll;
        if (decoding && worker_done < next) next = worker_done;
        if (packet < 512) {
            unsigned arrival = packet < 13 ? arrivals[packet] :
                               arrivals[12] + (packet - 12u) * 15000u;
            if (arrival < next) next = arrival;
        }
        assert(next > now);
        now = next;
    }
    phone_worker_queue_snapshot_t queue = phone_worker_queue_snapshot();
    assert(queue.queued_packets == 0 && !queue.output_ready);
    stop();
}
static void queue_gap_boundary(void) {
    start();
    uint8_t payload[4] = {1,0,0,0};
    assert(phone_worker_submit(payload, sizeof(payload), 0));
    assert(phone_worker_step()); // Hold this output while HCI callbacks fill the queue.
    unsigned resets = codec_resets;
    for (unsigned i = 0; i < INPUT_SLOTS; ++i) {
        payload[0] = (uint8_t)(i + 2u);
        assert(phone_worker_submit(payload, sizeof(payload), 0));
    }
    for (unsigned i = 0; i < 3; ++i)
        assert(!phone_worker_submit(payload, sizeof(payload), 0));
    expected_sample = 1;assert(phone_worker_poll(receive, &delivered));
    assert(phone_worker_step()); // An old packet frees a slot before the gap boundary.
    payload[0] = 42;assert(phone_worker_submit(payload, sizeof(payload), 0));
    for (unsigned i = 0; i < INPUT_SLOTS; ++i) {
        expected_sample = (int16_t)(i + 2u);
        assert(phone_worker_poll(receive, &delivered));
        assert(codec_resets == resets); // Earlier good packets survive the rejected input.
        if (i + 1u < INPUT_SLOTS) assert(phone_worker_step());
    }
    assert(phone_worker_step());expected_sample = 42;
    assert(phone_worker_poll(receive, &delivered));
    assert(codec_resets == resets + 1u); // Reset once at the first post-gap packet.
    assert(!phone_worker_submit(NULL, sizeof(payload), 0));
    assert(phone_worker_submit(payload, sizeof(payload), 0));
    assert(phone_worker_step());assert(phone_worker_poll(receive, &delivered));
    assert(codec_resets == resets + 1u); // Invalid calls do not invent a transport gap.
    stop();
}
static void maximum_packet_outputs(void) {
    const phone_codec_t codecs[] = {PHONE_CODEC_LHDCV5, PHONE_CODEC_APTX, PHONE_CODEC_SBC};
    const phone_sbc_config_t sbc = {16,8,2,0,2,53};
    uint8_t payload[PHONE_CODEC_MAX_PAYLOAD];memset(payload,7,sizeof(payload));
    for (unsigned codec = 0; codec < sizeof(codecs)/sizeof(*codecs); ++codec) {
        assert(phone_worker_configure(codecs[codec], &sbc));
        assert(phone_worker_prepare() == PHONE_WORKER_READY);
        uint8_t count = codecs[codec] == PHONE_CODEC_LHDCV5 ? 8 :
                        codecs[codec] == PHONE_CODEC_SBC ? 15 : 0;
        unsigned frames = codecs[codec] == PHONE_CODEC_APTX ? PHONE_CODEC_MAX_PAYLOAD : 1920;
        for (unsigned i = 0; i < INPUT_SLOTS; ++i)
            assert(phone_worker_submit(payload, sizeof(payload), count));
        assert(!phone_worker_submit(payload, sizeof(payload), count));
        expected_sample = 7;
        for (unsigned i = 0; i < INPUT_SLOTS; ++i) {
            unsigned before = delivered;
            assert(phone_worker_step());assert(phone_worker_poll(receive, &delivered));
            assert(delivered == before + frames);
        }
        stop();
    }
}
static void deterministic(void) {
    phone_worker_queue_snapshot_t queue=phone_worker_queue_snapshot();
    assert(queue.queued_packets==0 && !queue.output_ready);
    fail_allocation = true;
    assert(phone_worker_configure(PHONE_CODEC_APTX, NULL));
    assert(phone_worker_prepare() == PHONE_WORKER_FAILED);
    assert(phone_worker_released());
    fail_allocation = false;
    start();
    queue=phone_worker_queue_snapshot();assert(queue.queued_packets==0 && !queue.output_ready);
    assert(phone_worker_last_decode_us() == 0u && phone_worker_max_decode_us() == 0u);
    uint8_t data[] = {7, 0, 0, 0};
    assert(!phone_worker_submit(NULL, sizeof(data), 0));
    assert(!phone_worker_submit(data, 0, 0));
    assert(!phone_worker_submit(data, PHONE_CODEC_MAX_PAYLOAD + 1u, 0));
    for (unsigned i = 0; i < INPUT_SLOTS; ++i)
        assert(phone_worker_submit(data, sizeof(data), 0));
    queue=phone_worker_queue_snapshot();assert(queue.queued_packets==INPUT_SLOTS && !queue.output_ready);
    assert(!phone_worker_submit(data, sizeof(data), 0));
    assert(phone_worker_step());
    queue=phone_worker_queue_snapshot();assert(queue.queued_packets==INPUT_SLOTS-1u && queue.output_ready);
    assert(!phone_worker_step()); // Main-loop backpressure, no decode spinning.
    phone_worker_reset();
    assert(phone_worker_poll(receive, &delivered)); // Stale mailbox discarded.
    queue=phone_worker_queue_snapshot();assert(queue.queued_packets==INPUT_SLOTS-1u && !queue.output_ready);
    assert(delivered == 0);
    for (unsigned i = 1; i < INPUT_SLOTS; ++i) assert(phone_worker_step()); // Stale queued input discarded.
    assert(!phone_worker_step());
    queue=phone_worker_queue_snapshot();assert(queue.queued_packets==0 && !queue.output_ready);
    expected_sample = 7;
    assert(phone_worker_submit(data, sizeof(data), 0));
    data[0] = 10; // Input is copied before the Bluetooth callback returns.
    assert(phone_worker_step());
    assert(phone_worker_poll(receive, &delivered));
    assert(delivered == 4 && callbacks == 1);
    assert(phone_worker_last_decode_us() == 1250u);
    assert(phone_worker_max_decode_us() == 1250u);
    for (unsigned bad = 0xfe; bad <= 0xff; ++bad) {
        decode_time = bad == 0xfe ? 2500u : 1250u;
        fake_time = UINT32_MAX - 500u;
        data[0] = bad;
        assert(phone_worker_submit(data, sizeof(data), 0));
        assert(phone_worker_step());
        assert(!phone_worker_poll(receive, &delivered));
        assert(delivered == 4 && callbacks == 1);
        assert(phone_worker_last_decode_us() == decode_time);
        assert(phone_worker_max_decode_us() == 2500u);
        phone_worker_reset();
    }
    // Configuration changes wait for worker detachment; they cannot free or
    // reset a codec underneath its decode operation.
    assert(phone_worker_configure(PHONE_CODEC_APTX, NULL));
    assert(phone_worker_prepare() == PHONE_WORKER_WAITING);
    assert(phone_worker_step());
    assert(phone_worker_prepare() == PHONE_WORKER_READY);
    assert(phone_worker_max_decode_us() == 0u);
    stop();
    queue=phone_worker_queue_snapshot();assert(queue.queued_packets==0 && !queue.output_ready);
    assert(phone_worker_last_decode_us() == 0u && phone_worker_max_decode_us() == 0u);
    assert(allocations == frees);
}

static void queue_snapshot_wrap(void) {
    start();
    atomic_store(&owned->input_read,UINT32_MAX-3u);
    atomic_store(&owned->input_written,UINT32_MAX-3u);
    uint8_t data[4]={7,0,0,0};
    for (unsigned i=0;i<INPUT_SLOTS;++i) {
        data[0]=(uint8_t)(7u+i);assert(phone_worker_submit(data,sizeof(data),0));
    }
    phone_worker_queue_snapshot_t queue=phone_worker_queue_snapshot();
    assert(queue.queued_packets==INPUT_SLOTS && !queue.output_ready);
    for (unsigned i=0;i<INPUT_SLOTS;++i) {
        assert(phone_worker_step());
        queue=phone_worker_queue_snapshot();assert(queue.queued_packets==INPUT_SLOTS-i-1u && queue.output_ready);
        expected_sample=(int16_t)(7u+i);assert(phone_worker_poll(receive,&delivered));
    }
    queue=phone_worker_queue_snapshot();assert(queue.queued_packets==0 && !queue.output_ready);
    stop();
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
    const unsigned decode_costs[] = {7000,11000,13800};
    for (unsigned i = 0; i < sizeof(decode_costs)/sizeof(*decode_costs); ++i) {
        timed_phone_bursts(decode_costs[i], 1000);
        timed_phone_bursts(decode_costs[i], 3600);
    }
    queue_gap_boundary();
    maximum_packet_outputs();
    delivered = callbacks = 0;
    decode_time = 1250u;
    deterministic();
    queue_snapshot_wrap();
    threaded();
    printf("phone worker: bounds, cancellation, partial failure, reconfigure, threaded release passed; %zu-byte phone-only storage\n", sizeof(worker_t));
}
