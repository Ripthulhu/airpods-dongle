// SPDX-License-Identifier: GPL-3.0-only
// Host-only: the firmware-profile encoder and full stereo decoder run separately.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "aacenc_lib.h"
#include "aacdecoder_lib.h"
#include "playback_eld.h"

static void require(bool valid, const char *message) {
    if (!valid) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}

static void write_u32(FILE *file, uint32_t value) {
    unsigned char data[] = {static_cast<unsigned char>(value),
        static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value >> 16),
        static_cast<unsigned char>(value >> 24)};
    require(std::fwrite(data, 1, sizeof(data), file) == sizeof(data), "write fixture");
}

static bool read_u32(FILE *file, uint32_t &value) {
    unsigned char data[4];
    size_t count = std::fread(data, 1, sizeof(data), file);
    if (!count && std::feof(file)) return false;
    require(count == sizeof(data), "truncated fixture");
    value = data[0] | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
    return true;
}

static INT_PCM reference_sample(uint64_t sample, unsigned channel) {
    const double pi = 3.14159265358979323846;
    double time = static_cast<double>(sample) / 48000;
    double primary = channel ? 683.0 : 437.0;
    double secondary = channel ? 1601.0 : 997.0;
    double envelope = 0.70 + 0.25 * std::sin(2 * pi * (channel ? 3.7 : 2.3) * time);
    double signal = envelope * (11000 * std::sin(2 * pi * primary * time) +
                                3700 * std::sin(2 * pi * secondary * time));
    return static_cast<INT_PCM>(std::lrint(signal));
}

#ifndef ADAPTIVE_CODEC_DECODER
// Linker wrappers cover the static FDK archive, not just fixture allocations.
static bool heap_guard;
static unsigned heap_calls[4], guarded_heap_calls[4], guarded_frames;
extern "C" {
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__wrap_malloc(size_t size) {
    ++heap_calls[0]; if (heap_guard) ++guarded_heap_calls[0];
    return __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
    ++heap_calls[1]; if (heap_guard) ++guarded_heap_calls[1];
    return __real_calloc(count, size);
}
void *__wrap_realloc(void *pointer, size_t size) {
    ++heap_calls[2]; if (heap_guard) ++guarded_heap_calls[2];
    return __real_realloc(pointer, size);
}
void __wrap_free(void *pointer) {
    ++heap_calls[3]; if (heap_guard) ++guarded_heap_calls[3];
    __real_free(pointer);
}
}

static void check_allocator_wrappers() {
    // Volatile storage prevents the compiler from removing paired allocations.
    heap_guard = true;
    void *volatile pointer = std::malloc(7);
    require(pointer != nullptr, "malloc probe");
    pointer = std::realloc(pointer, 13);
    require(pointer != nullptr, "realloc probe");
    std::free(pointer);
    pointer = std::calloc(2, 9);
    require(pointer != nullptr, "calloc probe");
    std::free(pointer);
    heap_guard = false;
    require(guarded_heap_calls[0] == 1 && guarded_heap_calls[1] == 1 &&
            guarded_heap_calls[2] == 1 && guarded_heap_calls[3] == 2,
            "allocator wrappers inactive");
    std::memset(heap_calls, 0, sizeof(heap_calls));
    std::memset(guarded_heap_calls, 0, sizeof(guarded_heap_calls));
}

static HANDLE_AACENCODER handleAAC;
static AACENC_InfoStruct aacinf;
static struct { unsigned rate_mode; } playback_options;
static uint32_t aaceld_bitrate;
static bool aaceld_variable_allowed = true;
static bool fail_peak_once, fail_info_once;
static unsigned setter_calls;

static AACENC_ERROR checked_set_param(HANDLE_AACENCODER handle, AACENC_PARAM parameter, UINT value) {
    ++setter_calls;
    if (fail_peak_once && parameter == AACENC_PEAK_BITRATE) {
        fail_peak_once = false;
        return AACENC_INVALID_CONFIG;
    }
    return aacEncoder_SetParam(handle, parameter, value);
}

