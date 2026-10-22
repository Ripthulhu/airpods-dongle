// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/btstack/phone_codec.h"
#include "../src/btstack/phone_worker.h"
#include "../src/btstack/phone_benchmark.h"
#include "freeaptx.h"
#include "sbc_encoder.h"
#include "lhdc48_bench_fixture.h"

static int16_t captured[4096];
static unsigned samples, calls;
static bool reject_allocations;
static unsigned allocations;
static uint32_t synthetic_clock = 1;
uint32_t phone_test_time_us(void) { return synthetic_clock += 100u; }
static bool benchmark_idle(void) {
    phone_worker_step();
    synthetic_clock += 100u;
    return true;
}
void phone_worker_wake(void) {}
extern void test_phone_resampler(void);
extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t count, size_t size);
extern void __real_free(void *pointer);
void *__wrap_malloc(size_t size) {
    allocations++;
    return reject_allocations ? NULL : __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
    allocations++;
    return reject_allocations ? NULL : __real_calloc(count, size);
}
void __wrap_free(void *pointer) { __real_free(pointer); }
static void collect(const int16_t *pcm, uint16_t frames, void *context) {
    assert(context == &samples && frames != 0 && samples + frames * 2u <= 4096);
    memcpy(captured + samples, pcm, frames * 2u * sizeof(*pcm));
    samples += frames * 2u;
    calls++;
}

static void test_aptx(void) {
    struct aptx_context *encoder = aptx_init(0), *reference = aptx_init(0);
    assert(encoder && reference && !phone_codec_ready());
    assert(phone_codec_available(PHONE_CODEC_APTX));
    assert(!phone_codec_available(PHONE_CODEC_NONE));
    assert(phone_codec_configure(PHONE_CODEC_APTX, NULL) && !phone_codec_ready());
    assert(phone_codec_prepare() && phone_codec_ready());
    unsigned prepared_allocations = allocations;
    unsigned char input[6144], encoded[1024], raw[6144];
    for (unsigned frame = 0; frame < 1024; ++frame) {
        int32_t left = ((int32_t)(frame % 96) - 48) * 30000;
        int32_t right = -left;
        for (unsigned byte = 0; byte < 3; ++byte) {
            input[frame * 6 + byte] = (uint8_t)((uint32_t)left >> (byte * 8));
            input[frame * 6 + 3 + byte] = (uint8_t)((uint32_t)right >> (byte * 8));
        }
    }
    size_t written, reference_bytes;
    assert(aptx_encode(encoder, input, sizeof(input), encoded, sizeof(encoded), &written)
           == sizeof(input) && written == sizeof(encoded));
    assert(aptx_decode(reference, encoded, sizeof(encoded), raw, sizeof(raw), &reference_bytes)
           == sizeof(encoded));
    samples = calls = 0;
    assert(phone_codec_decode(encoded, sizeof(encoded), 0, collect, &samples));
    assert(allocations == prepared_allocations);
    assert(samples == reference_bytes / 3 && calls > 1);
    uint32_t checksum = 2166136261u;
    for (unsigned i = 0; i < samples; ++i) {
        uint16_t bits = raw[i * 3 + 1] | ((uint16_t)raw[i * 3 + 2] << 8);
        int16_t expected;
        memcpy(&expected, &bits, sizeof(expected));
        assert(captured[i] == expected);
        checksum = (checksum ^ (uint16_t)captured[i]) * 16777619u;
    }
    printf("aptX fixture samples=%u checksum=%08x\n", samples, (unsigned)checksum);
    assert(samples == 1868 && checksum == 0xd9974350u);
    phone_codec_reset();
    samples = calls = 0;
    assert(phone_codec_decode(encoded, 400, 0, collect, &samples));
    assert(phone_codec_decode(encoded + 400, sizeof(encoded) - 400, 0, collect, &samples));
    assert(samples == reference_bytes / 3);
    assert(!phone_codec_decode(encoded, 3, 0, collect, &samples));
    assert(!phone_codec_decode(encoded, sizeof(encoded), 1, collect, &samples));
    assert(!phone_codec_decode(encoded, sizeof(encoded) + 4, 0, collect, &samples));
    unsigned char corrupted[1024];
    memcpy(corrupted, encoded, sizeof(corrupted));
    // A parity error must resynchronize without requiring a new phone Start.
    corrupted[0] ^= 0x20;
    samples = calls = 0;
    assert(phone_codec_decode(corrupted, sizeof(corrupted), 0, collect, &samples));
    assert(samples > 0 && samples < reference_bytes / 3);
    phone_codec_reset();
    samples = calls = 0;
    assert(phone_codec_decode(encoded, sizeof(encoded), 0, collect, &samples));
    assert(samples == reference_bytes / 3);
    aptx_finish(encoder);
    aptx_finish(reference);
    phone_codec_close();
    assert(!phone_codec_ready());
    assert(phone_worker_configure(PHONE_CODEC_APTX, NULL));
    assert(phone_worker_prepare() == PHONE_WORKER_READY);
    prepared_allocations = allocations;
    samples = calls = 0;
    assert(phone_worker_submit(encoded, sizeof(encoded), 0));
    assert(phone_worker_step());
    assert(calls == 0); // Core1 never calls the Bluetooth-side PCM consumer.
    assert(phone_worker_poll(collect, &samples));
    assert(calls == 1 && samples == 1868);
    checksum = 2166136261u;
    for (unsigned i = 0; i < samples; ++i)
        checksum = (checksum ^ (uint16_t)captured[i]) * 16777619u;
    assert(checksum == 0xd9974350u && allocations == prepared_allocations);
    assert(!phone_worker_close());
    assert(phone_worker_step());
    assert(phone_worker_close());
}

