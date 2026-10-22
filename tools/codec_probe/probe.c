// SPDX-License-Identifier: GPL-3.0-only
#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "aacenc_lib.h"
#include "aacdecoder_lib.h"

#ifndef PROBE_HEAP_BYTES
#error "Pass the firmware's linked heap capacity as PROBE_HEAP_BYTES"
#endif

extern void probe_emit(const char *text);
extern void _exit(int code) __attribute__((noreturn));
extern char _end;
static char *heap_break;

void *_sbrk(ptrdiff_t increment) {
    if (heap_break == NULL) heap_break = &_end;
    char *previous = heap_break;
    if ((increment >= 0 && (size_t)increment >
         PROBE_HEAP_BYTES - (size_t)(heap_break - &_end)) ||
        (increment < 0 && increment < &_end - heap_break)) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_break += increment;
    return previous;
}

static void report_heap(const char *name) {
    struct mallinfo info = mallinfo();
    char text[160];
    snprintf(text, sizeof(text), "HEAP %s arena=%u inuse=%u free=%u\n",
             name, (unsigned)info.arena, (unsigned)info.uordblks,
             (unsigned)info.fordblks);
    probe_emit(text);
}

static void fail(const char *operation, int code) {
    char text[160];
    snprintf(text, sizeof(text), "FAIL %s %x\n", operation, code);
    probe_emit(text);
    _exit(1);
}

#define ENC_OK(operation) do { \
    int probe_codec_result = (operation); \
    if (probe_codec_result != AACENC_OK) fail(#operation, probe_codec_result); \
} while (0)
#define DEC_OK(operation) do { \
    int probe_codec_result = (operation); \
    if (probe_codec_result != AAC_DEC_OK) fail(#operation, probe_codec_result); \
} while (0)

static uint32_t *watermark_bottom, *watermark_top;

__attribute__((noinline)) static void watermark_start(void) {
    uintptr_t stack_pointer;
    __asm__ volatile("mov %0,sp" : "=r"(stack_pointer));
    watermark_bottom = (uint32_t *)(stack_pointer - 131072);
    watermark_top = (uint32_t *)(stack_pointer - 128);
    for (uint32_t *p = watermark_bottom; p < watermark_top; ++p)
        *p = 0xa5a5a5a5u;
    __asm__ volatile("" ::: "memory");
}

static unsigned watermark_depth(void) {
    __asm__ volatile("" ::: "memory");
    uint32_t *p = watermark_bottom;
    while (p < watermark_top && *p == 0xa5a5a5a5u) ++p;
    if (p == watermark_bottom) fail("stack watermark exhausted", -1);
    return (unsigned)((uintptr_t)watermark_top - (uintptr_t)p);
}

static void report_stack(const char *name, unsigned depth) {
    char text[100];
    snprintf(text, sizeof(text), "STACK %s below_mark=%u\n", name, depth);
    probe_emit(text);
}

static HANDLE_AACENCODER encoder(unsigned channels, unsigned bitrate) {
    HANDLE_AACENCODER handle = NULL;
    ENC_OK(aacEncOpen(&handle, 0x01, channels));
    const struct { AACENC_PARAM key; UINT value; } parameters[] = {
        {AACENC_AOT, 39}, {AACENC_BITRATE, bitrate}, {AACENC_SAMPLERATE, 48000},
        {AACENC_CHANNELMODE, channels == 2 ? MODE_2 : MODE_1},
        {AACENC_GRANULE_LENGTH, 480}, {AACENC_SBR_MODE, 0}, {AACENC_BITRATEMODE, 0},
        {AACENC_PEAK_BITRATE, bitrate}, {AACENC_AFTERBURNER, 0},
        {AACENC_TRANSMUX, TT_MP4_RAW},
    };
    for (unsigned i = 0; i < sizeof(parameters) / sizeof(*parameters); ++i)
        ENC_OK(aacEncoder_SetParam(handle, parameters[i].key, parameters[i].value));
    ENC_OK(aacEncEncode(handle, NULL, NULL, NULL, NULL));
    return handle;
}

static INT_PCM pcm[960];
static unsigned char packets[16][1024];
static unsigned packet_lengths[16];
static unsigned checksum = 2166136261u;

