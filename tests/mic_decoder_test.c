// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <aacdecoder_lib.h>
#include "../src/btstack/aacp_mic_dec.h"

char __StackLimit[1];
static CStreamInfo info = {48000, 480, 1, 39};
static bool fill_error, partial_fill, decode_error, no_info, config_error;
static unsigned closed;
static unsigned decoded, reset_count;
static UINT last_flags;

HANDLE_AACDECODER aacDecoder_Open(int transport, UINT layers) {
    assert(transport == TT_MP4_RAW && layers == 1);
    return &info;
}
void aacDecoder_Close(HANDLE_AACDECODER decoder) { assert(decoder == &info); ++closed; }
AAC_DECODER_ERROR aacDecoder_ConfigRaw(HANDLE_AACDECODER decoder, UCHAR **config, UINT *size) {
    const UCHAR expected[] = {0xf8, 0xe6, 0x30, 0x00};
    assert(decoder == &info && *size == sizeof(expected));
    assert(memcmp(*config, expected, sizeof(expected)) == 0);
    assert(!aacp_mic_dec_ready()); // no half-configured handle is published to BTstack
    return config_error ? 1 : AAC_DEC_OK;
}
AAC_DECODER_ERROR aacDecoder_SetParam(HANDLE_AACDECODER decoder, int param, int value) {
    assert(decoder == &info && param == AAC_TPDEC_CLEAR_BUFFER && value == 1);
    ++reset_count;
    return AAC_DEC_OK;
}
AAC_DECODER_ERROR aacDecoder_Fill(HANDLE_AACDECODER decoder, UCHAR **input, UINT *size, UINT *valid) {
    assert(decoder == &info && input != NULL && size != NULL && *size == *valid);
    *valid = partial_fill ? 1 : 0;
    return fill_error ? 1 : AAC_DEC_OK;
}
AAC_DECODER_ERROR aacDecoder_DecodeFrame(HANDLE_AACDECODER decoder, INT_PCM *pcm, int capacity, UINT flags) {
    assert(decoder == &info && capacity >= 480);
    ++decoded;
    last_flags = flags;
    for (int i = 0; i < 480; ++i) pcm[i] = (int16_t)(i + 1);
    return decode_error ? 1 : AAC_DEC_OK;
}
CStreamInfo *aacDecoder_GetStreamInfo(HANDLE_AACDECODER decoder) {
    assert(decoder == &info);
    return no_info ? NULL : &info;
}
uint32_t time_us_32(void) { static uint32_t now; return ++now; }

int main(void) {
    const uint8_t au[] = {1, 2, 3};
    int16_t output[480];
    assert(!aacp_mic_dec_ready());
    config_error = true;
    assert(!aacp_mic_dec_init() && !aacp_mic_dec_ready() && closed == 1);
    config_error = false;
    assert(aacp_mic_dec_init() && aacp_mic_dec_ready());
    assert(aacp_mic_dec_reset());
    assert(aacp_mic_pcm_read(output, 480) == 0);
    assert(aacp_mic_dec_decode(au, sizeof(au)));
    assert(last_flags == AACDEC_CLRHIST);
    assert(aacp_mic_pcm_read(output, 480) == 480);
    for (int i = 0; i < 480; ++i) assert(output[i] == i + 1);
    assert(aacp_mic_dec_decode(au, sizeof(au)) && last_flags == 0);

    // A changed format must discard previously queued PCM, not play it as mono.
    int *invalid_fields[] = {&info.numChannels, &info.frameSize, &info.sampleRate, &info.aot};
    const int wrong[] = {2, 960, 64000, 2};
    for (unsigned i = 0; i < 4; ++i) {
        int original = *invalid_fields[i];
        *invalid_fields[i] = wrong[i];
        unsigned before = reset_count;
        assert(!aacp_mic_dec_decode(au, sizeof(au)) && reset_count == before + 1);
        assert(aacp_mic_pcm_read(output, 480) == 0);
        *invalid_fields[i] = original;
        assert(aacp_mic_dec_decode(au, sizeof(au)) && last_flags == AACDEC_CLRHIST);
    }
    fill_error = true;
    unsigned before = decoded;
    assert(!aacp_mic_dec_decode(au, sizeof(au)) && decoded == before);
    fill_error = false;
    partial_fill = true;
    assert(!aacp_mic_dec_decode(au, sizeof(au)) && decoded == before);
    partial_fill = false;
    decode_error = true;
    assert(!aacp_mic_dec_decode(au, sizeof(au)));
    decode_error = false;
    no_info = true;
    assert(!aacp_mic_dec_decode(au, sizeof(au)));
    no_info = false;
    assert(!aacp_mic_dec_decode(NULL, sizeof(au)));
    assert(!aacp_mic_dec_decode(au, 0));
    assert(!aacp_mic_dec_decode(au, 256));
    assert(aacp_mic_pcm_read(output, 480) == 0);
    assert(aacp_mic_dec_decode(au, sizeof(au)) && last_flags == AACDEC_CLRHIST);
    assert(aacp_mic_pcm_read(output, 480) == 480);
    puts("mic decoder format validation and interruption recovery: passed");
}
