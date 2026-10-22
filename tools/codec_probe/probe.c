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
static size_t peak_heap_break;

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
    size_t used = (size_t)(heap_break - &_end);
    if (used > peak_heap_break) peak_heap_break = used;
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

static HANDLE_AACENCODER playback_encoder(unsigned aot, unsigned bitrate) {
    if (aot == 39) return encoder(2, bitrate);
    HANDLE_AACENCODER handle = NULL;
    ENC_OK(aacEncOpen(&handle, 1, 2));
    const struct { AACENC_PARAM key; UINT value; } parameters[] = {
        {AACENC_AOT, aot}, {AACENC_BITRATE, bitrate}, {AACENC_SAMPLERATE, 48000},
        {AACENC_CHANNELMODE, MODE_2}, {AACENC_BITRATEMODE, 0}, {AACENC_AFTERBURNER, 0},
        {AACENC_TRANSMUX, TT_MP4_LATM_MCP1}, {AACENC_HEADER_PERIOD, 1},
        {AACENC_AUDIOMUXVER, 1},
    };
    for (unsigned i = 0; i < sizeof(parameters) / sizeof(*parameters); ++i)
        ENC_OK(aacEncoder_SetParam(handle, parameters[i].key, parameters[i].value));
    ENC_OK(aacEncEncode(handle, NULL, NULL, NULL, NULL));
    return handle;
}

static INT_PCM pcm[2048];
static unsigned char packets[16][1024];
static unsigned packet_lengths[16];
static unsigned checksum = 2166136261u;

static void encode_frame(HANDLE_AACENCODER handle, unsigned channels, unsigned frame,
                         unsigned char *packet, unsigned *length) {
    AACENC_InfoStruct info = {0};
    ENC_OK(aacEncInfo(handle, &info));
    unsigned samples = info.frameLength;
    if (channels != info.inputChannels || samples == 0 || samples > 1024 || channels > 2)
        fail("encoder input format", samples);
    for (unsigned sample = 0; sample < samples; ++sample) {
        int value = (sample + frame * samples) % 96;
        value = (value < 48 ? value : 96 - value) - 24;
        for (unsigned channel = 0; channel < channels; ++channel)
            pcm[sample * channels + channel] = (INT_PCM)(value * 100);
    }
    void *input_buffers[] = {pcm}, *output_buffers[] = {packet};
    INT input_ids[] = {IN_AUDIO_DATA}, output_ids[] = {OUT_BITSTREAM_DATA};
    INT input_sizes[] = {samples * channels * sizeof(INT_PCM)}, output_sizes[] = {1024};
    INT input_elements[] = {sizeof(INT_PCM)}, output_elements[] = {1};
    AACENC_BufDesc input = {1, input_buffers, input_ids, input_sizes, input_elements};
    AACENC_BufDesc output = {1, output_buffers, output_ids, output_sizes, output_elements};
    AACENC_InArgs arguments = {0};
    AACENC_OutArgs result = {0};
    arguments.numInSamples = samples * channels;
    ENC_OK(aacEncEncode(handle, &input, &output, &arguments, &result));
    if (result.numInSamples != (INT)(samples * channels) || result.numOutBytes < 0 ||
        result.numOutBytes > 1024) fail("encode bounds", result.numOutBytes);
    *length = result.numOutBytes;
    for (unsigned i = 0; i < *length; ++i)
        checksum = (checksum ^ packet[i]) * 16777619u;
}

#include "profile_checks.h"

