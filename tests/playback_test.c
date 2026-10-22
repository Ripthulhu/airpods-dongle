#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/btstack/playback_eld.h"
#include "../src/btstack/playback_pcm.h"
#include "../src/btstack/playback_peer.h"
#include "../src/btstack/playback_format.h"

static playback_pcm_t pcm;

static void test_priming_preserves_stereo_onset(void) {
    assert(playback_pcm_reset(&pcm, 480 * 16, true));
    int16_t in[960], out[960];
    uint32_t source = 0;
    for (unsigned frame = 0; frame < 40; ++frame) {
        for (unsigned i = 0; i < 480; ++i) {
            in[i * 2] = (int16_t)++source;
            in[i * 2 + 1] = (int16_t)-in[i * 2];
        }
        playback_pcm_push(&pcm, in, 480);
        uint32_t copied = playback_pcm_read(&pcm, out, 480);
        assert(pcm.used <= 4800);
        if (frame < 10) {
            assert(copied == 0);
            for (unsigned i = 0; i < 960; ++i) assert(out[i] == 0);
        } else {
            assert(copied == 480);
            for (unsigned i = 0; i < 480; ++i) {
                int16_t expected = (int16_t)((frame - 10) * 480 + i + 1);
                assert(out[i * 2] == expected);
                assert(out[i * 2 + 1] == (int16_t)-expected);
            }
        }
    }
}

static void test_short_source_and_reset(void) {
    assert(playback_pcm_reset(&pcm, 7680, true));
    int16_t in[] = {1, -1, 2, -2, 3, -3};
    int16_t out[960];
    playback_pcm_push(&pcm, in, 3);
    for (unsigned i = 0; i < 10; ++i) {
        assert(playback_pcm_read(&pcm, out, 480) == 0);
    }
    assert(playback_pcm_read(&pcm, out, 480) == 3);
    assert(memcmp(in, out, sizeof(in)) == 0);
    for (unsigned i = 6; i < 960; ++i) assert(out[i] == 0);
    playback_pcm_push(&pcm, in, 3);
    assert(playback_pcm_reset(&pcm, 7680, false));
    assert(playback_pcm_read(&pcm, out, 480) == 0);
    assert(!playback_pcm_reset(&pcm, 0, false));
    assert(!playback_pcm_reset(&pcm, PLAYBACK_PCM_FRAMES + 1, false));
    assert(!playback_pcm_reset(&pcm, 4799, true));
}

static void test_overflow_and_usb_chunk_boundaries(void) {
    assert(playback_pcm_reset(&pcm, 8, false));
    int16_t in[] = {1,-1,2,-2,3,-3,4,-4,5,-5,6,-6,7,-7,8,-8,9,-9,10,-10};
    int16_t out[20];
    playback_pcm_push(&pcm, in, 5);
    assert(playback_pcm_read(&pcm, out, 3) == 3);
    assert(memcmp(in, out, 12) == 0);
    playback_pcm_push(&pcm, in + 10, 5);
    assert(playback_pcm_read(&pcm, out, 8) == 7);
    assert(memcmp(in + 6, out, 28) == 0);
    assert(out[14] == 0 && out[15] == 0);
    playback_pcm_push(&pcm, in, 10);
    assert(pcm.used == 8);
    assert(playback_pcm_read(&pcm, out, 8) == 8);
    assert(memcmp(in + 4, out, 32) == 0);
    playback_pcm_push(&pcm, in, 7);
    playback_pcm_push(&pcm, in + 14, 3);
    assert(playback_pcm_read(&pcm, out, 8) == 8);
    assert(memcmp(in + 4, out, 32) == 0);
}

static void test_pcm_matches_linear_queue(void) {
    static int16_t queue[PLAYBACK_PCM_FRAMES * 2];
    static int16_t input[(PLAYBACK_PCM_FRAMES + 31) * 2];
    int16_t out[1100 * 2], expected[1100 * 2];
    const uint32_t capacities[] = {1, 7, 8, 17, 128, 480, 1024, 7680, 8192};
    const uint32_t chunks[] = {0, 1, 47, 48, 128, 480, 1024, 8192 + 31};
    for (unsigned i = 0; i < sizeof(input) / sizeof(input[0]); ++i) input[i] = (int16_t)(i * 37);
    uint32_t random = 12345;
    for (unsigned c = 0; c < sizeof(capacities) / sizeof(capacities[0]); ++c) {
        uint32_t capacity = capacities[c], used = 0;
        uint32_t priming = capacity >= PLAYBACK_PRIMING_FRAMES ? PLAYBACK_PRIMING_FRAMES : 0;
        assert(playback_pcm_reset(&pcm, capacity, priming != 0));
        for (unsigned action = 0; action < 2048; ++action) {
            random = random * 1664525u + 1013904223u;
            if (random & 0x100) {
                uint32_t count = chunks[(random >> 16) % (sizeof(chunks) / sizeof(chunks[0]))];
                playback_pcm_push(&pcm, input, count);
                const int16_t *source = input;
                if (count > capacity) { source += (count - capacity) * 2; count = capacity; }
                uint32_t drop = count > capacity - used ? count - (capacity - used) : 0;
                memmove(queue, queue + drop * 2, (size_t)(used - drop) * 2 * sizeof(*queue));
                used -= drop;
                memcpy(queue + used * 2, source, (size_t)count * 2 * sizeof(*queue));
                used += count;
            } else {
                uint32_t count = (random >> 16) % 1101;
                uint32_t prefix = priming < count ? priming : count;
                priming -= prefix;
                uint32_t copied = count - prefix < used ? count - prefix : used;
                memset(expected, 0, sizeof(expected));
                memcpy(expected + prefix * 2, queue, (size_t)copied * 2 * sizeof(*queue));
                memmove(queue, queue + copied * 2, (size_t)(used - copied) * 2 * sizeof(*queue));
                used -= copied;
                memset(out, 0xa5, sizeof(out));
                assert(playback_pcm_read(&pcm, out, count) == copied);
                assert(memcmp(out, expected, (size_t)count * 2 * sizeof(*out)) == 0);
            }
            assert(pcm.used == used && pcm.priming == priming && pcm.read < capacity);
        }
    }
}

