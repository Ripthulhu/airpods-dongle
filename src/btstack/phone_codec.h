// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PHONE_CODEC_H
#define AIRPODS_PHONE_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { PHONE_CODEC_NONE, PHONE_CODEC_SBC, PHONE_CODEC_APTX } phone_codec_t;
typedef struct {
    uint8_t blocks, subbands, channel_mode, allocation, min_bitpool, max_bitpool;
} phone_sbc_config_t;
typedef void (*phone_codec_pcm_callback)(const int16_t *stereo, uint16_t frames, void *context);

#define PHONE_CODEC_MAX_PAYLOAD 1024u

// Capability only; prepare() must succeed before accepting a stream Start.
bool phone_codec_available(phone_codec_t codec);
bool phone_codec_configuration_valid(phone_codec_t codec, const phone_sbc_config_t *sbc);
bool phone_codec_configure(phone_codec_t codec, const phone_sbc_config_t *sbc);
// Thread context under the Bluetooth lock: allocates/frees aptX only here.
bool phone_codec_prepare(void);
void phone_codec_close(void);
bool phone_codec_ready(void);
void phone_codec_reset(void);
// SBC RTP/media headers are removed by the caller; classic aptX has neither.
// Success means processed, not necessarily PCM produced while aptX resynchronizes.
bool phone_codec_decode(const uint8_t *payload, size_t length, uint8_t sbc_frames,
                        phone_codec_pcm_callback callback, void *context);

#endif
