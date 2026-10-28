// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PHONE_CALL_H
#define AIRPODS_PHONE_CALL_H

#include <stdbool.h>
#include <stdint.h>

// Register after shared RFCOMM/SDP setup and before powering on Bluetooth.
bool phone_call_init(void);
// Main-loop thread, with the Bluetooth async lock held.
void phone_call_poll(void);
// Aggregate snapshot only on an explicit CDC timing request, under the same lock.
void phone_call_report(void);
bool phone_call_active(void);
bool phone_call_audio_requested(void);
bool phone_call_mic_requested(void);
// Only a current, authenticated HFP phone may open a synchronous voice link.
bool phone_call_sco_allowed(const uint8_t address[6]);
// Called only for a validated AirPods stem PRESS, before media dispatch.
bool phone_call_media_control(uint8_t operation);

#endif
