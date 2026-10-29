// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_DONGLE_PLAYBACK_ELD_H
#define AIRPODS_DONGLE_PLAYBACK_ELD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PLAYBACK_ELD_CONFIG_SIZE 14u
#define PLAYBACK_ELD_BITRATE 256000u
#define PLAYBACK_ELD_MIN_BITRATE 128000u
#define PLAYBACK_ELD_HEADER_SIZE 4u
#define PLAYBACK_ELD_FRAME_SAMPLES 480u
#define PLAYBACK_ELD_PAYLOAD_CEILING 986u

static inline uint32_t playback_eld_au_ceiling(uint32_t bitrate, bool variable) {
    // FDK CBR permits one alignment byte beyond the rounded nominal frame.
    return (bitrate + 799u) / 800u + (variable ? 0u : 1u);
}

static inline bool playback_eld_capability(const uint8_t *cap, size_t len) {
    static const uint8_t id[6] = {0x4c, 0, 0, 0, 1, 0x80};
    return cap != NULL && len == PLAYBACK_ELD_CONFIG_SIZE &&
           memcmp(cap, id, sizeof(id)) == 0 &&
           (cap[7] & 0x80) && (cap[9] & 0x80) && (cap[9] & 4);
}

static inline uint32_t playback_eld_bitrate(const uint8_t *config) {
    return ((uint32_t)(config[11] & 0x7f) << 16) |
           ((uint32_t)config[12] << 8) | config[13];
}

static inline bool playback_eld_variable(const uint8_t *config) {
    return (config[11] & 0x80) != 0;
}

static inline bool playback_eld_negotiate(const uint8_t *cap, size_t len,
                                         uint8_t config[PLAYBACK_ELD_CONFIG_SIZE]) {
    if (!playback_eld_capability(cap, len)) return false;
    uint32_t bitrate = playback_eld_bitrate(cap);
    uint8_t permission = cap[11] & 0x80;
    if (bitrate == 0 || bitrate > PLAYBACK_ELD_BITRATE) bitrate = PLAYBACK_ELD_BITRATE;
    if (bitrate < PLAYBACK_ELD_MIN_BITRATE) return false;
    static const uint8_t fixed[14] = {0x4c, 0, 0, 0, 1, 0x80, 0, 0x80, 0, 0x84, 0, 0, 0, 0};
    memcpy(config, fixed, sizeof(fixed));
    config[11] = (uint8_t)(bitrate >> 16) | permission;
    config[12] = (uint8_t)(bitrate >> 8);
    config[13] = (uint8_t)bitrate;
    return true;
}

static inline bool playback_eld_configuration(const uint8_t *config, size_t len) {
    if (!playback_eld_capability(config, len)) return false;
    return config[6] == 0 && config[7] == 0x80 && config[8] == 0 &&
           config[9] == 0x84 && config[10] == 0 &&
           playback_eld_bitrate(config) >= PLAYBACK_ELD_MIN_BITRATE &&
           playback_eld_bitrate(config) <= PLAYBACK_ELD_BITRATE;
}

static inline bool playback_eld_configuration_supported(const uint8_t *config, size_t len,
                                                         const uint8_t *cap, size_t cap_len) {
    if (!playback_eld_configuration(config, len) || !playback_eld_capability(cap, cap_len)) return false;
    uint32_t ceiling = playback_eld_bitrate(cap);
    return (!playback_eld_variable(config) || playback_eld_variable(cap)) &&
           (!ceiling || playback_eld_bitrate(config) <= ceiling);
}

static inline bool playback_eld_append(uint8_t *packet, size_t capacity,
                                      uint16_t *used, uint16_t *sequence,
                                      size_t au_size) {
    if (au_size == 0 || au_size > 2047 || *used > capacity ||
        capacity - *used < PLAYBACK_ELD_HEADER_SIZE ||
        au_size > capacity - *used - PLAYBACK_ELD_HEADER_SIZE) return false;
    uint16_t seq = *sequence & 0xfff;
    packet[*used] = 0xb0 | (uint8_t)(seq >> 8);
    packet[*used + 1] = (uint8_t)seq;
    packet[*used + 2] = 0x10 | (uint8_t)(au_size >> 8);
    packet[*used + 3] = (uint8_t)au_size;
    *used += PLAYBACK_ELD_HEADER_SIZE + (uint16_t)au_size;
    *sequence = (seq + 1) & 0xfff;
    return true;
}

#endif