static void test_eld_negotiation(void) {
    uint8_t cap[] = {0x4c,0,0,0,1,0x80,0,0x80,0,0x8c,0,0x83,0xe8,0};
    uint8_t config[14];
    for (size_t n = 0; n < sizeof(cap); ++n) assert(!playback_eld_negotiate(cap, n, config));
    assert(playback_eld_negotiate(cap, sizeof(cap), config));
    assert(playback_eld_configuration(config, sizeof(config)));
    assert(config[9] == 0x84 && config[11] == 3);
    assert(playback_eld_bitrate(config) == 256000);
    cap[11] = 0x82; cap[12] = 0xee; cap[13] = 0;
    assert(playback_eld_negotiate(cap, sizeof(cap), config));
    assert(playback_eld_bitrate(config) == 192000);
    cap[11] = 0x81; cap[12] = 0; cap[13] = 0;
    assert(!playback_eld_negotiate(cap, sizeof(cap), config));
    cap[11] = cap[12] = cap[13] = 0;
    assert(playback_eld_negotiate(cap, sizeof(cap), config));
    assert(playback_eld_bitrate(config) == 256000);
    cap[9] = 0x88; // No stereo support.
    assert(!playback_eld_negotiate(cap, sizeof(cap), config));
    config[11] |= 0x80;
    assert(!playback_eld_configuration(config, sizeof(config)));
}

static void test_eld_packet_bounds_and_sequence_wrap(void) {
    uint8_t packet[PLAYBACK_ELD_PAYLOAD_CEILING];
    memset(packet, 0xa5, sizeof(packet));
    uint16_t used = 0, sequence = 4095;
    assert(playback_eld_append(packet, sizeof(packet), &used, &sequence, 320));
    assert(used == 324 && sequence == 0);
    assert(packet[0] == 0xbf && packet[1] == 0xff && packet[2] == 0x11 && packet[3] == 0x40);
    assert(playback_eld_append(packet, sizeof(packet), &used, &sequence, 320));
    assert(playback_eld_append(packet, sizeof(packet), &used, &sequence, 320));
    assert(used == 972 && sequence == 2);
    uint8_t before[PLAYBACK_ELD_PAYLOAD_CEILING];
    memcpy(before, packet, sizeof(packet));
    assert(!playback_eld_append(packet, sizeof(packet), &used, &sequence, 320));
    assert(used == 972 && sequence == 2 && memcmp(before, packet, sizeof(packet)) == 0);
    used = 985;
    assert(!playback_eld_append(packet, sizeof(packet), &used, &sequence, 1));
    used = 0;
    assert(!playback_eld_append(packet, sizeof(packet), &used, &sequence, 0));
    assert(!playback_eld_append(packet, sizeof(packet), &used, &sequence, 2048));
    assert(playback_eld_append(packet, 324, &used, &sequence, 320));
    assert(used == 324);
}

static void test_peer_fences_cancelled_and_old_connections(void) {
    const uint8_t selected[6] = {1,2,3,4,5,6};
    const uint8_t other[6] = {7,8,9,10,11,12};
    assert(playback_peer_accept(selected, selected, 42, 42, true, true));
    assert(playback_peer_accept(selected, selected, 0, 43, true, false));
    assert(!playback_peer_accept(selected, selected, 42, 43, true, true));
    assert(!playback_peer_accept(selected, other, 42, 42, true, true));
    assert(!playback_peer_accept(selected, selected, 42, 42, false, false));
    assert(!playback_peer_accept(selected, selected, 0, 42, true, true));
    assert(!playback_peer_accept(selected, selected, 0, 0, true, false));
    assert(!playback_peer_accept(NULL, selected, 0, 42, true, false));
    const uint8_t erased[6] = {255,255,255,255,255,255};
    assert(!playback_peer_accept(erased, erased, 0, 42, true, false));
}

static void test_fixed_usb_format_and_codec_transition(void) {
    assert(playback_format_supported(48000, 2));
    assert(!playback_format_supported(44100, 2));
    assert(!playback_format_supported(96000, 2));
    assert(!playback_format_supported(48000, 1));
    assert(!playback_format_supported(48000, 0));
    uint16_t samples = playback_aac_frame_samples(48000, 2, 480, true);
    assert(samples == 480);
    samples = playback_aac_frame_samples(48000, 2, 1024, false);
    assert(samples == 1024);
    assert(playback_aac_frame_samples(48000, 2, 480, false) == 0);
    assert(playback_aac_frame_samples(48000, 2, 1024, true) == 0);
    assert(playback_aac_frame_samples(48000, 1, 1024, false) == 0);
    assert(playback_aac_frame_samples(44100, 2, 1024, false) == 0);
    assert(playback_aac_frame_samples(48000, 2, 65536, false) == 0);
}

int main(void) {
    test_priming_preserves_stereo_onset();
    test_short_source_and_reset();
    test_overflow_and_usb_chunk_boundaries();
    test_pcm_matches_linear_queue();
    test_eld_negotiation();
    test_eld_packet_bounds_and_sequence_wrap();
    test_peer_fences_cancelled_and_old_connections();
    test_fixed_usb_format_and_codec_transition();
    puts("playback tests passed");
    return 0;
}
