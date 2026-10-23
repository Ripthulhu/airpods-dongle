// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PHONE_SINK_H
#define AIRPODS_PHONE_SINK_H

#include <stdbool.h>
#include <stdint.h>

// Initialize after shared AVRCP setup, before Bluetooth is powered on.
bool phone_sink_init(void);
// Thread context, with the Bluetooth async lock held; codec allocation stays here.
void phone_sink_poll(void);
// Thread context, under the Bluetooth lock: release phone decoder before USB mic allocation.
void phone_sink_yield(void);
bool phone_sink_decoder_released(void);
bool phone_sink_connected(void);
bool phone_sink_active(void);
bool phone_sink_owns_controls(void);

// Shared AVRCP dispatchers call these first and skip their AirPods path if handled.
bool phone_sink_handle_avrcp(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size);
bool phone_sink_handle_controller(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size);
bool phone_sink_handle_target(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size);
bool phone_sink_media_control(uint8_t operation);
void phone_sink_volume_changed(uint8_t absolute_volume);
void phone_sink_volume_invalidated(void);

// AirPods/USB ownership boundary; only the selected phone input can publish PCM.
bool bridge_phone_open(void);
bool bridge_phone_ready(void);
bool bridge_phone_start(void);
void bridge_phone_stop(void);
void bridge_phone_pcm_push(const int16_t *stereo, uint16_t frames);
void bridge_phone_drop_pcm(void);
bool bridge_phone_volume(uint8_t absolute_volume);
bool bridge_phone_get_volume(uint8_t *absolute_volume);
void bt_volume_cancel_phone(void);

#endif
