/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AIRPODS_CODEC_PROFILE_CHECKS_H
#define AIRPODS_CODEC_PROFILE_CHECKS_H

#include <string.h>

static void profile_require(int condition, const char *name) {
    if (!condition) fail(name, -1);
}

static void profile_configure(HANDLE_AACDECODER decoder) {
    UCHAR config[] = {0xf8, 0xe6, 0x30, 0};
    UCHAR *configs[] = {config};
    UINT length = sizeof(config);
    DEC_OK(aacDecoder_ConfigRaw(decoder, configs, &length));
}

static AAC_DECODER_ERROR profile_decode(HANDLE_AACDECODER decoder,
                                         const unsigned char *packet,
                                         unsigned length, UINT flags) {
    UCHAR *data[] = {(UCHAR *)packet};
    UINT sizes[] = {length}, remaining = length;
    DEC_OK(aacDecoder_Fill(decoder, data, sizes, &remaining));
    profile_require(remaining == 0, "profile input remaining");
    struct { INT_PCM before[8], samples[480], after[8]; } output;
    memset(&output, 0x5a, sizeof(output));
    AAC_DECODER_ERROR error = aacDecoder_DecodeFrame(decoder, output.samples, 480, flags);
    for (unsigned i = 0; i < 8; ++i) {
        profile_require(output.before[i] == (INT_PCM)0x5a5a &&
                        output.after[i] == (INT_PCM)0x5a5a, "profile output canary");
    }
    if (error == AAC_DEC_OK) {
        CStreamInfo *info = aacDecoder_GetStreamInfo(decoder);
        profile_require(info != NULL && info->numChannels == 1 &&
                        info->sampleRate == 48000 && info->frameSize == 480 &&
                        info->aot == 39, "profile decoded format");
    }
    return error;
}

static void profile_checks(const unsigned char fixtures[16][1024],
                             const unsigned lengths[16]) {
#ifndef FDK_AIRPODS_PROFILE
#error "Profile regression checks must use the firmware codec profile"
#endif
    unsigned before = mallinfo().uordblks;
    for (unsigned channels = 0; channels <= 8; ++channels) {
        if (channels == 1 || channels == 2) continue;
        HANDLE_AACENCODER invalid = NULL;
        profile_require(aacEncOpen(&invalid, 1, channels) == AACENC_INVALID_CONFIG &&
                        invalid == NULL, "profile encoder channel guard");
    }
    HANDLE_AACENCODER encoder = NULL;
    profile_require(aacEncOpen(&encoder, 0, 2) == AACENC_INVALID_CONFIG && encoder == NULL,
                    "profile encoder module guard");
    ENC_OK(aacEncOpen(&encoder, 1, 2));
    profile_require(aacEncoder_SetParam(encoder, AACENC_CHANNELMODE, MODE_1_2) != AACENC_OK,
                    "profile encoder mode guard");
    ENC_OK(aacEncClose(&encoder));
    profile_require(aacDecoder_Open(TT_MP4_ADTS, 1) == NULL &&
                    aacDecoder_Open(TT_MP4_RAW, 0) == NULL &&
                    aacDecoder_Open(TT_MP4_RAW, 2) == NULL, "profile transport guard");

    HANDLE_AACDECODER decoder = aacDecoder_Open(TT_MP4_RAW, 1);
    profile_require(decoder != NULL, "profile decoder open");
    UCHAR wrong[5] = {0xf8, 0xe6, 0x30, 0, 0};
    UCHAR *configs[] = {wrong};
    for (UINT length = 0; length <= sizeof(wrong); ++length) {
        if (length == 4) continue;
        profile_require(aacDecoder_ConfigRaw(decoder, configs, &length) != AAC_DEC_OK,
                        "profile configuration length guard");
    }
    for (unsigned byte = 0; byte < 4; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            wrong[byte] ^= (UCHAR)(1u << bit);
            UINT length = 4;
            profile_require(aacDecoder_ConfigRaw(decoder, configs, &length) != AAC_DEC_OK,
                            "profile configuration value guard");
            wrong[byte] ^= (UCHAR)(1u << bit);
        }
    }
    profile_require(aacDecoder_ConfigRaw(decoder, NULL, NULL) != AAC_DEC_OK,
                    "profile null configuration guard");
    profile_configure(decoder);
    wrong[0] ^= 1;
    UINT reconfigure_length = 4;
    profile_require(aacDecoder_ConfigRaw(decoder, configs, &reconfigure_length) != AAC_DEC_OK,
                    "profile active reconfiguration guard");
    wrong[0] ^= 1;
    profile_require(aacDecoder_SetParam(decoder, AAC_PCM_MIN_OUTPUT_CHANNELS, 2) != AAC_DEC_OK &&
                    aacDecoder_SetParam(decoder, AAC_PCM_MAX_OUTPUT_CHANNELS, 2) != AAC_DEC_OK,
                    "profile output channel guard");

    unsigned first = 0;
    while (first < 16 && lengths[first] == 0) ++first;
    profile_require(first < 16 && lengths[first] > 3 && lengths[first] <= 255,
                    "profile microphone fixture size");
    DEC_OK(profile_decode(decoder, fixtures[first], lengths[first], AACDEC_CLRHIST));
    UCHAR malformed[256];
    memset(malformed, 0xa5, sizeof(malformed));
    UCHAR *bad[] = {malformed};
    UINT size = sizeof(malformed), remaining = size;
    profile_require(aacDecoder_Fill(decoder, bad, &size, &remaining) != AAC_DEC_OK &&
                    remaining == size, "profile access-unit size guard");
    size = 1;
    remaining = 2;
    profile_require(aacDecoder_Fill(decoder, bad, &size, &remaining) != AAC_DEC_OK &&
                    remaining == 2, "profile inconsistent input guard");

    const unsigned truncated[] = {1, 2, 3, lengths[first] / 2, lengths[first] - 1};
    for (unsigned i = 0; i < sizeof(truncated) / sizeof(*truncated); ++i) {
        DEC_OK(aacDecoder_SetParam(decoder, AAC_TPDEC_CLEAR_BUFFER, 1));
        (void)profile_decode(decoder, fixtures[first], truncated[i], AACDEC_CLRHIST);
        DEC_OK(aacDecoder_SetParam(decoder, AAC_TPDEC_CLEAR_BUFFER, 1));
        DEC_OK(profile_decode(decoder, fixtures[first], lengths[first], AACDEC_CLRHIST));
    }
    for (unsigned pattern = 0; pattern < 4; ++pattern) {
        memset(malformed, (int)(pattern * 85), 255);
        DEC_OK(aacDecoder_SetParam(decoder, AAC_TPDEC_CLEAR_BUFFER, 1));
        (void)profile_decode(decoder, malformed, 255, AACDEC_CLRHIST);
        DEC_OK(aacDecoder_SetParam(decoder, AAC_TPDEC_CLEAR_BUFFER, 1));
        DEC_OK(profile_decode(decoder, fixtures[first], lengths[first], AACDEC_CLRHIST));
    }
    DEC_OK(aacDecoder_SetParam(decoder, AAC_PCM_LIMITER_ENABLE, 1));
    DEC_OK(aacDecoder_SetParam(decoder, AAC_TPDEC_CLEAR_BUFFER, 1));
    DEC_OK(profile_decode(decoder, fixtures[first], lengths[first], AACDEC_CLRHIST));
    aacDecoder_Close(decoder);
    profile_require(mallinfo().uordblks == before, "profile cleanup leak");
    probe_emit("PROFILE guards, truncated/malformed input, reset recovery, limiter and canaries passed\n");
}

#endif
