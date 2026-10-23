//
// Created by Sean on 8/7/23.
//

#ifndef PICOW_USB_BT_AUDIO_SSP_COUNTER_H
#define PICOW_USB_BT_AUDIO_SSP_COUNTER_H

#include <stdint.h>
#include <stdbool.h>

// The packed 32KB PCM pool holds priming plus the existing 60ms ELD reserve.
#define AUDIO_SLOT_COUNT_SBC   8
#define AUDIO_SLOT_COUNT_AAC   8
#define AUDIO_SLOT_COUNT_ELD   16
#define AUDIO_SLOT_MAX_SAMPLES 1024
#define AUDIO_SLOT_MAX_INT16   (AUDIO_SLOT_MAX_SAMPLES * 2)

// Slot queue API (multicore-safe)
void audio_slot_queue_init(void);
void audio_slot_push_samples(const int16_t *src, uint16_t stereo_pair_count);

bool check_is_streaming();
bool dongle_playback_transitioning(void);

void set_bt_volume(int16_t db256);
bool get_bt_volume(uint8_t *level);
void bt_volume_poll(void);
void bt_volume_cancel_phone(void);

void set_usb_streaming(bool flag);
bool get_usb_streaming(void);

bool * get_is_bt_sink_volume_changed_ptr();

int btstack_main(int argc, const char * argv[]);

bool avdtp_disconnect_and_scan(void);

bool gap_start_scanning(void);

bool get_a2dp_connected_flag();
uint8_t dongle_output_codec(void);
uint8_t dongle_playback_configure(uint8_t codec, uint8_t rate_mode, uint32_t bitrate);
uint8_t dongle_playback_game_mode(bool enabled);
uint16_t dongle_playback_status(uint8_t *out, uint16_t capacity);
uint16_t dongle_playback_diagnostics(uint8_t *out, uint16_t capacity);
void dongle_playback_poll(void);

void a2dp_source_reconnect();

void avdtp_source_establish_stream();

void set_next_capablity_and_start_stream();

void start_led_blink();

static int setup_aac_configuration();

static int setup_sbc_configuration();

bool get_allow_switch_slot();


void increase_vol_by_key();

void decrease_vol_by_key();

#endif //PICOW_USB_BT_AUDIO_SSP_COUNTER_H
