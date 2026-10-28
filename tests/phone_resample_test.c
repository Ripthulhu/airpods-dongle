// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "btstack_resample.h"
#include "../src/btstack/phone_clock.h"
#include "../src/btstack/playback_pcm.h"

static playback_pcm_t clock_pcm;

static void test_resampler_bounds(void) {
    int16_t input[128 * 2];
    for (unsigned i = 0; i < 128; ++i) {
        input[i * 2] = 12345;
        input[i * 2 + 1] = -23456;
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        btstack_resample_t resampler;
        btstack_resample_init(&resampler, 2);
        uint32_t total_in = 0, total_out = 0;
        for (unsigned block = 0; block < 10000; ++block) {
            uint32_t count = 1 + block % 128;
            uint32_t factor = mode == 0 ? 0x10000u : mode == 1 ? 0x10000u - 32u :
                              mode == 2 ? 0x10000u + 32u :
                              block % 2 ? 0x10000u - 32u : 0x10000u + 32u;
            struct { uint32_t before; int16_t pcm[130 * 2]; uint32_t after; } output;
            output.before = output.after = 0xa5c37e91u;
            btstack_resample_set_factor(&resampler, factor);
            uint16_t written = btstack_resample_block(&resampler, input, count, output.pcm);
            assert(written <= count + 2 && written <= 130);
            assert(output.before == 0xa5c37e91u && output.after == 0xa5c37e91u);
            for (unsigned i = 0; i < written; ++i) {
                assert(output.pcm[i * 2] == 12345);
                assert(output.pcm[i * 2 + 1] == -23456);
            }
            total_in += count;
            total_out += written;
        }
        if (mode == 0) assert(total_out == total_in - 1u);
        if (mode == 1 || mode == 2) {
            uint32_t factor = mode == 1 ? 0x10000u - 32u : 0x10000u + 32u;
            uint32_t ideal = (uint32_t)((uint64_t)total_in * 0x10000u / factor);
            assert(total_out <= ideal + 1u && total_out + 2u >= ideal);
        }
    }
}

#ifndef PHONE_TEST_ARM
// Exercise real interpolation/state, not a mock sample-count converter. Compare
// variable-size 20 ms bursts and fixed-size packets from a drifting source clock.
static void test_long_clock(uint32_t read_frames, int ppm, bool fixed_packets) {
    btstack_resample_t resampler;
    phone_clock_t clock;
    btstack_resample_init(&resampler, 2);
    phone_clock_init(&clock, read_frames);
    assert(playback_pcm_reset(&clock_pcm, PHONE_PCM_CAPACITY, false));
    int16_t input[128 * 2] = {0}, adjusted[130 * 2], output[1024 * 2];
    uint64_t packet = 0, reads = 1;
    uint32_t input_fraction = 0, underflows = 0, overflows = 0;
    bool started = false;
    for (;;) {
        uint64_t input_time = fixed_packets ? packet * 20000u * 1000000u / (1000000 + ppm) :
                                             packet * 20000u;
        uint64_t output_time = reads * read_frames * 1000000u / 48000u;
        if (input_time > 600000000u && output_time > 600000000u) break;
        if (input_time <= output_time) {
            uint32_t frames = PHONE_PACKET_FRAMES;
            if (!fixed_packets) {
                input_fraction += PHONE_PACKET_FRAMES * (1000000 + ppm);
                frames = input_fraction / 1000000u;
                input_fraction %= 1000000u;
            }
            while (frames != 0) {
                uint32_t count = frames > 128 ? 128 : frames;
                uint16_t written = btstack_resample_block(&resampler, input, count, adjusted);
                assert(written <= 130);
                if (written > clock_pcm.capacity - clock_pcm.used) overflows++;
                playback_pcm_push(&clock_pcm, adjusted, written);
                frames -= count;
            }
            packet++;
        } else {
            reads++;
            if (!started && clock_pcm.used < phone_clock_prefill(read_frames)) continue;
            started = true;
            if (playback_pcm_read(&clock_pcm, output, read_frames) != read_frames) underflows++;
            uint32_t factor = phone_clock_update(&clock, clock_pcm.used, read_frames);
            assert(factor >= 0x10000u - 32u && factor <= 0x10000u + 32u);
            btstack_resample_set_factor(&resampler, factor);
        }
    }
    if (underflows != 0 || overflows != 0)
        printf("clock %u/%d/%u: underflows=%u overflows=%u\n",
               read_frames, ppm, fixed_packets, underflows, overflows);
    assert(started && underflows == 0 && overflows == 0);
}

