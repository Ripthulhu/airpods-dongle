// SPDX-License-Identifier: GPL-3.0-only
#include "phone_call_audio.h"
#include <limits.h>
#include <string.h>

// Q15, unity-DC Hamming-windowed sinc: 3.4/6.8 kHz cutoff at 64 kHz.
// 127/63 taps give >26 dB attenuation at the respective 4/8 kHz Nyquist edge.
static const int16_t filter8[] = {
    11, 13, 14, 14, 12, 8, 3, -3, -10, -18, -24, -29,
    -32, -30, -24, -14, 1, 18, 37, 55, 69, 76, 75, 65,
    43, 13, -25, -66, -106, -139, -160, -164, -148, -111, -54, 18,
    99, 181, 252, 303, 325, 310, 254, 157, 26, -130, -296, -454,
    -583, -663, -676, -607, -446, -192, 151, 570, 1043, 1544, 2044, 2509,
    2909, 3217, 3410, 3478, 3410, 3217, 2909, 2509, 2044, 1544, 1043, 570,
    151, -192, -446, -607, -676, -663, -583, -454, -296, -130, 26, 157,
    254, 310, 325, 303, 252, 181, 99, 18, -54, -111, -148, -164,
    -160, -139, -106, -66, -25, 13, 43, 65, 75, 76, 69, 55,
    37, 18, 1, -14, -24, -30, -32, -29, -24, -18, -10, -3,
    3, 8, 12, 14, 14, 13, 11
};
static const int16_t filter16[] = {
    26, 26, 16, -6, -33, -55, -56, -26, 35, 104, 146, 124,
    25, -128, -270, -320, -217, 36, 356, 600, 614, 312, -258, -903,
    -1322, -1211, -383, 1138, 3087, 5017, 6434, 6952, 6434, 5017, 3087, 1138,
    -383, -1211, -1322, -903, -258, 312, 614, 600, 356, 36, -217, -320,
    -270, -128, 25, 124, 146, 104, 35, -26, -56, -55, -33, -6,
    16, 26, 26
};

typedef struct {
    int16_t samples[PHONE_CALL_AUDIO_FIFO_SAMPLES];
    uint16_t read, used;
} call_fifo_t;
static struct {
    call_fifo_t down, up;
    int16_t history[127], from, to;
    uint16_t rate;
    uint8_t down_ratio, mic_ratio, phase, mic_phase, history_write;
    int32_t uplink_average_q8;
    uint32_t uplink_step, uplink_fraction;
    bool output_ready;
} audio;
static uint8_t speaker_gain = 15, microphone_gain = 15;
static uint32_t microphone_samples;

