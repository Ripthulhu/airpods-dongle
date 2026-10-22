// SPDX-License-Identifier: GPL-3.0-only
#include "lhdc48.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lhdc48_bench_fixture.h"
#include "music_reference.h"
#include "decoder/lhdc_entropy_dec.h"
#include "decoder/lhdc_tables.h"
#include "decoder/lhdc_bit_reader.h"
#ifdef LHDC48_ARM_PROBE
#include <malloc.h>
static uint32_t *mark_bottom, *mark_top;
__attribute__((noinline)) static void mark_stack(void) {
    uintptr_t pointer;
    __asm__ volatile("mov %0,sp" : "=r"(pointer));
    mark_bottom = (uint32_t *)(pointer - 32768);
    mark_top = (uint32_t *)(pointer - 128);
    for (uint32_t *p = mark_bottom; p < mark_top; ++p) *p = 0xa5a5a5a5u;
    __asm__ volatile("" ::: "memory");
}
static unsigned stack_depth(void) {
    __asm__ volatile("" ::: "memory");
    uint32_t *p = mark_bottom;
    while (p < mark_top && *p == 0xa5a5a5a5u) ++p;
    assert(p != mark_bottom);
    return (unsigned)((uintptr_t)mark_top - (uintptr_t)p);
}
#endif

