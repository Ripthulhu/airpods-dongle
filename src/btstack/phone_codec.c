// SPDX-License-Identifier: GPL-3.0-only
#include "phone_codec.h"
#include <string.h>
#include "freeaptx.h"
#include "oi_codec_sbc.h"
#include "lhdc48.h"

static phone_codec_t selected;
static phone_sbc_config_t configuration;
static struct aptx_context *aptx;
static OI_CODEC_SBC_DECODER_CONTEXT sbc;
static OI_UINT32 sbc_data[CODEC_DATA_WORDS(2, SBC_CODEC_MIN_FILTER_BUFFERS)];
static bool sbc_ready;
static bool lhdc_enabled;

bool phone_codec_available(phone_codec_t codec) {
    return codec == PHONE_CODEC_SBC || codec == PHONE_CODEC_APTX ||
           (codec == PHONE_CODEC_LHDCV5 && lhdc_enabled);
}
void phone_codec_enable_lhdc(bool enabled) { lhdc_enabled = enabled; }

void phone_codec_reset(void) {
    if (aptx != NULL) aptx_reset(aptx);
    if (selected == PHONE_CODEC_LHDCV5) lhdc48_reset();
    sbc_ready = OI_CODEC_SBC_DecoderReset(&sbc, sbc_data, sizeof(sbc_data), 2, 2, FALSE)
                == OI_STATUS_SUCCESS;
}

bool phone_codec_configuration_valid(phone_codec_t codec, const phone_sbc_config_t *config) {
    // Startup exercises LHDC before its endpoint becomes available to a phone.
    if (codec == PHONE_CODEC_LHDCV5) return true;
    if (!phone_codec_available(codec)) return false;
    if (codec == PHONE_CODEC_SBC) {
        if (config == NULL || (config->blocks != 4 && config->blocks != 8 &&
            config->blocks != 12 && config->blocks != 16) ||
            (config->subbands != 4 && config->subbands != 8) ||
            (config->channel_mode != 2 && config->channel_mode != 3) ||
            config->allocation > 1 || config->min_bitpool < 2 ||
            config->max_bitpool > 53 || config->min_bitpool > config->max_bitpool)
            return false;
    }
    return true;
}

bool phone_codec_configure(phone_codec_t codec, const phone_sbc_config_t *config) {
    selected = PHONE_CODEC_NONE;
    if (!phone_codec_configuration_valid(codec, config)) return false;
    if (codec == PHONE_CODEC_SBC) configuration = *config;
    selected = codec;
    phone_codec_reset();
    return true;
}

bool phone_codec_prepare(void) {
    if (selected != PHONE_CODEC_LHDCV5 && lhdc48_ready()) lhdc48_close();
    if (selected != PHONE_CODEC_APTX && aptx != NULL) {
        aptx_finish(aptx);
        aptx = NULL;
    }
    if (selected == PHONE_CODEC_APTX && aptx == NULL) aptx = aptx_init(0);
    if (selected == PHONE_CODEC_LHDCV5 && !lhdc48_ready()) return lhdc48_prepare(24);
    return phone_codec_ready();
}

void phone_codec_close(void) {
    selected = PHONE_CODEC_NONE;
    phone_codec_prepare();
    sbc_ready = false;
}

bool phone_codec_ready(void) {
    return (selected == PHONE_CODEC_SBC && sbc_ready) ||
           (selected == PHONE_CODEC_APTX && aptx != NULL) ||
           (selected == PHONE_CODEC_LHDCV5 && lhdc48_ready());
}

static uint8_t crc_bit(uint8_t crc, unsigned bit) {
    unsigned feedback = (crc >> 7) ^ bit;
    return (uint8_t)((crc << 1) ^ (feedback ? 0x1du : 0u));
}