static void uplink_clock_reset(void) {
    audio.uplink_average_q8 = (int32_t)(audio.rate * 256u / 80u);
    audio.uplink_step = 65536u;
}
static void fifo_push(call_fifo_t *fifo, int16_t value) {
    if (fifo->used == PHONE_CALL_AUDIO_FIFO_SAMPLES) {
        if (fifo == &audio.up) uplink_clock_reset();
        fifo->read = (fifo->read + 1u) % PHONE_CALL_AUDIO_FIFO_SAMPLES;
        --fifo->used;
    }
    fifo->samples[(fifo->read + fifo->used) % PHONE_CALL_AUDIO_FIFO_SAMPLES] = value;
    ++fifo->used;
}
static int16_t fifo_pop(call_fifo_t *fifo) {
    if (!fifo->used) return 0;
    int16_t value = fifo->samples[fifo->read];
    fifo->read = (fifo->read + 1u) % PHONE_CALL_AUDIO_FIFO_SAMPLES;
    --fifo->used;
    return value;
}
static int16_t rounded_sample(int64_t value, uint32_t divisor) {
    value = (value + (value < 0 ? -(int64_t)(divisor / 2u) : divisor / 2u)) / divisor;
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

static bool reset_conversion(uint32_t sample_rate) {
    memset(&audio, 0, sizeof(audio));
    if (sample_rate != 0 && sample_rate != 8000 && sample_rate != 16000) return false;
    audio.rate = (uint16_t)sample_rate;
    audio.down_ratio = sample_rate ? 48000u / sample_rate : 0;
    audio.mic_ratio = sample_rate ? PHONE_CALL_MIC_SAMPLE_RATE / sample_rate : 0;
    uplink_clock_reset();
    return true;
}
bool phone_call_audio_reset(uint32_t sample_rate) {
    microphone_samples = 0;
    return reset_conversion(sample_rate);
}
void phone_call_audio_set_gain(uint8_t speaker, uint8_t microphone) {
    speaker_gain = speaker > 15 ? 15 : speaker;
    microphone_gain = microphone > 15 ? 15 : microphone;
}
bool phone_call_audio_downlink(const int16_t *mono, uint16_t frames, bool bad_frame) {
    if (!audio.down_ratio || frames > PHONE_CALL_AUDIO_MAX_INPUT || (!mono && frames && !bad_frame))
        return false;
    if (bad_frame && frames) uplink_clock_reset();
    for (unsigned i = 0; i < frames; ++i) fifo_push(&audio.down, bad_frame ? 0 : mono[i]);
    return true;
}
void phone_call_audio_read(int16_t *stereo48, uint16_t frames) {
    if (!stereo48) return;
    for (unsigned i = 0; i < frames; ++i) {
        int16_t value = 0;
        if (audio.down_ratio) {
            if (!audio.phase) audio.to = fifo_pop(&audio.down);
            ++audio.phase;
            value = rounded_sample((int32_t)audio.from * (audio.down_ratio - audio.phase) +
                                   (int32_t)audio.to * audio.phase, audio.down_ratio);
            if (audio.phase == audio.down_ratio) { audio.phase = 0; audio.from = audio.to; }
        }
        stereo48[2u*i] = stereo48[2u*i + 1u] = rounded_sample((int32_t)value * speaker_gain, 15);
    }
}
void phone_call_audio_mic(const int16_t *mono_source, uint16_t frames) {
    if (!audio.mic_ratio || !mono_source || frames > PHONE_CALL_AUDIO_MAX_INPUT) return;
    microphone_samples += frames;
    const int16_t *filter = audio.rate == 8000 ? filter8 : filter16;
    unsigned taps = audio.rate == 8000 ? sizeof(filter8)/sizeof(*filter8) : sizeof(filter16)/sizeof(*filter16);
    for (unsigned i = 0; i < frames; ++i) {
        audio.history[audio.history_write] = mono_source[i];
        if (++audio.history_write == 127) audio.history_write = 0;
        if (++audio.mic_phase != audio.mic_ratio) continue;
        audio.mic_phase = 0;
        unsigned index = audio.history_write;
        int64_t sum = 0;
        for (unsigned tap = 0; tap < taps; ++tap) {
            index = index ? index - 1u : 126u;
            sum += (int32_t)audio.history[index] * filter[tap];
        }
        fifo_push(&audio.up, rounded_sample(sum, 32768));
    }
}
uint32_t phone_call_audio_mic_samples(void) { return microphone_samples; }
void phone_call_audio_uplink(int16_t *mono, uint16_t frames) {
    if (!mono || !frames) return;
    if (!audio.down_ratio) { memset(mono, 0, (size_t)frames * sizeof(*mono)); return; }
    // Average by consumed phone time, not callback count; tolerate 10 ms mic bursts.
    uint32_t target = audio.rate / 80u; // 12.5 ms, separate from the arrival sawtooth.
    if (frames > audio.rate / 40u || audio.up.used < frames + 2u ||
        audio.up.used > target + audio.rate / 100u + frames + 2u) {
        uplink_clock_reset();
    } else {
        int32_t remaining_q8 = (int32_t)(audio.up.used - frames) * 256;
        audio.uplink_average_q8 += (remaining_q8 - audio.uplink_average_q8) * frames / (int32_t)(audio.rate / 2u);
        int32_t correction = (audio.uplink_average_q8 - (int32_t)target * 256) / 128;
        if (correction > 65) correction = 65;
        if (correction < -65) correction = -65;
        audio.uplink_step = (uint32_t)(65536 + correction); // At most +/-992 ppm.
    }
    for (unsigned i = 0; i < frames; ++i) {
        if (!audio.up.used) {
            memset(mono + i, 0, (size_t)(frames - i) * sizeof(*mono));
            audio.uplink_fraction = 0;
            uplink_clock_reset();
            break;
        }
        int16_t first = audio.up.samples[audio.up.read];
        int16_t next = audio.up.used > 1 ? audio.up.samples[(audio.up.read + 1u) % PHONE_CALL_AUDIO_FIFO_SAMPLES] : first;
        int16_t value = rounded_sample((int64_t)first * (65536u - audio.uplink_fraction) +
                                      (int64_t)next * audio.uplink_fraction, 65536);
        mono[i] = rounded_sample((int32_t)value * microphone_gain, 15);
        uint32_t position = audio.uplink_fraction + audio.uplink_step;
        unsigned consume = position >> 16;
        audio.uplink_fraction = position & 0xffffu;
        while (consume-- && audio.up.used) (void)fifo_pop(&audio.up);
    }
}
void phone_call_audio_poll(void) {
    if (!audio.down_ratio) return;
    uint16_t rate = audio.rate;
    if (!bridge_call_ready()) {
        if (audio.output_ready || audio.down.used || audio.up.used) reset_conversion(rate);
        return;
    }
    if (!audio.output_ready) {
        reset_conversion(rate);
        audio.output_ready = true;
    }
    int16_t samples[128 * 2];
    unsigned available = audio.down.used * audio.down_ratio + (audio.phase ? audio.down_ratio - audio.phase : 0);
    unsigned remaining = available > 512 ? 512 : available;
    while (remaining) {
        uint16_t count = remaining > 128 ? 128 : (uint16_t)remaining;
        phone_call_audio_read(samples, count);
        bridge_call_pcm_push(samples, count);
        remaining -= count;
    }
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t got = aacp_mic_call_read(samples, 256);
        if (!got || got > 256) break;
        phone_call_audio_mic(samples, (uint16_t)got);
    }
}