static bool forbid_allocations;
static size_t live, peak;
static unsigned allocations, fault_at, delivered;
static bool compare_music;
static unsigned music_samples, compared;
static uint32_t music_hash = 2166136261u;
static int64_t signal_energy[2], error_energy[2], cross_energy[2];
#ifndef LHDC48_ARM_PROBE
static FILE *pcm_file;
#endif
static struct { void *pointer; size_t size; } blocks[64];
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void __real_free(void *);
static void *tracked(size_t bytes, bool zero) {
    assert(!forbid_allocations);
    if (++allocations == fault_at) return NULL;
    void *pointer = zero ? __real_calloc(1, bytes) : __real_malloc(bytes);
    if (!pointer) return NULL;
    unsigned n = 0;
    while (n < 64 && blocks[n].pointer) ++n;
    assert(n < 64);
    blocks[n].pointer = pointer; blocks[n].size = bytes;
    live += bytes; if (live > peak) peak = live;
    return pointer;
}
void *__wrap_malloc(size_t bytes) { return tracked(bytes, false); }
void *__wrap_calloc(size_t n, size_t bytes) { assert(!bytes || n <= SIZE_MAX / bytes); return tracked(n * bytes, true); }
void __wrap_free(void *pointer) {
    if (!pointer) return;
    unsigned n = 0;
    while (n < 64 && blocks[n].pointer != pointer) ++n;
    assert(n < 64);
    live -= blocks[n].size; blocks[n].pointer = NULL;
    __real_free(pointer);
}
int64_t lhdc48_probe_time_us(void) {
#ifdef LHDC48_ARM_PROBE
    return 0;
#else
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
#endif
}
static void collect(const int16_t *pcm, uint16_t frames, void *context) {
    (void)pcm; assert(context == &delivered && frames == 240); ++delivered;
    if (compare_music) {
        for (unsigned n = 0; n < frames * 2u; ++n)
            music_hash = (music_hash ^ (uint16_t)pcm[n]) * 16777619u;
        for (unsigned n = 0; n < frames; ++n, ++music_samples) {
            if (music_samples < 960 + 168 || (music_samples - 960 - 168) % 8) continue;
            unsigned index = (music_samples - 960 - 168) / 8;
            if (index >= sizeof(lhdc_music_reference) / sizeof(*lhdc_music_reference) / 2) continue;
            for (unsigned c = 0; c < 2; ++c) {
                int64_t expected = lhdc_music_reference[index * 2 + c];
                int64_t actual = pcm[n * 2 + c];
                signal_energy[c] += expected * expected;
                error_energy[c] += (actual - expected) * (actual - expected);
                cross_energy[c] += actual * expected;
            }
            ++compared;
        }
    }
#ifndef LHDC48_ARM_PROBE
    if (pcm_file) assert(fwrite(pcm, sizeof(*pcm), frames * 2u, pcm_file) == frames * 2u);
#endif
}
static uint32_t random_state = 0x0315abcd;
static uint8_t random_byte(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return (uint8_t)random_state;
}
static void profile_bounds(void) {
    for (unsigned bytes = 0; bytes <= 511; ++bytes) {
        const lhdc_band_cfg_desc_t *band = lhdc_get_band_cfg_frame(48000, 5, (int)bytes);
        assert(band->mdct_size == 480 && band->ma_win1 <= 57 && band->ma_win2 <= 63);
    }
    const uint8_t encoded[4] = {0};
    uint8_t fac[4], symbols[1];
    int32_t coefficient;
    lhdc_dec_bit_reader_t bits;
    lhdc_bit_reader_init(&bits, encoded, sizeof(encoded));
    assert(lhdc_entropy_decode_spectrum_ex2(&coefficient, 1, &bits, 1, 0, 58, 63,
            NULL, fac, sizeof(fac), symbols) == LHDC_DEC_BITSTREAM_ERROR);
    assert(lhdc_bit_reader_tell(&bits) == 0);
    assert(lhdc_entropy_decode_spectrum_ex2(&coefficient, 1, &bits, 1, 0, 57, 64,
            NULL, fac, sizeof(fac), symbols) == LHDC_DEC_BITSTREAM_ERROR);
    assert(lhdc_bit_reader_tell(&bits) == 0);
    lhdc_decoder_t storage = {0};
    const lhdc_dec_config_t config = {.sample_rate = LHDC_DEC_SR_48000,
        .bit_depth = LHDC_DEC_BITDEPTH_24, .frame_duration = LHDC_DEC_FRAME_5MS,
        .channels = 2, .max_frame_bytes = 1024};
    for (unsigned n = 0; n < 6; ++n) {
        lhdc_dec_config_t invalid = config;
        switch (n) {
            case 0: invalid.sample_rate = LHDC_DEC_SR_96000; break;
            case 1: invalid.frame_duration = LHDC_DEC_FRAME_10MS; break;
            case 2: invalid.bit_depth = LHDC_DEC_BITDEPTH_32; break;
            case 3: invalid.channels = 1; break;
            case 4: invalid.max_frame_bytes = 1536; break;
            default: invalid.lossless_enable = 1; break;
        }
        unsigned before = allocations;
        assert(!lhdc_dec_init(&storage, &invalid) && allocations == before);
    }
}
#ifndef LHDC48_ARM_PROBE
int main(int argc, char **argv) {
    if (argc == 4) {
        assert(lhdc48_prepare((uint8_t)atoi(argv[1])));
        FILE *input = fopen(argv[2], "rb"); assert(input);
        pcm_file = fopen(argv[3], "wb"); assert(pcm_file);
        unsigned failed = 0, frames = 0;
        int64_t elapsed = 0, maximum = 0;
        unsigned prepared = allocations;
        uint8_t packet[1024];
        forbid_allocations = true;
        while (fread(packet, 1, 2, input) == 2) {
            unsigned bytes = 2 + 2 * ((packet[0] | (unsigned)packet[1] << 8) & 0x3ffu);
            assert(bytes <= sizeof(packet) && fread(packet + 2, 1, bytes - 2, input) == bytes - 2);
            int64_t before = lhdc48_probe_time_us();
            bool ok = lhdc48_decode(packet, bytes, 1, collect, &delivered);
            int64_t duration = lhdc48_probe_time_us() - before;
            elapsed += duration; if (duration > maximum) maximum = duration;
            if (!ok) {
                int16_t silence[480] = {0};
                assert(fwrite(silence, sizeof(*silence), 480, pcm_file) == 480);
                ++failed;
            }
            assert(allocations == prepared);
            ++frames;
        }
        forbid_allocations = false;
        fclose(input); fclose(pcm_file); pcm_file = NULL;
        printf("LHDC corpus frames=%u failed=%u live=%u peak=%u avg_us=%u max_us=%u\n",
               frames, failed, (unsigned)live, (unsigned)peak,
               frames ? (unsigned)(elapsed / frames) : 0, (unsigned)maximum);
        lhdc48_close(); assert(live == 0);
        return failed ? 2 : 0;
    }
#else
int main(void) {
#endif
    profile_bounds();
    assert(!lhdc48_ready() && !lhdc48_prepare(32));
    assert(lhdc48_prepare(24) && lhdc48_ready());
    size_t prepared_live = live;
    unsigned prepared_allocations = allocations;
    printf("LHDC prepare live=%u peak=%u allocations=%u\n", (unsigned)live, (unsigned)peak, allocations);
    uint8_t packet[1024] = {0};
    assert(!lhdc48_decode(NULL, 4, 1, collect, &delivered));
    assert(!lhdc48_decode(packet, sizeof(packet), 0, collect, &delivered));
    assert(!lhdc48_decode(packet, sizeof(packet), 9, collect, &delivered));
    assert(!lhdc48_decode(packet, sizeof(packet) + 1u, 1, collect, &delivered));
    assert(delivered == 0);
    forbid_allocations = true;
#ifdef LHDC48_ARM_PROBE
    mark_stack();
#endif
    compare_music = true;
    for (unsigned n = 0; n < LHDC48_BENCH_FRAME_COUNT; ++n)
        assert(lhdc48_decode(lhdc48_bench_data + n * LHDC48_BENCH_FRAME_BYTES,
                            LHDC48_BENCH_FRAME_BYTES, 1, collect, &delivered));
    compare_music = false;
    assert(compared == sizeof(lhdc_music_reference) / sizeof(*lhdc_music_reference) / 2);
    for (unsigned c = 0; c < 2; ++c) {
        assert(error_energy[c] * 1000 < signal_energy[c]); // >30 dB without fitting gain.
        assert(cross_energy[c] * 1000 > signal_energy[c] * 995 &&
               cross_energy[c] * 1000 < signal_energy[c] * 1005);
    }
    printf("LHDC quality music frames=%u reference_samples=%u gain_ppm=%u/%u snr_above_30db=1 checksum=%08x\n",
           LHDC48_BENCH_FRAME_COUNT, compared,
           (unsigned)(cross_energy[0] * 1000000 / signal_energy[0]),
           (unsigned)(cross_energy[1] * 1000000 / signal_energy[1]), (unsigned)music_hash);
    unsigned before_calls = delivered;
    lhdc48_reset();
    assert(lhdc48_decode(lhdc48_bench_data, 4 * LHDC48_BENCH_FRAME_BYTES, 4, collect, &delivered));
    assert(delivered == before_calls + 4);
    before_calls = delivered;
    memcpy(packet, lhdc48_bench_data, LHDC48_BENCH_FRAME_BYTES);
    packet[LHDC48_BENCH_FRAME_BYTES] = 0xff;
    packet[LHDC48_BENCH_FRAME_BYTES + 1] = 0x07;
    assert(!lhdc48_decode(packet, LHDC48_BENCH_FRAME_BYTES + 2, 2, collect, &delivered));
    assert(delivered == before_calls); // No valid prefix may escape a malformed packet tail.
    for (unsigned byte = 0; byte < LHDC48_BENCH_FRAME_BYTES; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            memcpy(packet, lhdc48_bench_data, LHDC48_BENCH_FRAME_BYTES);
            packet[byte] ^= (uint8_t)(1u << bit);
            lhdc48_reset();
            (void)lhdc48_decode(packet, LHDC48_BENCH_FRAME_BYTES, 1, collect, &delivered);
            assert(live == prepared_live && allocations == prepared_allocations);
        }
    }
    for (unsigned trial = 0; trial < 20000; ++trial) {
        unsigned length = 20 + 2 * (trial % 503);
        unsigned header = (length - 2) / 2 | (1u << 10);
        packet[0] = (uint8_t)header; packet[1] = (uint8_t)(header >> 8);
        for (unsigned n = 2; n < length; ++n) packet[n] = random_byte();
        lhdc48_reset();
        (void)lhdc48_decode(packet, length, 1, collect, &delivered);
        assert(live == prepared_live && allocations == prepared_allocations);
        delivered = 0;
        assert(!lhdc48_decode(packet, length - 1, 1, collect, &delivered));
        assert(delivered == 0);
    }
#ifdef LHDC48_ARM_PROBE
    unsigned depth = stack_depth();
    printf("LHDC ARM stack_below_mark=%u allocator_arena=%u\n", depth, (unsigned)mallinfo().arena);
#endif
    forbid_allocations = false;
    lhdc48_close(); assert(!lhdc48_ready() && live == 0);
    // Every allocation site must fail cleanly, including transform/window prewarming.
    for (unsigned n = 1; n <= prepared_allocations; ++n) {
        fault_at = allocations + n;
        bool ready = lhdc48_prepare(24);
        (void)ready;
        lhdc48_close(); assert(!lhdc48_ready() && live == 0);
    }
    puts("LHDC malformed bounds, allocation-free decode, failure cleanup: passed");
}