static void check_encoder_matrix(void) {
    static const struct { unsigned aot, bitrate, bytes, hash; } cases[] = {
        {2, 96000, 25154, 0x4b593030}, {2, 128000, 33372, 0x6695ac06},
        {2, 174000, 45093, 0xf31b3f32}, {2, 192000, 49653, 0xcf081c13},
        {2, 220000, 56734, 0x17f98789}, {39, 128000, 15361, 0xdfce004b},
        {39, 174000, 20881, 0x02d55c6e}, {39, 192000, 23041, 0x7099ba4b},
        {39, 220000, 26400, 0x88655cac}, {39, 256000, 30720, 0x7d41fe7c},
    };
    static INT_PCM input_pcm[2048];
    static unsigned char encoded[1024];
    for (unsigned c = 0; c < sizeof(cases) / sizeof(*cases); ++c) {
        unsigned aot = cases[c].aot, rate = cases[c].bitrate;
        unsigned frames = aot == 39 ? 480 : 1024;
        HANDLE_AACENCODER handle = playback_encoder(aot, rate);
        unsigned hash = 2166136261u, random = 12345, bytes_total = 0;
        for (unsigned frame = 0; frame < 96; ++frame) {
            for (unsigned n = 0; n < frames * 2; ++n) {
                random = random * 1664525u + 1013904223u;
                int value = (int)((random >> 16) & 65535) - 32768;
                switch ((frame / 8) % 4) {
                    case 0: value = 0; break;
                    case 1: value = (n % 96 < 48 ? (int)(n % 48) : 48 - (int)(n % 48)) * 500 - 12000; break;
                    case 2: if (n % 80) value = 0; break;
                    default: break;
                }
                input_pcm[n] = (INT_PCM)value;
            }
            void *input_ptrs[] = {input_pcm}, *output_ptrs[] = {encoded};
            INT input_ids[] = {IN_AUDIO_DATA}, output_ids[] = {OUT_BITSTREAM_DATA};
            INT input_sizes[] = {frames * 2 * sizeof(INT_PCM)}, output_sizes[] = {sizeof(encoded)};
            INT input_elements[] = {sizeof(INT_PCM)}, output_elements[] = {1};
            AACENC_BufDesc in = {1, input_ptrs, input_ids, input_sizes, input_elements};
            AACENC_BufDesc out = {1, output_ptrs, output_ids, output_sizes, output_elements};
            AACENC_InArgs args = {0};
            AACENC_OutArgs result = {0};
            args.numInSamples = frames * 2;
            ENC_OK(aacEncEncode(handle, &in, &out, &args, &result));
            if (result.numInSamples != (INT)(frames * 2) || result.numOutBytes < 0 ||
                result.numOutBytes > (INT)sizeof(encoded)) fail("matrix output bounds", result.numOutBytes);
            bytes_total += result.numOutBytes;
            hash = (hash ^ (unsigned)result.numOutBytes) * 16777619u;
            for (INT n = 0; n < result.numOutBytes; ++n) hash = (hash ^ encoded[n]) * 16777619u;
        }
        if (hash != cases[c].hash || bytes_total != cases[c].bytes) fail("encoder matrix checksum", c);
        ENC_OK(aacEncClose(&handle));
        if (mallinfo().uordblks != 0) fail("encoder matrix cleanup", c);
    }
    probe_emit("MATRIX 960 frames across ten AAC/AAC-ELD configurations match baseline hashes\n");
}

static void check_lifecycle(void) {
    static const struct { unsigned aot, bitrate; const char *name; } modes[] = {
        {39, 256000, "lifecycle_ELD_256"},
        {2, 128000, "lifecycle_LC_128"},
        {2, 220000, "lifecycle_LC_220"},
    };
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(*modes); ++mode) {
        for (unsigned cycle = 0; cycle < 8; ++cycle) {
            HANDLE_AACDECODER decoder = NULL;
            HANDLE_AACENCODER stereo = NULL;
            if (cycle & 1) stereo = playback_encoder(modes[mode].aot, modes[mode].bitrate);
            decoder = aacDecoder_Open(TT_MP4_RAW, 1);
            if (decoder == NULL) fail("lifecycle decoder open", cycle);
            profile_configure(decoder);
            if (stereo == NULL) stereo = playback_encoder(modes[mode].aot, modes[mode].bitrate);
            unsigned char packet[1024];
            unsigned length;
            encode_frame(stereo, 2, 0, packet, &length);
            DEC_OK(profile_decode(decoder, packets[0], packet_lengths[0], AACDEC_CLRHIST));
            if (cycle == 0) report_heap(modes[mode].name);
            ENC_OK(aacEncClose(&stereo));
            // Recreate playback while recording retains its allocations.
            stereo = playback_encoder(modes[mode].aot, modes[mode].bitrate);
            encode_frame(stereo, 2, 1, packet, &length);
            DEC_OK(aacDecoder_SetParam(decoder, AAC_TPDEC_CLEAR_BUFFER, 1));
            DEC_OK(profile_decode(decoder, packets[0], packet_lengths[0], AACDEC_CLRHIST));
            aacDecoder_Close(decoder);
            ENC_OK(aacEncClose(&stereo));
            if (mallinfo().uordblks != 0) fail("lifecycle cleanup", cycle);
        }
    }
    probe_emit("LIFECYCLE ELD-256/LC-128/LC-220 alternating allocation order and retained-microphone playback recreation passed\n");
}