static void test_allocation_failure_falls_back(void) {
    assert(phone_codec_configure(PHONE_CODEC_APTX, NULL));
    reject_allocations = true;
    assert(!phone_codec_prepare() && !phone_codec_ready());
    const phone_sbc_config_t config = {16, 8, 3, 0, 2, 53};
    assert(phone_codec_configure(PHONE_CODEC_SBC, &config));
    assert(phone_codec_prepare() && phone_codec_ready());
    phone_codec_close();
    reject_allocations = false;
}

static void test_sbc(void) {
    const phone_sbc_config_t config = {16, 8, 3, 0, 2, 53};
    assert(phone_codec_configure(PHONE_CODEC_SBC, &config));
    assert(phone_codec_prepare() && phone_codec_ready());
    static SBC_ENC_PARAMS encoder;
    encoder.s16SamplingFreq = SBC_sf48000;
    encoder.s16ChannelMode = SBC_JOINT_STEREO;
    encoder.s16NumOfSubBands = 8;
    encoder.s16NumOfChannels = 2;
    encoder.s16NumOfBlocks = 16;
    encoder.s16AllocationMethod = SBC_LOUDNESS;
    encoder.s16BitPool = 53;
    encoder.u8NumPacketToEncode = 1;
    SBC_Encoder_Init(&encoder);
    int16_t pcm[256];
    uint8_t packet[1024];
    for (unsigned i = 0; i < 256; ++i) pcm[i] = (int16_t)(((int)i % 48 - 24) * 600);
    encoder.ps16PcmBuffer = pcm;
    encoder.pu8Packet = packet;
    SBC_Encoder(&encoder);
    unsigned frame_bytes = encoder.u16PacketLength;
    assert(frame_bytes > 4 && frame_bytes * 2 < sizeof(packet));
    memcpy(packet + frame_bytes, packet, frame_bytes);
    samples = calls = 0;
    assert(phone_codec_decode(packet, frame_bytes * 2, 2, collect, &samples));
    assert(calls == 2 && samples == 512);
    samples = calls = 0;
    packet[frame_bytes + 3] ^= 1; // Bad CRC in tail rejects the valid prefix too.
    assert(!phone_codec_decode(packet, frame_bytes * 2, 2, collect, &samples));
    assert(calls == 0);
    packet[frame_bytes + 3] ^= 1;
    for (unsigned n = 0; n < frame_bytes; ++n)
        assert(!phone_codec_decode(packet, n, 1, collect, &samples));
    assert(calls == 0);
    assert(!phone_codec_decode(packet, frame_bytes, 0, collect, &samples));
    assert(!phone_codec_decode(packet, frame_bytes, 2, collect, &samples));
    assert(!phone_codec_decode(packet, frame_bytes + 1, 1, collect, &samples));
    assert(calls == 0);
    packet[1] ^= 0x40; // Wrong sample rate cannot enter the 48 kHz bridge.
    assert(!phone_codec_decode(packet, frame_bytes, 1, collect, &samples));
    packet[1] ^= 0x40;
    phone_sbc_config_t invalid = config;
    invalid.channel_mode = 0;
    assert(!phone_codec_configure(PHONE_CODEC_SBC, &invalid));
    assert(!phone_codec_ready());
    assert(phone_codec_configure(PHONE_CODEC_SBC, &config));
    assert(phone_codec_decode(packet, frame_bytes, 1, collect, &samples));
    assert(calls == 1 && samples == 256);
    phone_codec_close();

    assert(phone_worker_configure(PHONE_CODEC_SBC, &config));
    assert(phone_worker_prepare() == PHONE_WORKER_READY);
    unsigned prepared_allocations = allocations;
    samples = calls = 0;
    packet[frame_bytes + 3] ^= 1;
    assert(phone_worker_submit(packet, frame_bytes * 2, 2));
    assert(phone_worker_step());
    assert(!phone_worker_poll(collect, &samples));
    assert(calls == 0); // Bad tail cannot publish a valid prefix.
    packet[frame_bytes + 3] ^= 1;
    phone_worker_reset();
    assert(phone_worker_submit(packet, frame_bytes * 2, 2));
    assert(phone_worker_step());
    phone_worker_reset();
    assert(phone_worker_poll(collect, &samples));
    assert(calls == 0); // Completed PCM from an old stream is discarded.
    encoder.s16BitPool = 2;
    SBC_Encoder_Init(&encoder);
    encoder.ps16PcmBuffer = pcm;
    encoder.pu8Packet = packet;
    SBC_Encoder(&encoder);
    frame_bytes = encoder.u16PacketLength;
    assert(frame_bytes * 15 <= sizeof(packet));
    for (unsigned frame = 1; frame < 15; ++frame)
        memcpy(packet + frame * frame_bytes, packet, frame_bytes);
    assert(phone_worker_submit(packet, frame_bytes * 15, 15));
    assert(phone_worker_step());
    assert(phone_worker_poll(collect, &samples));
    assert(samples == 3840 && calls == 1 && allocations == prepared_allocations);
    assert(!phone_worker_close());
    assert(phone_worker_step());
    assert(phone_worker_close());
}