static AACENC_ERROR checked_info(HANDLE_AACENCODER handle, AACENC_InfoStruct *info) {
    if (fail_info_once) { fail_info_once = false; return AACENC_INVALID_CONFIG; }
    return aacEncInfo(handle, info);
}

#define aacEncoder_SetParam checked_set_param
#define aacEncInfo checked_info
#include "production_rate.inc"
#undef aacEncoder_SetParam
#undef aacEncInfo

static void encoder_open(unsigned mode, uint32_t ceiling) {
    playback_options.rate_mode = mode;
    aaceld_bitrate = ceiling;
    require(aacEncOpen(&handleAAC, 1, 2) == AACENC_OK, "encoder open");
    const struct { AACENC_PARAM parameter; UINT value; } parameters[] = {
        {AACENC_AOT, 39}, {AACENC_BITRATE, ceiling}, {AACENC_SAMPLERATE, 48000},
        {AACENC_CHANNELMODE, MODE_2}, {AACENC_GRANULE_LENGTH, 480},
        {AACENC_SBR_MODE, 0}, {AACENC_BITRATEMODE, mode ? 5u : 0u},
        {AACENC_PEAK_BITRATE, ceiling}, {AACENC_AFTERBURNER, 0},
        {AACENC_TRANSMUX, TT_MP4_RAW}
    };
    for (const auto &parameter : parameters)
        require(aacEncoder_SetParam(handleAAC, parameter.parameter, parameter.value) == AACENC_OK,
                "encoder parameters");
    if (mode) {
        unsigned bandwidth = ceiling <= 128000 ? 15500 : ceiling <= 160000 ? 17500 : 20000;
        require(aacEncoder_SetParam(handleAAC, AACENC_BANDWIDTH, bandwidth) == AACENC_OK,
                "encoder bandwidth");
    }
    require(aacEncEncode(handleAAC, nullptr, nullptr, nullptr, nullptr) == AACENC_OK &&
            aacEncInfo(handleAAC, &aacinf) == AACENC_OK, "encoder initialize");
}

static AACENC_ERROR encode_without_heap(AACENC_BufDesc *input, AACENC_BufDesc *output,
                                        AACENC_InArgs *arguments, AACENC_OutArgs *result) {
    heap_guard = true;
    AACENC_ERROR error = aacEncEncode(handleAAC, input, output, arguments, result);
    heap_guard = false;
    ++guarded_frames;
    require(guarded_heap_calls[0] == 0 && guarded_heap_calls[1] == 0 &&
            guarded_heap_calls[2] == 0 && guarded_heap_calls[3] == 0,
            "steady encode touched the heap");
    return error;
}