static size_t sbc_frame_length(const uint8_t *data, size_t available) {
    if (available < 4 || data[0] != 0x9c || data[1] >> 6 != 3) return 0;
    unsigned blocks = 4u * (1u + ((data[1] >> 4) & 3u));
    unsigned mode = (data[1] >> 2) & 3u;
    unsigned bands = (data[1] & 1u) ? 8u : 4u;
    unsigned allocation = (data[1] >> 1) & 1u;
    unsigned bitpool = data[2];
    if (blocks != configuration.blocks || mode != configuration.channel_mode ||
        bands != configuration.subbands || allocation != configuration.allocation ||
        bitpool < configuration.min_bitpool || bitpool > configuration.max_bitpool)
        return 0;
    unsigned joint_bits = mode == 3 ? bands : 0;
    size_t size = 4u + bands + (joint_bits + blocks * bitpool + 7u) / 8u;
    if (size > available) return 0;
    uint8_t crc = 0x0f;
    for (unsigned byte = 1; byte <= 2; ++byte)
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = crc_bit(crc, (data[byte] >> (7u - bit)) & 1u);
    for (unsigned bit = 0; bit < joint_bits + bands * 8u; ++bit)
        crc = crc_bit(crc, (data[4u + bit / 8u] >> (7u - bit % 8u)) & 1u);
    return crc == data[3] ? size : 0;
}

static bool decode_sbc(const uint8_t *payload, size_t length, uint8_t frames,
                       phone_codec_pcm_callback callback, void *context) {
    if (frames == 0 || frames > 15) return false;
    size_t offset = 0;
    for (unsigned i = 0; i < frames; ++i) {
        size_t bytes = sbc_frame_length(payload + offset, length - offset);
        if (bytes == 0) return false;
        offset += bytes;
    }
    if (offset != length) return false;
    // Validate every header/length/CRC before decoding any part of the packet.
    for (unsigned i = 0; i < frames; ++i) {
        int16_t pcm[256];
        OI_UINT32 remaining = (OI_UINT32)length, pcm_bytes = sizeof(pcm);
        const OI_BYTE *next = payload;
        if (OI_CODEC_SBC_DecodeFrame(&sbc, &next, &remaining, pcm, &pcm_bytes)
                != OI_STATUS_SUCCESS || remaining >= length || pcm_bytes == 0 ||
            pcm_bytes > sizeof(pcm) || pcm_bytes % 4u != 0)
            return false;
        size_t consumed = length - remaining;
        payload += consumed;
        length -= consumed;
        callback(pcm, (uint16_t)(pcm_bytes / 4u), context);
    }
    return length == 0;
}

static bool decode_aptx(const uint8_t *payload, size_t length,
                        phone_codec_pcm_callback callback, void *context) {
    if (length % 4u != 0) return false;
    while (length != 0) {
        unsigned char raw[216];
        int16_t pcm[72];
        size_t bytes = length > 32u ? 32u : length, written = 0;
        int synced;
        size_t dropped;
        // Classic aptX has no RTP sequence; parity recovery must tolerate lost words.
        size_t consumed = aptx_decode_sync(aptx, payload, bytes, raw, sizeof(raw), &written,
                                          &synced, &dropped);
        if (consumed != bytes || written > sizeof(raw) || written % 6u != 0) return false;
        for (size_t i = 0; i < written / 3u; ++i) {
            // aptX emits signed little-endian 24-bit PCM; retain its top 16 bits.
            uint16_t bits = (uint16_t)(raw[i * 3u + 1u] | ((uint16_t)raw[i * 3u + 2u] << 8));
            memcpy(&pcm[i], &bits, sizeof(bits));
        }
        if (written != 0) callback(pcm, (uint16_t)(written / 6u), context);
        payload += consumed;
        length -= consumed;
    }
    return true;
}

bool phone_codec_decode(const uint8_t *payload, size_t length, uint8_t frames,
                        phone_codec_pcm_callback callback, void *context) {
    if (!phone_codec_ready() || payload == NULL || callback == NULL ||
        length == 0 || length > PHONE_CODEC_MAX_PAYLOAD) return false;
    bool ok = selected == PHONE_CODEC_SBC ? decode_sbc(payload, length, frames, callback, context) :
              selected == PHONE_CODEC_LHDCV5 ? lhdc48_decode(payload, length, frames, callback, context) :
              frames == 0 && decode_aptx(payload, length, callback, context);
    if (!ok) phone_codec_reset();
    return ok;
}
