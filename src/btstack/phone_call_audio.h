// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PHONE_CALL_AUDIO_H
#define AIRPODS_PHONE_CALL_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define PHONE_CALL_AUDIO_FIFO_SAMPLES 512u
#define PHONE_CALL_AUDIO_MAX_INPUT 256u
#define PHONE_CALL_MIC_SAMPLE_RATE 64000u

// Serialized Bluetooth/main-loop context. No allocation; 0 stops and flushes.
// Other accepted rates are 8000 (CVSD) and 16000 (mSBC), signed mono PCM.
bool phone_call_audio_reset(uint32_t sample_rate);
// HFP gains are local attenuation only: 0=mute, 15=unity. Preserved on reset.
void phone_call_audio_set_gain(uint8_t speaker, uint8_t microphone);
// Bad/lost frames insert silence; mono may be NULL only for bad or empty input.
bool phone_call_audio_downlink(const int16_t *mono, uint16_t frames, bool bad_frame);
// Reads always fill the requested destination, using silence when empty/stopped.
void phone_call_audio_read(int16_t *stereo48, uint16_t frames);
// Source PCM presentation clock is 64 kHz on the captured Pro 3, independent of ASC metadata.
void phone_call_audio_mic(const int16_t *mono_source, uint16_t frames);
// Real accepted source mic PCM only; public reset starts a new session counter.
uint32_t phone_call_audio_mic_samples(void);
void phone_call_audio_uplink(int16_t *mono, uint16_t frames);
// One bounded pass: drain actual call audio and fetch available, call-owned mic PCM.
void phone_call_audio_poll(void);

// Shared output/microphone ownership boundary, implemented by the application.
bool bridge_call_ready(void);
void bridge_call_pcm_push(const int16_t *stereo48, uint16_t frames);
uint32_t aacp_mic_call_read(int16_t *mono_source, uint32_t capacity);

#endif