static void check_lc_allocator(uint32_t bitrate) {
    require(bitrate == 96000 || bitrate == 128000 || bitrate == 174000 ||
            bitrate == 192000 || bitrate == 220000, "AAC-LC bitrate");
    check_allocator_wrappers();
    require(aacEncOpen(&handleAAC, 1, 2) == AACENC_OK, "AAC-LC open");
    // Matches the production AAC-LC configuration, including its LATM headers.
    const struct { AACENC_PARAM parameter; UINT value; } parameters[] = {
        {AACENC_AOT, AOT_AAC_LC}, {AACENC_BITRATE, bitrate},
        {AACENC_SAMPLERATE, 48000}, {AACENC_CHANNELMODE, MODE_2},
        {AACENC_BITRATEMODE, 0}, {AACENC_AFTERBURNER, 0},
        {AACENC_TRANSMUX, TT_MP4_LATM_MCP1},
        {AACENC_HEADER_PERIOD, 1}, {AACENC_AUDIOMUXVER, 1}
    };
    for (const auto &parameter : parameters)
        require(aacEncoder_SetParam(handleAAC, parameter.parameter, parameter.value) == AACENC_OK,
                "AAC-LC parameters");
    require(aacEncEncode(handleAAC, nullptr, nullptr, nullptr, nullptr) == AACENC_OK &&
            aacEncInfo(handleAAC, &aacinf) == AACENC_OK && aacinf.inputChannels == 2 &&
            aacinf.frameLength == 1024 && heap_calls[0] + heap_calls[1] > 0,
            "AAC-LC initialization");
    for (unsigned frame = 0; frame < 128; ++frame) {
        INT_PCM pcm[2048]; unsigned char output[2048];
        for (unsigned sample = 0; sample < 1024; ++sample)
            for (unsigned channel = 0; channel < 2; ++channel)
                pcm[2 * sample + channel] = reference_sample(uint64_t(frame) * 1024 + sample, channel);
        void *inputs[] = {pcm}, *outputs[] = {output};
        INT input_id = IN_AUDIO_DATA, output_id = OUT_BITSTREAM_DATA;
        INT input_size = sizeof(pcm), output_size = sizeof(output);
        INT input_element = sizeof(INT_PCM), output_element = 1;
        AACENC_BufDesc input = {1, inputs, &input_id, &input_size, &input_element};
        AACENC_BufDesc encoded = {1, outputs, &output_id, &output_size, &output_element};
        AACENC_InArgs arguments = {2048, 0}; AACENC_OutArgs result = {};
        require(encode_without_heap(&input, &encoded, &arguments, &result) == AACENC_OK &&
                result.numInSamples == 2048 && result.numOutBytes >= 0 &&
                (frame < 3 || result.numOutBytes > 0), "AAC-LC steady encode");
    }
    require(aacEncClose(&handleAAC) == AACENC_OK, "AAC-LC close");
    std::printf("AAC_LC bitrate=%u guarded_frames=%u heap_calls=0 allocator_probe=passed\n",
                bitrate, guarded_frames);
}

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "lc") == 0) {
        check_lc_allocator(std::strtoul(argv[2], nullptr, 10));
        return 0;
    }
    require(argc == 4 || argc == 5, "encoder arguments: mode ceiling output [reset]");
    unsigned mode = std::strtoul(argv[1], nullptr, 10);
    uint32_t ceiling = std::strtoul(argv[2], nullptr, 10);
    require(mode <= 1 && ceiling >= 128000 && ceiling <= 256000, "encoder case");
    check_allocator_wrappers();
    encoder_open(mode, ceiling);
    require(heap_calls[0] + heap_calls[1] > 0, "FDK archive allocator interception");
    const AACENC_InfoStruct initial = aacinf;
    require(initial.inputChannels == 2 && initial.frameLength == 480, "initial stereo 480");
    FILE *file = std::fopen(argv[3], "wb"); require(file != nullptr, "open output");
    write_u32(file, initial.confSize);
    require(std::fwrite(initial.confBuf, 1, initial.confSize, file) == initial.confSize, "write ASC");
    std::vector<uint32_t> rates{ceiling};
    const uint32_t rungs[] = {256000, 220000, 192000, 174000, 160000, 128000};
    for (uint32_t rate : rungs) {
        if (mode) rate = rate / 800 * 800;
        if (rate < rates.back()) rates.push_back(rate);
    }
    const size_t down_count = rates.size();
    for (size_t i = down_count - 1; i > 0; --i) rates.push_back(rates[i - 1]);
    rates.push_back(ceiling); // A no-op must preserve history too.
    unsigned frame = 0, transitions = 0, largest = 0;
    uint32_t previous = ceiling;
    HANDLE_AACENCODER original = handleAAC;
    aaceld_variable_allowed = false;
    unsigned setters_before = setter_calls;
    uint32_t unauthorized = previous > 128000 ? 128000 : previous;
    require(playback_eld_rate_update(previous, unauthorized) == 1 &&
            setter_calls == setters_before && handleAAC == original &&
            aacEncoder_GetParam(handleAAC, AACENC_PEAK_BITRATE) == previous &&
            aacEncoder_GetParam(handleAAC, AACENC_CONTROL_STATE) == 0,
            "CBR-only wire configuration forbids live retune");
    aaceld_variable_allowed = true;
    require(playback_eld_rate_update(previous, 127999) == 1 &&
            playback_eld_rate_update(previous, ceiling + 800) == 1 &&
            (!mode || playback_eld_rate_update(previous, 128001) == 1), "invalid budget guard");
    bool rollback_checked = false;
    for (uint32_t rate : rates) {
        if (rate != previous && !rollback_checked) {
            // Fail both before and after a real FDK config commit; rollback must preserve audio.
            fail_peak_once = true;
            require(playback_eld_rate_update(previous, rate) == 1 && !fail_peak_once &&
                    aacEncoder_GetParam(handleAAC, AACENC_PEAK_BITRATE) == previous,
                    "rollback partially staged setter");
            fail_info_once = true;
            require(playback_eld_rate_update(previous, rate) == 1 && !fail_info_once &&
                    aacEncoder_GetParam(handleAAC, AACENC_PEAK_BITRATE) == previous,
                    "rollback committed configuration");
            rollback_checked = true;
        }
        require(playback_eld_rate_update(previous, rate) == 0, "production live update");
        require(argc == 5 || handleAAC == original, "encoder handle changed");
        require(aacinf.inputChannels == 2 && aacinf.frameLength == 480 &&
                aacinf.confSize == initial.confSize &&
                std::memcmp(aacinf.confBuf, initial.confBuf, initial.confSize) == 0,
                "ASC or format changed");
        require(aacEncoder_GetParam(handleAAC, AACENC_CONTROL_STATE) == 0, "uncommitted config");
        require(aacEncoder_GetParam(handleAAC, AACENC_PEAK_BITRATE) == rate, "committed peak");
        unsigned actual_mode = aacEncoder_GetParam(handleAAC, AACENC_BITRATEMODE);
        require(mode ? actual_mode >= 1 && actual_mode <= 5 : actual_mode == 0, "committed mode");
        if (rate != previous) {
            ++transitions;
            if (argc == 5) { // Negative control: actual Close/Open destroys overlap history.
                require(aacEncClose(&handleAAC) == AACENC_OK, "negative close");
                encoder_open(mode, rate);
                aaceld_bitrate = ceiling;
            }
        }
        previous = rate;
        for (unsigned local = 0; local < 48; ++local, ++frame) {
            INT_PCM pcm[960]; unsigned char output[2048];
            for (unsigned sample = 0; sample < 480; ++sample)
                for (unsigned channel = 0; channel < 2; ++channel)
                    pcm[2 * sample + channel] = reference_sample(uint64_t(frame) * 480 + sample, channel);
            void *inputs[] = {pcm}, *outputs[] = {output};
            INT input_id = IN_AUDIO_DATA, output_id = OUT_BITSTREAM_DATA;
            INT input_size = sizeof(pcm), output_size = sizeof(output);
            INT input_element = sizeof(INT_PCM), output_element = 1;
            AACENC_BufDesc input = {1, inputs, &input_id, &input_size, &input_element};
            AACENC_BufDesc encoded = {1, outputs, &output_id, &output_size, &output_element};
            AACENC_InArgs arguments = {960, 0}; AACENC_OutArgs result = {};
            AACENC_ERROR error = encode_without_heap(&input, &encoded, &arguments, &result);
            require(error == AACENC_OK, "encode after live update");
            require(result.numInSamples == 960 && result.numOutBytes > 0, "one complete AU per frame");
            unsigned limit = (rate + 799) / 800 + (mode ? 0 : 1);
            // FDK CBR permits one alignment byte above its average; VBR peak is strict.
            require(static_cast<unsigned>(result.numOutBytes) <= limit, "AU exceeds frame budget");
            largest = std::max(largest, static_cast<unsigned>(result.numOutBytes));
            write_u32(file, rate); write_u32(file, result.numOutBytes);
            require(std::fwrite(output, 1, result.numOutBytes, file) == static_cast<size_t>(result.numOutBytes),
                    "write AU");
        }
    }
    require(std::fclose(file) == 0, "close output");
    require(aacEncClose(&handleAAC) == AACENC_OK, "encoder close");
    std::printf("ENCODE mode=%u ceiling=%u frames=%u transitions=%u max_au=%u "
                "guarded_frames=%u heap_calls=0 allocator_probe=passed\n",
                mode, ceiling, frame, transitions, largest, guarded_frames);
}
#else
int main(int argc, char **argv) {
    require(argc == 2, "decoder arguments: input");
    FILE *file = std::fopen(argv[1], "rb"); require(file != nullptr, "open input");
    uint32_t config_size; require(read_u32(file, config_size) && config_size <= 64, "ASC size");
    UCHAR config[64]; require(std::fread(config, 1, config_size, file) == config_size, "read ASC");
    UCHAR *configs[] = {config}; UINT lengths[] = {config_size};
    HANDLE_AACDECODER decoder = aacDecoder_Open(TT_MP4_RAW, 1);
    require(decoder != nullptr && aacDecoder_ConfigRaw(decoder, configs, lengths) == AAC_DEC_OK,
            "decoder configure");
    require(aacDecoder_SetParam(decoder, AAC_PCM_LIMITER_ENABLE, 0) == AAC_DEC_OK, "disable limiter");
    std::vector<INT_PCM> decoded;
    uint32_t rate, count; unsigned frames = 0;
    while (read_u32(file, rate)) {
        require(read_u32(file, count) && count > 0 && count <= 2048, "AU size");
        UCHAR bytes[2048]; require(std::fread(bytes, 1, count, file) == count, "read AU");
        UCHAR *inputs[] = {bytes}; UINT sizes[] = {count}, remaining = count;
        require(aacDecoder_Fill(decoder, inputs, sizes, &remaining) == AAC_DEC_OK && remaining == 0,
                "decoder fill");
        INT_PCM pcm[960];
        require(aacDecoder_DecodeFrame(decoder, pcm, 960, 0) == AAC_DEC_OK, "decode live AU");
        const CStreamInfo *info = aacDecoder_GetStreamInfo(decoder);
        require(info && info->sampleRate == 48000 && info->numChannels == 2 &&
                info->frameSize == 480 && info->aot == 39, "decoded stereo format");
        decoded.insert(decoded.end(), pcm, pcm + 960); ++frames;
    }
    require(std::fclose(file) == 0, "close input"); aacDecoder_Close(decoder);
    require(frames >= 96, "enough decoded frames");
    // Find initial fixed codec delay once; later skips/resets cannot realign it.
    unsigned best_delay = 0; double best_error = 1e100;
    for (unsigned delay = 0; delay <= 2048; ++delay) {
        double error = 0;
        for (unsigned sample = 4096; sample < 12288; sample += 4) {
            double difference = decoded[2 * sample] - reference_sample(sample - delay, 0);
            error += difference * difference;
        }
        if (error < best_error) { best_error = error; best_delay = delay; }
    }
    double minimum_snr = 100, minimum_gain = 100, maximum_gain = 0;
    for (unsigned start = 4096; start + 1920 <= frames * 480; start += 1920) {
        for (unsigned channel = 0; channel < 2; ++channel) {
            double signal = 0, error = 0, cross = 0;
            for (unsigned sample = start; sample < start + 1920; ++sample) {
                double expected = reference_sample(sample - best_delay, channel);
                double actual = decoded[2 * sample + channel];
                signal += expected * expected; error += (actual - expected) * (actual - expected);
                cross += actual * expected;
            }
            double snr = 10 * std::log10(signal / std::max(error, 1.0));
            double gain = cross / signal;
            minimum_snr = std::min(minimum_snr, snr);
            minimum_gain = std::min(minimum_gain, gain); maximum_gain = std::max(maximum_gain, gain);
            if (snr < 20 || gain < 0.95 || gain > 1.05) {
                std::fprintf(stderr, "FAIL continuity start=%u channel=%u snr=%.3f gain=%.6f delay=%u\n",
                             start, channel, snr, gain, best_delay);
                return 1;
            }
        }
    }
    std::printf("DECODE frames=%u delay=%u min_snr_db=%.3f gain=%.6f..%.6f\n",
                frames, best_delay, minimum_snr, minimum_gain, maximum_gain);
}
#endif