static void test_lhdc(void) {
    assert(!phone_codec_available(PHONE_CODEC_LHDCV5));
    assert(phone_codec_available(PHONE_CODEC_APTX) && phone_codec_available(PHONE_CODEC_SBC));

    phone_benchmark_result_t result;
    assert(phone_benchmark_lhdc(phone_test_time_us, benchmark_idle, &result));
    assert(result.cleanup && result.frames == LHDC48_BENCH_FRAME_COUNT);
    assert(phone_codec_available(PHONE_CODEC_LHDCV5));
    assert(phone_worker_released() && !phone_codec_ready());
    phone_codec_enable_lhdc(false);
    // Startup uses the same worker before exposing this endpoint to a phone.
    assert(phone_worker_configure(PHONE_CODEC_LHDCV5, NULL));
    assert(phone_worker_prepare() == PHONE_WORKER_READY);
    unsigned prepared_allocations = allocations;
    for (unsigned i = 0; i < LHDC48_BENCH_FRAME_COUNT; ++i) {
        samples = calls = 0;
        assert(phone_worker_submit(lhdc48_bench_data + i * LHDC48_BENCH_FRAME_BYTES,
                                   LHDC48_BENCH_FRAME_BYTES, 1));
        assert(phone_worker_step());
        assert(phone_worker_poll(collect, &samples));
        assert(samples == 480 && calls == 1);
        assert(allocations == prepared_allocations);
    }
    phone_worker_reset();
    samples = calls = 0;
    assert(phone_worker_submit(lhdc48_bench_data, LHDC48_BENCH_FRAME_BYTES - 1, 1));
    assert(phone_worker_step());
    assert(!phone_worker_poll(collect, &samples));
    assert(samples == 0 && calls == 0);
    assert(!phone_worker_close());
    assert(phone_worker_step());
    assert(phone_worker_close());
    assert(!phone_codec_ready());
    phone_codec_enable_lhdc(true);
    assert(phone_codec_available(PHONE_CODEC_LHDCV5));
    phone_codec_enable_lhdc(false);
    assert(!phone_codec_available(PHONE_CODEC_LHDCV5));
    assert(phone_codec_available(PHONE_CODEC_APTX) && phone_codec_available(PHONE_CODEC_SBC));

    assert(phone_codec_configure(PHONE_CODEC_LHDCV5, NULL));
    reject_allocations = true;
    assert(!phone_codec_prepare() && !phone_codec_ready());
    phone_codec_close();
    reject_allocations = false;
    assert(phone_codec_configure(PHONE_CODEC_APTX, NULL));
    assert(phone_codec_prepare());
    phone_codec_close();
}

int main(void) {
    test_phone_resampler();
    test_aptx();
    test_allocation_failure_falls_back();
    test_sbc();
    test_lhdc();
    assert(!phone_codec_prepare());
    puts("phone LHDC/aptX/SBC real codecs, PCM conversion, bounds and reset: passed");
}
