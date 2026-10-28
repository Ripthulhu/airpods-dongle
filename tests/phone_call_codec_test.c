// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "classic/btstack_sbc_bluedroid.h"
#include "classic/hfp_codec.h"

static btstack_sbc_decoder_bluedroid_t decoder;
static btstack_sbc_encoder_bluedroid_t encoder;
static hfp_codec_t framing;
static unsigned received_frames;
static uint64_t energy;

void hci_dump_log(int level, const char *format, ...) { (void)level; (void)format; }

void btstack_assert_failed(const char *file, uint16_t line) {
    fprintf(stderr, "BTstack assertion: %s:%u\n", file, line);
    abort();
}

static void receive(int16_t *pcm, int frames, int channels, int rate, void *context) {
    assert(context == &received_frames && frames == 120 && channels == 1 && rate == 16000);
    ++received_frames;
    for (int i = 0; i < frames; ++i) energy += (uint64_t)((int64_t)pcm[i] * pcm[i]);
}

static const btstack_sbc_decoder_t *reset_decoder(void) {
    memset(&decoder, 0xa5, sizeof(decoder));
    const btstack_sbc_decoder_t *instance = btstack_sbc_decoder_bluedroid_init_instance(&decoder);
    assert(instance != NULL);
    // A recycled decoder must not retain mSBC loss debt, buffers or counters.
    const uint8_t *bytes = (const uint8_t *)&decoder;
    for (size_t i = 0; i < sizeof(decoder); ++i) assert(bytes[i] == 0);
    instance->configure(&decoder, SBC_MODE_mSBC, receive, &received_frames);
    return instance;
}

int main(void) {
    const btstack_sbc_decoder_t *instance = reset_decoder();
    const btstack_sbc_encoder_t *encoding = btstack_sbc_encoder_bluedroid_init_instance(&encoder);
    assert(encoding != NULL);
    hfp_codec_init_msbc_with_codec(&framing, encoding, &encoder);
    assert(hfp_codec_num_audio_samples_per_frame(&framing) == 120);
    static const uint8_t h2_sequence[] = {0x08, 0x38, 0xc8, 0xf8};
    for (unsigned frame = 0; frame < 60; ++frame) {
        int16_t pcm[120];
        for (unsigned i = 0; i < 120; ++i)
            pcm[i] = (int16_t)(6000.0 * sin((frame * 120u + i) * 0.39269908169872414));
        assert(hfp_codec_can_encode_audio_frame_now(&framing));
        hfp_codec_encode_audio_frame(&framing, pcm);
        assert(hfp_codec_num_bytes_available(&framing) == 60);
        uint8_t packet[60];
        hfp_codec_read_from_stream(&framing, packet, 60);
        assert(packet[0] == 1 && packet[1] == h2_sequence[frame & 3u]);
        assert(packet[2] == 0xad && packet[59] == 0);
        unsigned split = 1u + frame % 59u;
        instance->decode_signed_16(&decoder, 0, packet, (uint16_t)split);
        instance->decode_signed_16(&decoder, 0, packet + split, (uint16_t)(60u - split));
    }
    assert(received_frames == 60 && decoder.good_frames_nr == 60 && energy > 1000000u);
    uint8_t lost[60] = {0};
    instance->decode_signed_16(&decoder, 2, lost, sizeof(lost));
    assert(decoder.bad_frames_nr != 0 || decoder.zero_frames_nr != 0 || decoder.msbc_bad_bytes != 0);
    (void)reset_decoder();
    assert(decoder.good_frames_nr == 0 && decoder.bad_frames_nr == 0 && decoder.msbc_bad_bytes == 0);
    printf("real SDK mSBC: H2 sequence, all fragment boundaries, mono/16k PCM, PLC and recycled reset passed; encoder=%u decoder=%u bytes\n",
           (unsigned)sizeof(encoder), (unsigned)sizeof(decoder));
}
