//
// Created by Sean on 8/7/23.
//

#ifndef PICOW_USB_BT_AUDIO_SSP_COUNTER_H
#define PICOW_USB_BT_AUDIO_SSP_COUNTER_H

#include <stdint.h>
#include <stdbool.h>

// The float PCM pool supports priming; live USB occupancy is bounded separately.
#define AUDIO_SLOT_COUNT_SBC   8
#define AUDIO_SLOT_COUNT_AAC   8
#define AUDIO_SLOT_COUNT_ELD   16
#define AUDIO_SLOT_MAX_SAMPLES 1024
#define AUDIO_SLOT_MAX_INT16   (AUDIO_SLOT_MAX_SAMPLES * 2)

// Slot queue API (multicore-safe)
void audio_slot_queue_init(void);
void audio_slot_push_samples(const int16_t *src, uint16_t stereo_pair_count);
void audio_slot_push_float(const float *src, uint16_t stereo_pair_count);
void audio_slot_usb_reset(void);

bool check_is_streaming();
bool dongle_playback_transitioning(void);
// Capture startup waits for normal buffering; cleanup requires a confirmed media restart.
bool dongle_playback_mic_ready(void);
void dongle_playback_mic_request_changed(void);
bool dongle_playback_mic_recover(void);
uint32_t dongle_playback_mic_recovery_revision(void);
void dongle_playback_mic_recovery_abort(void);

void set_bt_volume(int16_t db256);
bool get_bt_volume(uint8_t *level);
void bt_volume_poll(void);
void bt_volume_cancel_phone(void);

void set_usb_streaming(bool flag);
bool get_usb_streaming(void);
bool bridge_call_open(void);
bool bridge_call_ready(void);
void bridge_call_stop(void);
void bridge_call_pcm_push(const int16_t *stereo, uint16_t frames);

bool * get_is_bt_sink_volume_changed_ptr();

int btstack_main(int argc, const char * argv[]);

bool avdtp_disconnect_and_scan(void);

bool gap_start_scanning(void);

bool get_a2dp_connected_flag();
uint8_t dongle_output_codec(void);
uint8_t dongle_playback_configure(uint8_t codec, uint8_t rate_mode, uint32_t bitrate);
uint8_t dongle_playback_game_mode(bool enabled);
uint8_t dongle_playback_game_toggle(void);
uint16_t dongle_playback_status(uint8_t *out, uint16_t capacity);
uint16_t dongle_playback_diagnostics(uint8_t *out, uint16_t capacity);
uint16_t dongle_playback_trace(uint8_t *out, uint16_t capacity);
uint16_t dongle_playback_inspect(uint8_t *out, uint16_t capacity);
void dongle_playback_inspect_enable(bool enabled);
void dongle_playback_usb_packet(uint32_t us, uint16_t frames, uint8_t format, uint8_t error);
void dongle_playback_usb_format(uint8_t format);
void dongle_playback_controller_event(const uint8_t *packet, uint16_t size);
void dongle_playback_poll(void);

void a2dp_source_reconnect();

void avdtp_source_establish_stream();

void set_next_capablity_and_start_stream();



bool get_allow_switch_slot();


void increase_vol_by_key();

void decrease_vol_by_key();

#endif //PICOW_USB_BT_AUDIO_SSP_COUNTER_H