static void check_variable_encoder(void) {
    static const unsigned rates[]={128000,174000,192000,220000,256000};
    for (unsigned mode=0;mode<sizeof(rates)/sizeof(*rates);++mode) {
        unsigned peak=rates[mode]/800*800;
        unsigned bandwidth=peak<=128000 ? 15500 : peak<=160000 ? 17500 : 20000;
        HANDLE_AACDECODER decoder=aacDecoder_Open(TT_MP4_RAW,1);
        if (decoder==NULL) fail("VBR simultaneous microphone",mode);
        profile_configure(decoder);
        HANDLE_AACENCODER handle=NULL;
        ENC_OK(aacEncOpen(&handle,1,2));
        const struct { AACENC_PARAM key; UINT value; } parameters[]={
            {AACENC_AOT,39},{AACENC_SAMPLERATE,48000},{AACENC_CHANNELMODE,MODE_2},
            {AACENC_GRANULE_LENGTH,480},{AACENC_SBR_MODE,0},{AACENC_BITRATE,peak},
            {AACENC_BITRATEMODE,5},{AACENC_PEAK_BITRATE,peak},{AACENC_AFTERBURNER,0},
            {AACENC_TRANSMUX,TT_MP4_RAW},{AACENC_BANDWIDTH,bandwidth},
        };
        for (unsigned i=0;i<sizeof(parameters)/sizeof(*parameters);++i)
            ENC_OK(aacEncoder_SetParam(handle,parameters[i].key,parameters[i].value));
        ENC_OK(aacEncEncode(handle,NULL,NULL,NULL,NULL));
        AACENC_InfoStruct info={0}; ENC_OK(aacEncInfo(handle,&info));
        if (info.frameLength!=480 || info.inputChannels!=2 ||
            aacEncoder_GetParam(handle,AACENC_PEAK_BITRATE)!=peak ||
            aacEncoder_GetParam(handle,AACENC_BITRATEMODE)!=(peak<192000 ? 4u : 5u)) fail("VBR configuration",mode);
        unsigned maximum=0;
        for (unsigned frame=0;frame<96;++frame) {
            for (unsigned n=0;n<960;++n) {
                unsigned value=(n+frame*960)*1103515245u+12345u;
                pcm[n]=(INT_PCM)((frame%24<8) ? 0 : (frame%24<16) ? (int)((n%96)*500)-24000 : (int)(value>>16)-32768);
            }
            unsigned char output[1024];
            void *in_ptrs[]={pcm}, *out_ptrs[]={output};
            INT in_ids[]={IN_AUDIO_DATA},out_ids[]={OUT_BITSTREAM_DATA};
            INT in_sizes[]={960*sizeof(INT_PCM)},out_sizes[]={sizeof(output)};
            INT in_elements[]={sizeof(INT_PCM)},out_elements[]={1};
            AACENC_BufDesc in={1,in_ptrs,in_ids,in_sizes,in_elements};
            AACENC_BufDesc out={1,out_ptrs,out_ids,out_sizes,out_elements};
            AACENC_InArgs args={960,0}; AACENC_OutArgs result={0};
            ENC_OK(aacEncEncode(handle,&in,&out,&args,&result));
            if (result.numInSamples!=960 || result.numOutBytes<0 || (unsigned)result.numOutBytes>peak/800)
                fail("VBR frame ceiling",result.numOutBytes);
            if ((unsigned)result.numOutBytes>maximum) maximum=(unsigned)result.numOutBytes;
            DEC_OK(profile_decode(decoder,packets[frame%16],packet_lengths[frame%16],AACDEC_CLRHIST));
        }
        char line[120];
        snprintf(line,sizeof(line),"VBR requested=%u peak=%u frames=96 max_au=%u limit=%u\n",rates[mode],peak,maximum,peak/800);
        probe_emit(line);
        aacDecoder_Close(decoder); ENC_OK(aacEncClose(&handle));
        if (mallinfo().uordblks!=0) fail("VBR lifecycle cleanup",mode);
    }
    probe_emit("VBR 480 frames with retained microphone respect five negotiated frame ceilings\n");
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
    if (successful != 16 || checksum != 0x4105af5du || mallinfo().uordblks != 0) return 1;
    profile_checks(packets, packet_lengths);
    check_encoder_matrix();
    check_lifecycle();
    check_variable_encoder();
    snprintf(text, sizeof(text), "HEAP_PEAK bytes=%u\n", (unsigned)peak_heap_break);
    probe_emit(text);
    probe_emit("CHECKS complete\n");
    return 0;
}