static void encode_frame(HANDLE_AACENCODER handle, unsigned channels, unsigned frame,
                         unsigned char *packet, unsigned *length) {
    for (unsigned sample = 0; sample < 480; ++sample) {
        int value = (sample + frame * 480) % 96;
        value = (value < 48 ? value : 96 - value) - 24;
        for (unsigned channel = 0; channel < channels; ++channel)
            pcm[sample * channels + channel] = (INT_PCM)(value * 100);
    }
    void *input_buffers[] = {pcm}, *output_buffers[] = {packet};
    INT input_ids[] = {IN_AUDIO_DATA}, output_ids[] = {OUT_BITSTREAM_DATA};
    INT input_sizes[] = {480 * channels * sizeof(INT_PCM)}, output_sizes[] = {1024};
    INT input_elements[] = {sizeof(INT_PCM)}, output_elements[] = {1};
    AACENC_BufDesc input = {1, input_buffers, input_ids, input_sizes, input_elements};
    AACENC_BufDesc output = {1, output_buffers, output_ids, output_sizes, output_elements};
    AACENC_InArgs arguments = {0};
    AACENC_OutArgs result = {0};
    arguments.numInSamples = 480 * channels;
    ENC_OK(aacEncEncode(handle, &input, &output, &arguments, &result));
    if (result.numInSamples != (INT)(480 * channels) || result.numOutBytes < 0 ||
        result.numOutBytes > 1024) fail("encode bounds", result.numOutBytes);
    *length = result.numOutBytes;
    for (unsigned i = 0; i < *length; ++i)
        checksum = (checksum ^ packet[i]) * 16777619u;
}

int main(void) {
    report_heap("baseline");
    HANDLE_AACENCODER mono = encoder(1, 64000);
    AACENC_InfoStruct info = {0};
    ENC_OK(aacEncInfo(mono, &info));
    if (info.frameLength != 480 || info.inputChannels != 1 || info.confSize != 4 ||
        info.confBuf[0] != 0xf8 || info.confBuf[1] != 0xe6 ||
        info.confBuf[2] != 0x30 || info.confBuf[3] != 0)
        fail("generated microphone configuration", info.frameLength);
    for (unsigned frame = 0; frame < 16; ++frame)
        encode_frame(mono, 1, frame, packets[frame], &packet_lengths[frame]);
    ENC_OK(aacEncClose(&mono));
    report_heap("fixture_encoder_closed");

    watermark_start();
    HANDLE_AACENCODER stereo = encoder(2, 256000);
    report_stack("stereo_init", watermark_depth());
    report_heap("stereo_ready");
    unsigned char packet[1024];
    unsigned length;
    watermark_start();
    encode_frame(stereo, 2, 0, packet, &length);
    report_stack("stereo_encode", watermark_depth());
    report_heap("stereo_first_frame");

    watermark_start();
    HANDLE_AACDECODER decoder = aacDecoder_Open(TT_MP4_RAW, 1);
    report_stack("decoder_open", watermark_depth());
    if (decoder == NULL) fail("decoder open", -1);
    report_heap("plus_decoder_open");
    UCHAR configuration[] = {0xf8, 0xe6, 0x30, 0};
    UCHAR *configurations[] = {configuration};
    UINT configuration_length = sizeof(configuration);
    watermark_start();
    DEC_OK(aacDecoder_ConfigRaw(decoder, configurations, &configuration_length));
    report_stack("decoder_config", watermark_depth());
    report_heap("plus_decoder_config");

    INT_PCM decoded[480];
    unsigned successful = 0, maximum_depth = 0;
    for (unsigned frame = 0; frame < 16; ++frame) {
        if (packet_lengths[frame] == 0) continue;
        UCHAR *data[] = {packets[frame]};
        UINT sizes[] = {packet_lengths[frame]}, remaining = packet_lengths[frame];
        DEC_OK(aacDecoder_Fill(decoder, data, sizes, &remaining));
        if (remaining != 0) fail("decoder input remaining", remaining);
        watermark_start();
        DEC_OK(aacDecoder_DecodeFrame(decoder, decoded, 480,
                                      frame == 0 ? AACDEC_CLRHIST : 0));
        unsigned depth = watermark_depth();
        if (depth > maximum_depth) maximum_depth = depth;
        CStreamInfo *stream = aacDecoder_GetStreamInfo(decoder);
        if (stream->numChannels != 1 || stream->frameSize != 480 ||
            stream->sampleRate != 48000 || stream->aot != 39)
            fail("decoded format", stream->frameSize);
        for (unsigned sample = 0; sample < 480; ++sample) {
            uint16_t value = (uint16_t)decoded[sample];
            checksum = (checksum ^ (value & 255)) * 16777619u;
            checksum = (checksum ^ (value >> 8)) * 16777619u;
        }
        ++successful;
    }
    report_stack("decoder_frame_max", maximum_depth);
    report_heap("stereo_and_decoder_after_decode");
    char text[100];
    snprintf(text, sizeof(text), "SUCCESS frames=%u checksum=%08x\n", successful, checksum);
    probe_emit(text);
    aacDecoder_Close(decoder);
    ENC_OK(aacEncClose(&stereo));
    report_heap("closed");
    return successful == 16 && checksum == 0x4105af5du ? 0 : 1;
}