static void test_phone_packet_jitter(unsigned phase, bool ingress_gap, bool encoder_stall) {
    btstack_resample_t resampler;
    phone_clock_t clock;
    btstack_resample_init(&resampler, 2);
    phone_clock_init(&clock, 480);
    assert(playback_pcm_reset(&clock_pcm, PHONE_PCM_CAPACITY, false));
    int16_t input[128 * 2], adjusted[130 * 2], output[480 * 2];
    for (unsigned i = 0; i < 128; ++i) { input[2*i] = 12345; input[2*i+1] = -23456; }
    unsigned packet = 0, next_read = phase, debt = 0, next_encoder = 0;
    unsigned missing = 0, dropped = 0;
    bool started = false;
    // Real LHDC packets contain 720 frames / 15 ms, often delivered in 20 ms batches.
    // Let clock feedback settle before the observed 72 ms gap and a 60 ms sender stall.
    for (unsigned now = 0; now < 180000000u; now += 100) {
        for (;;) {
            unsigned arrival = (packet * 15000u + 19999u) / 20000u * 20000u;
            if (ingress_gap && arrival > 120000000u && arrival < 120072000u)
                arrival = 120072000u;
            if (arrival > now) break;
            unsigned frames = 720;
            while (frames != 0) {
                unsigned count = frames > 128 ? 128 : frames;
                unsigned written = btstack_resample_block(&resampler, input, count, adjusted);
                unsigned available = clock_pcm.capacity - clock_pcm.used;
                if (written > available) dropped += written - available;
                playback_pcm_push(&clock_pcm, adjusted, written);
                frames -= count;
            }
            ++packet;
        }
        while (now >= next_read) { debt += 480; next_read += 10000; }
        bool stalled = encoder_stall && now >= 140000100u && now < 140060100u;
        if (stalled || now < next_encoder || debt < 480) continue;
        debt -= 480;
        next_encoder = now + 1700; // One encoding unit per main-loop pass.
        if (!started && clock_pcm.used < phone_clock_prefill(480)) continue;
        started = true;
        unsigned copied = playback_pcm_read(&clock_pcm, output, 480);
        missing += 480 - copied;
        if (copied == 480) for (unsigned i = 0; i < 480; ++i) {
            assert(output[2*i] == 12345 && output[2*i+1] == -23456);
        }
        btstack_resample_set_factor(&resampler, phone_clock_update(&clock, clock_pcm.used, 480));
    }
    if (missing || dropped)
        printf("phone jitter phase=%u gap=%u stall=%u: missing=%u dropped=%u\n",
               phase, ingress_gap, encoder_stall, missing, dropped);
    assert(started && missing == 0 && dropped == 0);
}
#endif

void test_phone_resampler(void) {
    test_resampler_bounds();
#ifndef PHONE_TEST_ARM
    const uint32_t reads[] = {128, 480, 1024};
    for (unsigned i = 0; i < sizeof(reads) / sizeof(reads[0]); ++i)
        for (int ppm = -300; ppm <= 300; ppm += 300) {
            test_long_clock(reads[i], ppm, false);
            test_long_clock(reads[i], ppm, true);
        }
    for (unsigned phase = 0; phase < 10000; phase += 1000) {
        test_phone_packet_jitter(phase, true, false);
        test_phone_packet_jitter(phase, false, true);
        test_phone_packet_jitter(phase, true, true);
    }
#endif
    puts("phone real resampler bounds, channel state and clock correction: passed");
}
