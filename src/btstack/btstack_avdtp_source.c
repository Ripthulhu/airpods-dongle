#include <sys/cdefs.h>
/*
 * Copyright (C) 2016 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */


/*
 * 
*/

#include <stdint.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include "btstack.h"
#include "btstack_avdtp_source.h"
#include "pico/platform/sections.h"
#include "pico/sync.h"
#include "playback_pcm.h"
#include "playback_eld.h"
#include "playback_peer.h"
#include "playback_format.h"
#include "playback_options.h"
#include "pico/time.h"
#include "phone_sink.h"
#include "phone_clock.h"
#include "btstack_resample.h"
#include "../tinyusb/uac.h"

#include "btstack_hci.h"
#include "btstack_ble_scan.h"
#include "btstack_aacp.h"
#include "classic/device_id_server.h"

#include "../pico_w_led.h"
#include "../slot_storage.h"
#include "../tinyusb/media_control.h"

#define HAVE_AAC_FDK

#define A2DP_CODEC_VENDOR_ID_APPLE 0x004C
#define A2DP_APPLE_CODEC_AAC_ELD 0x8001


#include <aacenc_lib.h>


#define A2DP_CODEC_VENDOR_ID_FRAUNHOFER 0x08A9
#define A2DP_FRAUNHOFER_CODEC_LC3PLUS 0x0001

#define AVDTP_MAX_SEP_NUM 10
#define AVDTP_MAX_MEDIA_CODEC_CONFIG_LEN 16
#define AVDTP_MAX_MEDIA_CODEC_CAPABILITES_EVENT_LEN 100

#define VOLUME_REDUCTION 2

typedef struct {
    // bitmaps
    uint8_t sampling_frequency_bitmap;
    uint8_t channel_mode_bitmap;
    uint8_t block_length_bitmap;
    uint8_t subbands_bitmap;
    uint8_t allocation_method_bitmap;
    uint8_t min_bitpool_value;
    uint8_t max_bitpool_value;
} media_codec_information_sbc_t;

typedef struct {
    int reconfigure;
    int num_channels;
    int sampling_frequency;
    int block_length;
    int subbands;
    int min_bitpool_value;
    int max_bitpool_value;

    int channel_mode;
    int allocation_method;
} avdtp_media_codec_configuration_sbc_t;


#ifdef HAVE_BTSTACK_STDIN

#endif

typedef struct {
    uint16_t avdtp_cid;
    uint8_t  local_seid;
    uint8_t  remote_seid;
    uint16_t avrcp_cid;

    uint32_t time_audio_data_sent; // ms
    uint32_t acc_num_missed_samples;
    uint32_t samples_ready;
    btstack_timer_source_t audio_timer;
    uint8_t  streaming;
    int      max_media_payload_size;

    uint32_t rtp_timestamp;
    uint32_t send_wait_start;


    uint8_t  codec_storage[PLAYBACK_ELD_PAYLOAD_CEILING];
    uint16_t codec_storage_count;
    uint8_t  codec_ready_to_send;
    uint16_t codec_num_frames;
    uint8_t volume;
}  a2dp_media_sending_context_t;




#ifdef HAVE_AAC_FDK
static HANDLE_AACENCODER handleAAC;
static AACENC_InfoStruct aacinf;
#endif

static a2dp_media_sending_context_t media_tracker;

static int current_sample_rate = 48000;

static uint8_t sdp_avdtp_source_service_buffer[150];

static uint16_t     num_remote_seps;
static uint16_t     cur_num_remote_seps;
static bool        remote_catalog_truncated;

static uint8_t cur_capability = 0;
static bool is_streaming = false;
static bool phone_opened, phone_active, phone_buffer_started;
static phone_clock_t phone_clock;
static btstack_resample_t phone_resampler;
static void playback_media_sent(void);
static uint8_t cur_codec = 0;
static playback_options_t playback_options;
static playback_game_t playback_game;
static uint32_t actual_bitrate, delay_revision, game_session;
static uint16_t reported_delay=UINT16_MAX;
static bool profile_pending, profile_reconnect;
static uint32_t profile_since;
static playback_options_t profile_previous;
static uint8_t profile_peer[6];
static uint8_t game_peer[6];
enum { PLAYBACK_RUNNING, PLAYBACK_SUSPENDING, PLAYBACK_SUSPENDED, PLAYBACK_STARTING };
static uint8_t playback_idle_phase;
static bool playback_idle_counting;
static uint32_t playback_idle_since, playback_idle_transition_since;
#define PLAYBACK_IDLE_MS 1000u
#define PLAYBACK_TRANSITION_MS 3000u
static struct {
    uint32_t pcm_peak, pcm_underflows, pcm_drops, encoded_units;
    uint32_t encoder_last_us, encoder_max_us, send_failures, recoveries;
} playback_metrics;

uint8_t dongle_output_codec(void) {
    return get_a2dp_connected_flag() ? (cur_codec == 4 ? 3 : cur_codec <= 2 ? cur_codec : 0) : 0;
}

static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

typedef struct {
    uint8_t track_id[8];
    uint32_t song_length_ms;
    avrcp_playback_status_t status;
    uint32_t song_position_ms; // 0xFFFFFFFF if not supported
} avrcp_play_status_info_t;


avrcp_track_t tracks[] = {
    {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}, 2, "USB Audio", "Pico", "wasdwasd0105", "vivid", 12345},
};

avrcp_play_status_info_t play_info;

static uint8_t sdp_avrcp_target_service_buffer[200];
static uint8_t sdp_avrcp_controller_service_buffer[200];


static uint16_t                  media_codec_config_len;
static uint8_t                   media_codec_config_data[AVDTP_MAX_MEDIA_CODEC_CONFIG_LEN];

static struct {
    uint8_t seid;
    avdtp_media_codec_type_t codec_type;
    bool have_media_codec_capabilities;
    bool delay_reporting;
    uint8_t                  media_codec_event[AVDTP_MAX_MEDIA_CODEC_CAPABILITES_EVENT_LEN];
    uint32_t                 vendor_id;
    uint32_t                 codec_id;
} remote_seps[AVDTP_MAX_SEP_NUM];
static uint8_t selected_remote_sep_index;

static struct {
    avdtp_stream_endpoint_t * local_stream_endpoint;
}  sc;


// setup stream endpoints

avdtp_stream_endpoint_t * stream_endpoint_sbc;
avdtp_stream_endpoint_t * stream_endpoint_aac;
avdtp_stream_endpoint_t * stream_endpoint_aaceld;


static btstack_sbc_encoder_state_t sbc_encoder_state;
static uint8_t is_cmd_triggered_localy = 0;

static bool a2dp_is_connected_flag = false;
static bool a2dp_connect_pending;
static bool a2dp_accept_incoming;

static bool finish_scan_avdtp_codec = false;

static uint8_t audio_timer_interval = 5;

// on pico 2w the max stable aac bit rate under 512 simples without vbr is around 220000
static uint8_t aac_audio_timer_interval = 12;
static uint16_t acc_num_simples = 1024;
static int max_aac_bit_rate = 220000;


static const uint8_t media_sbc_codec_capabilities[] = {
    0x17, // 48 kHz; stereo, joint stereo and dual channel.
    0xFF,//(AVDTP_SBC_BLOCK_LENGTH_16 << 4) | (AVDTP_SBC_SUBBANDS_8 << 2) | AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS,
    2, 53
};


#ifdef HAVE_AAC_FDK
static uint8_t media_aac_codec_capabilities[] = {
        0xC0, // MPEG-2/4 AAC-LC
        0x00,
        0x84, // 48 kHz stereo
        0x03, 0x5B, 0x60 // CBR, 220 kbps ceiling
};

typedef struct {
    int reconfigure;
    int sampling_frequency_bitmap;
    int object_type_bitmap;
    int bit_rate;
    int channels_bitmap;
    int vbr;
} avdtp_media_codec_capabilities_aac_t;

#endif

// Apple AAC-ELD vendor codec capabilities (vendor_id=0x004C, codec_id=0x8001)
// Format: vendor_id(4 LE) + codec_id(2 LE) + obj_type(2) + freq+ch(2) + rsvd(1) + vbr+bitrate(3)
static uint8_t media_aaceld_codec_capabilities[] = {
        0x4C, 0x00, 0x00, 0x00,   // vendor_id = 0x004C (Apple)
        0x01, 0x80,                // codec_id  = 0x8001
        0x00, 0x80,                // object_type = 0x0080 (AAC-ELD)
        0x00, 0x84,                // freq bitmap 0x008 (48 kHz) + stereo
        0x00,                      // reserved
        0x83, 0xE8, 0x00           // VBR=1, bitrate=256000
};

// configurations for local stream endpoints
static uint8_t local_stream_endpoint_sbc_media_codec_configuration[4];
static uint8_t local_stream_endpoint_aaceld_media_codec_configuration[14];

#ifdef HAVE_AAC_FDK
static uint8_t local_stream_endpoint_aac_media_codec_configuration[6];
#endif

bool get_a2dp_connected_flag(){
    return a2dp_is_connected_flag;
}

static int a2dp_sample_rate(void){
    return current_sample_rate;
}

static const char * codec_name_for_type(avdtp_media_codec_type_t codec_type){
    switch (codec_type){
        case AVDTP_CODEC_SBC:            return "SBC";
        case AVDTP_CODEC_MPEG_1_2_AUDIO: return "MPEG_1_2_AUDIO";
        case AVDTP_CODEC_MPEG_2_4_AAC:   return "MPEG_2_4_AAC";
        case AVDTP_CODEC_ATRAC_FAMILY:   return "ATRAC_FAMILY";
        case AVDTP_CODEC_NON_A2DP:       return "NON_A2DP/Vendor";
        default:                         return "Unknown";
    }
}

static void playback_prepare_packet(a2dp_media_sending_context_t *, uint32_t);
static void a2dp_demo_send_media_packet(void) {
    a2dp_media_sending_context_t *context = &media_tracker;
    if (!context->streaming || !context->codec_ready_to_send ||
        !a2dp_is_connected_flag || context->codec_storage_count == 0) return;
    uint16_t payload_size = context->codec_storage_count;
    uint32_t samples = 0;
    switch (cur_codec) {
        case 1: {
            int frame_size = btstack_sbc_encoder_sbc_buffer_length();
            if (frame_size <= 0) return;
            uint8_t frames = context->codec_storage_count / frame_size;
            if (!frames || frames > 15) return;
            context->codec_storage[0] = frames;
            payload_size++;
            samples = frames * btstack_sbc_encoder_num_audio_frames();
            break;
        }
        case 2: samples = context->codec_num_frames * acc_num_simples; break;
        case 4: samples = context->codec_num_frames * PLAYBACK_ELD_FRAME_SAMPLES; break;
        default: return;
    }
    uint8_t status = a2dp_source_stream_send_media_payload_rtp(
        context->avdtp_cid, context->local_seid, 0, context->rtp_timestamp,
        context->codec_storage, payload_size);
    if (status != ERROR_CODE_SUCCESS) {
        ++playback_metrics.send_failures;
        context->codec_ready_to_send = 2; // Retry from the main loop, not a synchronous can-send callback.
        return;
    }
    // Keep the fork's sample clock; failed sends retain the same payload and timestamp.
    context->rtp_timestamp += samples;
    playback_media_sent();
    context->codec_storage_count = 0;
    context->codec_ready_to_send = 0;
    context->codec_num_frames = 0;
    playback_prepare_packet(context, btstack_run_loop_get_time_ms());
}

static uint32_t get_vendor_id(const uint8_t *codec_info) {
    uint32_t vendor_id = 0;
    vendor_id |= codec_info[0];
    vendor_id |= codec_info[1] << 8;
    vendor_id |= codec_info[2] << 16;
    vendor_id |= (uint32_t)codec_info[3] << 24;
    return vendor_id;
}

static uint16_t get_codec_id(const uint8_t *codec_info) {
    uint16_t codec_id = 0;
    codec_id |= codec_info[4];
    codec_id |= codec_info[5] << 8;
    return codec_id;
}

static playback_pcm_t playback_pcm;
static critical_section_t playback_pcm_lock;
static bool playback_pcm_initialized;
// The unused core1 bank holds this serialized buffer; the linker rejects a core1 stack here.
static int16_t __scratch_x("encoder_pcm") encoder_pcm[AUDIO_SLOT_MAX_INT16];
static uint16_t pcm_frame_samples = 128;
static uint8_t active_slot_count = AUDIO_SLOT_COUNT_SBC;
static bool is_usb_streaming;
static bool have_aaceld_codec_capabilities;
bd_addr_t cur_active_device;
static uint16_t aaceld_frame_sequence = 1;
static uint32_t aaceld_bitrate = PLAYBACK_ELD_BITRATE;
bool finish_setup_aac;
bool allow_switch_slot = true;
static btstack_timer_source_t suspend_recovery_timer;
static bool suspend_recovery_pending;
static bool recovery_reset_encoder;
#define SUSPEND_RECOVERY_TIMEOUT_MS 3000

static void playback_media_sent(void) {
    critical_section_enter_blocking(&playback_pcm_lock);
    playback_pcm_sent(&playback_pcm);
    critical_section_exit(&playback_pcm_lock);
}
static void playback_pcm_push_measured(const int16_t *src, uint32_t frames) {
    if (playback_pcm.capacity && src && frames) {
        uint32_t available=playback_pcm.capacity-playback_pcm.used;
        if (frames>available) playback_metrics.pcm_drops+=frames-available;
    }
    playback_pcm_push(&playback_pcm,src,frames);
    if (playback_pcm.used>playback_metrics.pcm_peak) playback_metrics.pcm_peak=playback_pcm.used;
}
static void playback_encode_measured(uint32_t started) {
    uint32_t elapsed=time_us_32()-started;
    playback_metrics.encoder_last_us=elapsed;
    if (elapsed>playback_metrics.encoder_max_us) playback_metrics.encoder_max_us=elapsed;
}

bool bridge_phone_open(void) {
    if (is_usb_streaming || usb_mic_requested() ||
        !peer_address_valid((const uint8_t *)get_device_addr())) return false;
    phone_opened = true;
    avdtp_source_establish_stream();
    return true;
}

bool bridge_phone_ready(void) {
    if (!phone_opened || is_usb_streaming || usb_mic_requested() ||
        !a2dp_is_connected_flag || !media_tracker.streaming || playback_idle_phase != PLAYBACK_RUNNING) return false;
    critical_section_enter_blocking(&playback_pcm_lock);
    bool warm = playback_pcm.primed_on_wire;
    critical_section_exit(&playback_pcm_lock);
    return warm;
}

bool bridge_phone_start(void) {
    if (!bridge_phone_ready()) return false;
    critical_section_enter_blocking(&playback_pcm_lock);
    playback_pcm_clear(&playback_pcm, PHONE_PCM_CAPACITY);
    phone_clock_init(&phone_clock, acc_num_simples);
    btstack_resample_init(&phone_resampler, 2);
    phone_buffer_started = false;
    phone_active = true;
    critical_section_exit(&playback_pcm_lock);
    return true;
}

void bridge_phone_drop_pcm(void) {
    critical_section_enter_blocking(&playback_pcm_lock);
    if (phone_active) {
        playback_pcm_clear(&playback_pcm, PHONE_PCM_CAPACITY);
        phone_buffer_started = false;
        phone_clock_init(&phone_clock, acc_num_simples);
        btstack_resample_init(&phone_resampler, 2);
    }
    critical_section_exit(&playback_pcm_lock);
}

void bridge_phone_stop(void) {
    critical_section_enter_blocking(&playback_pcm_lock);
    if (phone_active)
        playback_pcm_clear(&playback_pcm, pcm_frame_samples * active_slot_count);
    phone_opened = phone_active = phone_buffer_started = false;
    critical_section_exit(&playback_pcm_lock);
}

void bridge_phone_pcm_push(const int16_t *src, uint16_t frames) {
    if (src == NULL || !phone_active || !bridge_phone_ready()) return;
    while (frames != 0) {
        uint16_t count = frames > 128 ? 128 : frames;
        int16_t adjusted[130 * 2];
        uint16_t output = btstack_resample_block(&phone_resampler, src, count, adjusted);
        if (output > 130) { bridge_phone_drop_pcm(); return; }
        critical_section_enter_blocking(&playback_pcm_lock);
        playback_pcm_push_measured(adjusted, output);
        critical_section_exit(&playback_pcm_lock);
        src += count * 2;
        frames -= count;
    }
}

void bridge_phone_volume(uint8_t volume) {
    if (volume > 127 || is_usb_streaming || usb_mic_requested()) return;
    media_tracker.volume = volume;
    if (media_tracker.avrcp_cid != 0)
        avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, volume);
}

uint8_t bridge_phone_get_volume(void) { return media_tracker.volume; }

static bool a2dp_demo_timer_start(a2dp_media_sending_context_t *context);
static void a2dp_demo_timer_stop(a2dp_media_sending_context_t *context);
static void playback_idle_reset(void);

static void suspend_recovery_handler(btstack_timer_source_t *timer) {
    (void)timer;
    if (!suspend_recovery_pending || !a2dp_is_connected_flag) return;
    suspend_recovery_pending = false;
    // A timed-out peer has not confirmed a reset; release the connection instead.
    aacp_disconnect();
    a2dp_source_disconnect(media_tracker.avdtp_cid);
}

void audio_slot_queue_init(void) {
    if (!playback_pcm_initialized) {
        critical_section_init(&playback_pcm_lock);
        playback_pcm_initialized = true;
    }
    critical_section_enter_blocking(&playback_pcm_lock);
    playback_pcm_reset(&playback_pcm, pcm_frame_samples * active_slot_count, false);
    critical_section_exit(&playback_pcm_lock);
}

static void audio_slot_queue_configure_with_count(uint16_t samples_per_slot, uint8_t slot_count) {
    if (samples_per_slot == 0 || samples_per_slot > AUDIO_SLOT_MAX_SAMPLES ||
        slot_count == 0 || (uint32_t)samples_per_slot * slot_count > PLAYBACK_PCM_FRAMES) return;
    critical_section_enter_blocking(&playback_pcm_lock);
    pcm_frame_samples = samples_per_slot;
    active_slot_count = slot_count;
    playback_pcm_reset(&playback_pcm, (uint32_t)samples_per_slot * slot_count, false);
    critical_section_exit(&playback_pcm_lock);
}

void audio_slot_push_samples(const int16_t *src, uint16_t stereo_pair_count) {
    critical_section_enter_blocking(&playback_pcm_lock);
    if (!phone_active) playback_pcm_push_measured(src, stereo_pair_count);
    critical_section_exit(&playback_pcm_lock);
}

static void audio_pcm_read(uint16_t frames) {
    critical_section_enter_blocking(&playback_pcm_lock);
    if (phone_active && !phone_buffer_started) {
        if (playback_pcm.used < phone_clock_prefill(acc_num_simples)) {
            memset(encoder_pcm, 0, (size_t)frames * 2 * sizeof(*encoder_pcm));
            critical_section_exit(&playback_pcm_lock);
            return;
        }
        phone_buffer_started = true;
    }
    uint32_t priming=playback_pcm.priming<frames ? playback_pcm.priming : frames;
    uint32_t copied=playback_pcm_read(&playback_pcm, encoder_pcm, frames);
    if (is_usb_streaming || phone_active) playback_metrics.pcm_underflows+=frames-priming-copied;
    if (phone_active)
        btstack_resample_set_factor(&phone_resampler,
                                   phone_clock_update(&phone_clock, playback_pcm.used, frames));
    critical_section_exit(&playback_pcm_lock);
}

bool check_is_streaming(){
    return is_streaming;
}
bool dongle_playback_transitioning(void) {
    return playback_idle_phase == PLAYBACK_SUSPENDING || playback_idle_phase == PLAYBACK_STARTING;
}

bool get_usb_streaming(void) { return is_usb_streaming; }

void set_usb_streaming(bool flag){
    if (is_usb_streaming && !flag && !phone_active) {
        critical_section_enter_blocking(&playback_pcm_lock);
        playback_pcm_clear(&playback_pcm, pcm_frame_samples * active_slot_count);
        critical_section_exit(&playback_pcm_lock);
    }
    is_usb_streaming = flag;
}

bool get_allow_switch_slot(){
    return allow_switch_slot;
}


static int fill_sbc_audio_buffer(a2dp_media_sending_context_t * context){
    int total_num_bytes_read = 0;
    unsigned int num_audio_samples_per_sbc_buffer = btstack_sbc_encoder_num_audio_frames();

    while (context->samples_ready >= num_audio_samples_per_sbc_buffer &&
           context->codec_num_frames < 15 &&
           (context->max_media_payload_size - context->codec_storage_count - 1) >= btstack_sbc_encoder_sbc_buffer_length()){

        audio_pcm_read((uint16_t)num_audio_samples_per_sbc_buffer);
        uint32_t started=time_us_32();
        btstack_sbc_encoder_process_data(encoder_pcm);
        playback_encode_measured(started);
        ++playback_metrics.encoded_units;

        uint16_t sbc_frame_size = btstack_sbc_encoder_sbc_buffer_length();
        uint8_t * sbc_frame = btstack_sbc_encoder_sbc_buffer();

        total_num_bytes_read += num_audio_samples_per_sbc_buffer;

        memcpy(&context->codec_storage[1 + context->codec_storage_count], sbc_frame, sbc_frame_size);
        context->codec_storage_count += sbc_frame_size;
        ++context->codec_num_frames;
        context->samples_ready -= num_audio_samples_per_sbc_buffer;

    }

    return total_num_bytes_read;
}

#ifdef HAVE_AAC_FDK

static int playback_encoder_failed(a2dp_media_sending_context_t *context) {
    a2dp_demo_timer_stop(context);
    finish_scan_avdtp_codec = false;
    aacp_disconnect();
    a2dp_source_disconnect(context->avdtp_cid);
    return -1;
}

static bool aaceld_encoder_init(void) {
    if (handleAAC != NULL) aacEncClose(&handleAAC);
    handleAAC = NULL;
    if (aacEncOpen(&handleAAC, 0x01, 2) != AACENC_OK) return false;
    const struct { AACENC_PARAM key; UINT value; } params[] = {
        {AACENC_AOT, 39}, {AACENC_BITRATE, aaceld_bitrate},
        {AACENC_SAMPLERATE, 48000}, {AACENC_CHANNELMODE, MODE_2},
        {AACENC_GRANULE_LENGTH, PLAYBACK_ELD_FRAME_SAMPLES},
        {AACENC_SBR_MODE, 0}, {AACENC_BITRATEMODE, playback_options.rate_mode ? 5u : 0u},
        {AACENC_PEAK_BITRATE, aaceld_bitrate}, {AACENC_AFTERBURNER, 0},
        {AACENC_TRANSMUX, TT_MP4_RAW}
    };
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); ++i) {
        if (aacEncoder_SetParam(handleAAC, params[i].key, params[i].value) != AACENC_OK) goto fail;
    }
    if (playback_options.rate_mode) {
        unsigned bandwidth=aaceld_bitrate<=128000 ? 15500 : aaceld_bitrate<=160000 ? 17500 : 20000;
        if (aacEncoder_SetParam(handleAAC,AACENC_BANDWIDTH,bandwidth)!=AACENC_OK) goto fail;
    }
    if (aacEncEncode(handleAAC, NULL, NULL, NULL, NULL) != AACENC_OK ||
        aacEncInfo(handleAAC, &aacinf) != AACENC_OK ||
        playback_aac_frame_samples(48000, aacinf.inputChannels, aacinf.frameLength, true) == 0 ||
        aacEncoder_GetParam(handleAAC,AACENC_PEAK_BITRATE)!=aaceld_bitrate ||
        aacEncoder_GetParam(handleAAC,AACENC_BITRATEMODE)!=(playback_options.rate_mode ? (aaceld_bitrate<192000 ? 4u : 5u) : 0u)) goto fail;
    aaceld_frame_sequence = 1;
    media_tracker.rtp_timestamp = 0;
    return true;
fail:
    aacEncClose(&handleAAC);
    handleAAC = NULL;
    return false;
}

static int fill_aac_audio_buffer(a2dp_media_sending_context_t *context) {
    int          total_samples_read               = 0;

    unsigned int num_audio_samples_per_aac_buffer = acc_num_simples;

    btstack_assert(num_audio_samples_per_aac_buffer <= AUDIO_SLOT_MAX_SAMPLES);

    unsigned required_samples = num_audio_samples_per_aac_buffer * aacinf.inputChannels;

    AACENC_BufDesc in_buf   = { 0 }, out_buf = { 0 };
    AACENC_InArgs  in_args  = { 0 };
    AACENC_OutArgs out_args = { 0 };
    int in_identifier = IN_AUDIO_DATA;
    int in_size, in_elem_size;
    int out_identifier = OUT_BITSTREAM_DATA;
    int out_size, out_elem_size;
    void *in_ptr, *out_ptr;

    in_elem_size             = 2;
    in_buf.numBufs           = 1;
    in_buf.bufs              = &in_ptr;
    in_buf.bufferIdentifiers = &in_identifier;
    in_buf.bufSizes          = &in_size;
    in_buf.bufElSizes        = &in_elem_size;

    out_elem_size             = 1;
    out_buf.numBufs           = 1;
    out_buf.bufs              = &out_ptr;
    out_buf.bufferIdentifiers = &out_identifier;
    out_buf.bufSizes          = &out_size;
    out_buf.bufElSizes        = &out_elem_size;

    // AAC-ELD: Apple per-frame header (BTAudioHALPlugin adds "AAC-ELD Size Header")
    int aaceld_hdr_size = (cur_codec == 4) ? 4 : 0;

    // Reserve a complete AU and its Apple header before consuming source PCM.
    int min_space = (cur_codec == 4) ? (int)((aaceld_bitrate + 799) / 800 + 4) : 50;
    if (handleAAC == NULL) return playback_encoder_failed(context);
    while (context->samples_ready >= num_audio_samples_per_aac_buffer &&
           (context->max_media_payload_size - context->codec_storage_count) >= min_space) {

        audio_pcm_read((uint16_t)num_audio_samples_per_aac_buffer);

        int hdr_offset = context->codec_storage_count + aaceld_hdr_size;

        in_ptr               = encoder_pcm;
        in_size              = required_samples * sizeof(int16_t);
        in_args.numInSamples = required_samples;
        out_ptr              = &context->codec_storage[hdr_offset];
        out_size             = context->max_media_payload_size - hdr_offset;
        out_buf.bufs         = &out_ptr;
        out_buf.bufSizes     = &out_size;
        AACENC_ERROR err;

        uint32_t started=time_us_32();
        err=aacEncEncode(handleAAC, &in_buf, &out_buf, &in_args, &out_args);
        playback_encode_measured(started);
        if (err != AACENC_OK) {
            printf("Error in AAC encoding %d, storage=%d/%d, out_size=%d\n",
                   err, context->codec_storage_count, (int)sizeof(context->codec_storage), out_size);
            return playback_encoder_failed(context);
        }

        total_samples_read += num_audio_samples_per_aac_buffer;
        context->samples_ready -= num_audio_samples_per_aac_buffer;
        if (out_args.numOutBytes <= 0) continue;
        if (cur_codec==4 && playback_options.rate_mode &&
            (unsigned)out_args.numOutBytes>aaceld_bitrate/800u) return playback_encoder_failed(context);
        ++playback_metrics.encoded_units;
        if (cur_codec == 4) {
            if (!playback_eld_append(context->codec_storage, context->max_media_payload_size,
                                     &context->codec_storage_count, &aaceld_frame_sequence,
                                     (size_t)out_args.numOutBytes)) return playback_encoder_failed(context);
        } else {
            context->codec_storage_count += out_args.numOutBytes;
        }
        context->codec_num_frames++;

        // Use 10 ms packets during phone preparation as well as playback.
        if (cur_codec == 2 || context->codec_num_frames >= (phone_opened ? 1 : 3)) break;
    }
    return total_samples_read;
}

#endif

static void a2dp_demo_timer_stop(a2dp_media_sending_context_t * context);

static void avdtp_audio_timeout_handler(btstack_timer_source_t * timer){
    a2dp_media_sending_context_t * context = (a2dp_media_sending_context_t *) btstack_run_loop_get_timer_context(timer);
    if (!context->streaming || !a2dp_is_connected_flag || sc.local_stream_endpoint == NULL) return;
    btstack_run_loop_set_timer(&context->audio_timer, audio_timer_interval);
    btstack_run_loop_add_timer(&context->audio_timer);
    if (playback_idle_phase == PLAYBACK_SUSPENDING) return;
    uint32_t now = btstack_run_loop_get_time_ms();

    uint32_t update_period_ms = audio_timer_interval;
    if (context->time_audio_data_sent > 0){
        update_period_ms = now - context->time_audio_data_sent;
    }

    //printf("update_period_ms is %d\n", update_period_ms);
    //printf("a2dp_sample_rate() is %d\n", a2dp_sample_rate());


    uint32_t num_samples = (update_period_ms * a2dp_sample_rate()) / 1000;
    context->acc_num_missed_samples += (update_period_ms * a2dp_sample_rate()) % 1000;
    
    while (context->acc_num_missed_samples >= 1000){
        num_samples++;
        context->acc_num_missed_samples -= 1000;
    }
    context->time_audio_data_sent = now;
    context->samples_ready += num_samples;

    // Bound stale work in time as well as frames; SBC's 128 samples are shorter than one tick.
    uint32_t max_ready = 4 * acc_num_simples;
    if (max_ready < (uint32_t)a2dp_sample_rate() / 25) max_ready = (uint32_t)a2dp_sample_rate() / 25;
    if (context->samples_ready > max_ready) {
        context->samples_ready = max_ready;
    }

    //printf("num_samples is %d\n", num_samples);
    //printf("samples_ready is %d\n", context->samples_ready);

    if (context->codec_ready_to_send) {
        if ((uint32_t)(now - context->send_wait_start) >= 200) {
            ++playback_metrics.recoveries;
            a2dp_demo_timer_stop(context);
            // Only a confirmed SUSPEND lets us reset both ends of the ELD stream.
            if (avdtp_source_suspend(context->avdtp_cid, context->local_seid) != ERROR_CODE_SUCCESS) {
                aacp_disconnect();
                a2dp_source_disconnect(context->avdtp_cid);
                return;
            }
            recovery_reset_encoder = cur_codec == 4;
            suspend_recovery_pending = true;
            btstack_run_loop_set_timer_handler(&suspend_recovery_timer, suspend_recovery_handler);
            btstack_run_loop_set_timer(&suspend_recovery_timer, SUSPEND_RECOVERY_TIMEOUT_MS);
            btstack_run_loop_add_timer(&suspend_recovery_timer);
        }
        return;
    }

    playback_prepare_packet(context, now);
}

// A successful send can leave another whole packet owed by this timer interval.
static void playback_prepare_packet(a2dp_media_sending_context_t *context, uint32_t now) {
    if (!context->streaming || !a2dp_is_connected_flag || sc.local_stream_endpoint == NULL ||
        playback_idle_phase != PLAYBACK_RUNNING) return;
    if (context->codec_ready_to_send == 2) {
        context->codec_ready_to_send = 1;
        a2dp_source_stream_endpoint_request_can_send_now(context->avdtp_cid, context->local_seid);
        return;
    }
    if (context->codec_ready_to_send) return;
    adtvp_media_codec_capabilities_t local_cap;
    avdtp_media_codec_type_t codec_type = sc.local_stream_endpoint->remote_configuration.media_codec.media_codec_type;

    switch (codec_type){

        case AVDTP_CODEC_SBC:
            fill_sbc_audio_buffer(context);
            if (context->codec_storage_count && (context->codec_num_frames == 15 ||
                (context->codec_storage_count + 1 + btstack_sbc_encoder_sbc_buffer_length()) > context->max_media_payload_size)){
                // schedule sending
                context->codec_ready_to_send = 1;
                context->send_wait_start = now;
                a2dp_source_stream_endpoint_request_can_send_now(context->avdtp_cid, context->local_seid);
            }
            break;
        case AVDTP_CODEC_MPEG_1_2_AUDIO:
            break;
        case AVDTP_CODEC_ATRAC_FAMILY:
            break;
#ifdef HAVE_AAC_FDK
        case AVDTP_CODEC_MPEG_2_4_AAC:
            if (context->codec_storage_count == 0){
                if (fill_aac_audio_buffer(context) < 0) return;
            }

            if (context->codec_storage_count > 0) {
                // schedule sending
                context->codec_ready_to_send = 1;
                context->send_wait_start = now;
                a2dp_source_stream_endpoint_request_can_send_now(context->avdtp_cid, context->local_seid);
            }
            break;
#endif
        case AVDTP_CODEC_NON_A2DP:
            local_cap = sc.local_stream_endpoint->sep.capabilities.media_codec;
            uint32_t local_vendor_id = get_vendor_id(local_cap.media_codec_information);
            uint16_t local_codec_id = get_codec_id(local_cap.media_codec_information);

            // Playback encoding stays with its packet/transport state on core 0.
            if (local_vendor_id == A2DP_CODEC_VENDOR_ID_APPLE && local_codec_id == A2DP_APPLE_CODEC_AAC_ELD) {
                if (fill_aac_audio_buffer(context) < 0) return;

                if (context->codec_num_frames >= (phone_opened ? 1 : 3) ||
                    (context->codec_num_frames > 0 &&
                     (context->max_media_payload_size - context->codec_storage_count) <
                     (int)((aaceld_bitrate + 799) / 800 + 4))) {
                    context->codec_ready_to_send = 1;
                    context->send_wait_start = now;
                    a2dp_source_stream_endpoint_request_can_send_now(context->avdtp_cid, context->local_seid);
                }
            }
            break;
        default:
            break;
    }
}


static bool a2dp_demo_timer_start(a2dp_media_sending_context_t * context){

    int mtu = avdtp_max_media_payload_size(context->avdtp_cid, context->local_seid);
    if (mtu <= 0) {
        aacp_disconnect();
        a2dp_source_disconnect(context->avdtp_cid);
        return false;
    }
    int ceiling = cur_codec == 4 ? PLAYBACK_ELD_PAYLOAD_CEILING : 0x290;
    context->max_media_payload_size = (mtu > 0 && mtu < ceiling) ? mtu : ceiling;
    if (cur_codec == 4 && (handleAAC == NULL || context->max_media_payload_size <
                          (int)((aaceld_bitrate + 799) / 800 + 4))) {
        aacp_disconnect();
        a2dp_source_disconnect(context->avdtp_cid);
        return false;
    }
    critical_section_enter_blocking(&playback_pcm_lock);
    if (playback_idle_phase == PLAYBACK_STARTING) {
        // USB may have supplied the onset while our confirmed idle stream restarts.
        playback_pcm.priming = cur_codec == 4 ? PLAYBACK_PRIMING_FRAMES : 0;
        playback_pcm.primed_on_wire = false;
    } else playback_pcm_reset(&playback_pcm, (uint32_t)pcm_frame_samples * active_slot_count, cur_codec == 4);
    critical_section_exit(&playback_pcm_lock);
    playback_idle_reset();
    context->codec_num_frames = 0;
    context->time_audio_data_sent = 0;
    context->samples_ready = 0;
    context->acc_num_missed_samples = 0;
    context->codec_storage_count = 0;
    context->codec_ready_to_send = 0;
    context->streaming = 1;
    btstack_run_loop_remove_timer(&context->audio_timer);
    btstack_run_loop_set_timer_handler(&context->audio_timer, avdtp_audio_timeout_handler);
    btstack_run_loop_set_timer_context(&context->audio_timer, context);
    btstack_run_loop_set_timer(&context->audio_timer, audio_timer_interval);
    btstack_run_loop_add_timer(&context->audio_timer);
    return true;
}

static void a2dp_demo_timer_stop(a2dp_media_sending_context_t * context){
    is_streaming = false;
    recovery_reset_encoder = false;
    suspend_recovery_pending = false;
    btstack_run_loop_remove_timer(&suspend_recovery_timer);
    context->time_audio_data_sent = 0;
    context->acc_num_missed_samples = 0;
    context->samples_ready = 0;
    context->streaming = 0;
    context->codec_num_frames = 0;
    context->codec_storage_count = 0;
    context->codec_ready_to_send = 0;
    btstack_run_loop_remove_timer(&context->audio_timer);
    playback_idle_reset();
}

static void playback_idle_reset(void) {
    playback_idle_phase = PLAYBACK_RUNNING;
    playback_idle_counting = false;
}

static void playback_idle_poll(uint32_t now) {
    if (!a2dp_is_connected_flag || !finish_scan_avdtp_codec || !media_tracker.local_seid ||
        profile_pending || a2dp_connect_pending) return;
    if (playback_idle_phase == PLAYBACK_SUSPENDING || playback_idle_phase == PLAYBACK_STARTING) {
        if ((uint32_t)(now - playback_idle_transition_since) >= PLAYBACK_TRANSITION_MS)
            playback_encoder_failed(&media_tracker);
        return;
    }
    bool wanted = is_usb_streaming || phone_opened || phone_active || usb_mic_requested() || aacp_mic_active();
    if (playback_idle_phase == PLAYBACK_SUSPENDED) {
        if (!wanted) return;
        playback_idle_phase = PLAYBACK_STARTING;
        playback_idle_transition_since = now;
        if ((cur_codec == 4 && !aaceld_encoder_init()) ||
            avdtp_source_start_stream(media_tracker.avdtp_cid, media_tracker.local_seid) != ERROR_CODE_SUCCESS)
            playback_encoder_failed(&media_tracker);
        return;
    }
    if (!media_tracker.streaming) return; // Do not reclaim an unsolicited peer suspension.
    critical_section_enter_blocking(&playback_pcm_lock);
    bool primed = playback_pcm.primed_on_wire;
    critical_section_exit(&playback_pcm_lock);
    if (wanted || !primed) { playback_idle_counting = false; return; }
    if (!playback_idle_counting) { playback_idle_counting = true; playback_idle_since = now; return; }
    if ((uint32_t)(now - playback_idle_since) < PLAYBACK_IDLE_MS) return;
    playback_idle_phase = PLAYBACK_SUSPENDING;
    playback_idle_transition_since = now;
    if (avdtp_source_suspend(media_tracker.avdtp_cid, media_tracker.local_seid) != ERROR_CODE_SUCCESS)
        playback_encoder_failed(&media_tracker);
}

static void dump_sbc_capability(media_codec_information_sbc_t media_codec_sbc){
    printf("    - sampling_frequency: 0x%02x\n", media_codec_sbc.sampling_frequency_bitmap);
    printf("    - channel_mode: 0x%02x\n", media_codec_sbc.channel_mode_bitmap);
    printf("    - block_length: 0x%02x\n", media_codec_sbc.block_length_bitmap);
    printf("    - subbands: 0x%02x\n", media_codec_sbc.subbands_bitmap);
    printf("    - allocation_method: 0x%02x\n", media_codec_sbc.allocation_method_bitmap);
    printf("    - bitpool_value [%d, %d] \n", media_codec_sbc.min_bitpool_value, media_codec_sbc.max_bitpool_value);
}

static void dump_sbc_configuration(avdtp_media_codec_configuration_sbc_t configuration){
    printf("Received media codec configuration:\n");
    printf("    - num_channels: %d\n", configuration.num_channels);
    printf("    - sampling_frequency: %d\n", configuration.sampling_frequency);
    printf("    - channel_mode: %d\n", configuration.channel_mode);
    printf("    - block_length: %d\n", configuration.block_length);
    printf("    - subbands: %d\n", configuration.subbands);
    printf("    - allocation_method: %d\n", configuration.allocation_method);
    printf("    - bitpool_value [%d, %d] \n", configuration.min_bitpool_value, configuration.max_bitpool_value);
}

static void dump_remote_sink_endpoints(void){
    printf("Remote Endpoints:\n");
    int i;
    for (i=0;i<num_remote_seps;i++) {
        printf("- %u. remote seid %u\n", i, remote_seps[i].seid);
    }
}

static int find_remote_seid(uint8_t remote_seid){
    int i;
    for (i=0;i<num_remote_seps;i++){
        if (remote_seps[i].seid == remote_seid){
            return i;
        }
    }
    return -1;
}



static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (packet_type != HCI_EVENT_PACKET || size < 3 || packet[1] + 2u != size) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVDTP_META) return;
    if (packet[2] != AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED &&
        (size < 5 || little_endian_read_16(packet, 3) != media_tracker.avdtp_cid)) return;
    UNUSED(channel);
    UNUSED(size);

    uint8_t signal_identifier;
    uint8_t status;
    avdtp_sep_t sep;
    uint16_t avdtp_cid;
    uint8_t remote_seid;
    int local_remote_seid_index;
    uint32_t vendor_id;
    uint16_t codec_id;
    bd_addr_t address;


    //printf("Current packet event is 0x%02x\n", packet[2]);

    switch (packet[2]){
        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
            if (size < 14) break;
            avdtp_subevent_signaling_connection_established_get_bd_addr(packet, address);
            // The raw AVDTP event is shared with the phone's independent sink role.
            if (bt_hci_phone_peer_allowed(address)) return;
            // print info
            printf("Current Device found: %s \n",  bd_addr_to_str(address));
            avdtp_cid = avdtp_subevent_signaling_connection_established_get_avdtp_cid(packet);
            status = avdtp_subevent_signaling_connection_established_get_status(packet);
            if (status != 0){
                if (avdtp_cid != media_tracker.avdtp_cid) break;
                a2dp_connect_pending = false;
                printf("AVDTP source signaling connection failed: status %d\n", status);
                break;
            }
            if (!playback_peer_accept((const uint8_t *)get_device_addr(), address,
                                      media_tracker.avdtp_cid, avdtp_cid,
                                      a2dp_accept_incoming,
                                      a2dp_is_connected_flag || a2dp_connect_pending)) {
                a2dp_source_disconnect(avdtp_cid);
                break;
            }
            a2dp_connect_pending = false;
            memcpy(cur_active_device, address, sizeof(bd_addr_t));
            cur_codec=0; actual_bitrate=0; reported_delay=UINT16_MAX; delay_revision=0;
            memset(remote_seps, 0, sizeof(remote_seps));
            cur_num_remote_seps=0;
            remote_catalog_truncated=false;
            finish_scan_avdtp_codec=false;
            media_tracker.avdtp_cid = avdtp_subevent_signaling_connection_established_get_avdtp_cid(packet);
            printf("AVDTP source signaling connection established: avdtp_cid 0x%02x\n", avdtp_cid);

            allow_switch_slot = false;
            set_led_mode_off();
            // seid selected per argv
            num_remote_seps = 0;
            selected_remote_sep_index = 0;
            have_aaceld_codec_capabilities = false;
            finish_setup_aac = false;
            status = avdtp_source_discover_stream_endpoints(media_tracker.avdtp_cid);
            a2dp_is_connected_flag = true;

            // Open the AACP channel (PSM 0x1001) to the same AirPods once
            // A2DP signaling is up. Runs after a short settle delay.
            aacp_connect(cur_active_device);

            break;
        
        case AVDTP_SUBEVENT_STREAMING_CONNECTION_ESTABLISHED:
            status = avdtp_subevent_streaming_connection_established_get_status(packet);
            if (avdtp_subevent_streaming_connection_established_get_avdtp_cid(packet) != media_tracker.avdtp_cid) break;
            if (status != 0){
                printf("Streaming connection failed: status %d\n", status);
                break;
            }
            avdtp_cid = avdtp_subevent_streaming_connection_established_get_avdtp_cid(packet);
            media_tracker.local_seid = avdtp_subevent_streaming_connection_established_get_local_seid(packet);
            media_tracker.remote_seid = avdtp_subevent_streaming_connection_established_get_remote_seid(packet);
            a2dp_is_connected_flag = true;

            printf("Streaming connection established, avdtp_cid 0x%02x\n", avdtp_cid);

            if (sc.local_stream_endpoint != NULL) {
                l2cap_set_flushable_local_cid(sc.local_stream_endpoint->l2cap_media_cid);
            }
            avdtp_source_start_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
            avrcp_connect((uint8_t *) cur_active_device, &media_tracker.avrcp_cid);
            break;

        case AVDTP_SUBEVENT_SIGNALING_SEP_FOUND:
            memset(&sep, 0, sizeof(avdtp_sep_t));
            sep.seid = avdtp_subevent_signaling_sep_found_get_remote_seid(packet);;
            sep.in_use = avdtp_subevent_signaling_sep_found_get_in_use(packet);
            sep.media_type = avdtp_subevent_signaling_sep_found_get_media_type(packet);
            sep.type = avdtp_subevent_signaling_sep_found_get_sep_type(packet);
            printf("Found sep: seid %u, in_use %d, media type %d, sep type %d (1-SNK)\n", sep.seid, sep.in_use, sep.media_type, sep.type);
            if (sep.type == AVDTP_SINK && sep.media_type == AVDTP_AUDIO) {
                if (num_remote_seps < AVDTP_MAX_SEP_NUM) {
                    remote_seps[num_remote_seps].seid = sep.seid;
                    num_remote_seps++;
                } else remote_catalog_truncated=true;
            }
            break;

        case AVDTP_SUBEVENT_SIGNALING_SEP_DICOVERY_DONE:
            if (num_remote_seps == 0) {
                aacp_disconnect();
                a2dp_source_disconnect(media_tracker.avdtp_cid);
                break;
            }
            // select remote if there's only a single remote
            if (num_remote_seps == 1){
                media_tracker.remote_seid = remote_seps[0].seid;
                printf("Only one remote Stream Endpoint with SEID %u, select it for initiator commands\n", media_tracker.remote_seid);
            }


            cur_num_remote_seps = 0;
            // find 1st capabilities; should be sbc
            avdtp_source_get_all_capabilities(media_tracker.avdtp_cid, remote_seps[0].seid);

            break;

        case AVDTP_SUBEVENT_SIGNALING_CAPABILITIES_DONE:
            cur_num_remote_seps ++;
            if (cur_num_remote_seps < num_remote_seps){
                printf("\n\n Getting next CAPABILITY \n\n");
                avdtp_source_get_all_capabilities(media_tracker.avdtp_cid, remote_seps[cur_num_remote_seps].seid);
            }else{
                printf("finish scaning CAPABILITIES codecs\n");
                set_next_capablity_and_start_stream();
            }
            break;


        case AVDTP_SUBEVENT_SIGNALING_MEDIA_TRANSPORT_CAPABILITY:
            printf("CAPABILITY - MEDIA_TRANSPORT supported on remote.\n");
            break;
        case AVDTP_SUBEVENT_SIGNALING_REPORTING_CAPABILITY:
            printf("CAPABILITY - REPORTING supported on remote.\n");
            break;
        case AVDTP_SUBEVENT_SIGNALING_RECOVERY_CAPABILITY:
            printf("CAPABILITY - RECOVERY supported on remote: \n");
            printf("    - recovery_type                %d\n", avdtp_subevent_signaling_recovery_capability_get_recovery_type(packet));
            printf("    - maximum_recovery_window_size %d\n", avdtp_subevent_signaling_recovery_capability_get_maximum_recovery_window_size(packet));
            printf("    - maximum_number_media_packets %d\n", avdtp_subevent_signaling_recovery_capability_get_maximum_number_media_packets(packet));
            break;
        case AVDTP_SUBEVENT_SIGNALING_CONTENT_PROTECTION_CAPABILITY:
            if (size < 10 || avdtp_subevent_signaling_content_protection_capability_get_cp_type_value_len(packet) > size - 10u) break;
            printf("CAPABILITY - CONTENT_PROTECTION supported on remote: \n");
            printf("    - cp_type           %d\n", avdtp_subevent_signaling_content_protection_capability_get_cp_type(packet));
            printf("    - cp_type_value_len %d\n", avdtp_subevent_signaling_content_protection_capability_get_cp_type_value_len(packet));
            break;
        case AVDTP_SUBEVENT_SIGNALING_MULTIPLEXING_CAPABILITY:
            printf("CAPABILITY - MULTIPLEXING supported on remote: \n");
            printf("    - fragmentation                  %d\n", avdtp_subevent_signaling_multiplexing_capability_get_fragmentation(packet));
            printf("    - transport_identifiers_num      %d\n", avdtp_subevent_signaling_multiplexing_capability_get_transport_identifiers_num(packet));
            printf("    - transport_session_identifier_1 %d\n", avdtp_subevent_signaling_multiplexing_capability_get_transport_session_identifier_1(packet));
            printf("    - transport_session_identifier_2 %d\n", avdtp_subevent_signaling_multiplexing_capability_get_transport_session_identifier_2(packet));
            printf("    - transport_session_identifier_3 %d\n", avdtp_subevent_signaling_multiplexing_capability_get_transport_session_identifier_3(packet));
            printf("    - tcid_1                         %d\n", avdtp_subevent_signaling_multiplexing_capability_get_tcid_1(packet));
            printf("    - tcid_2                         %d\n", avdtp_subevent_signaling_multiplexing_capability_get_tcid_2(packet));
            printf("    - tcid_3                         %d\n", avdtp_subevent_signaling_multiplexing_capability_get_tcid_3(packet));
            break;
        case AVDTP_SUBEVENT_SIGNALING_DELAY_REPORTING_CAPABILITY:
            if (size<6) break;
            local_remote_seid_index=find_remote_seid(avdtp_subevent_signaling_delay_reporting_capability_get_remote_seid(packet));
            if (local_remote_seid_index>=0) remote_seps[local_remote_seid_index].delay_reporting=true;
            printf("CAPABILITY - DELAY_REPORTING supported on remote.\n");
            break;
        case AVDTP_SUBEVENT_SIGNALING_DELAY_REPORT:
            if (size!=8 || !media_tracker.local_seid ||
                avdtp_subevent_signaling_delay_report_get_local_seid(packet)!=media_tracker.local_seid) break;
            playback_game_observe_send(&playback_game,btstack_run_loop_get_time_ms(),
                aacp_receive_buffer_serial(),aacp_receive_buffer_last(),delay_revision);
            reported_delay=avdtp_subevent_signaling_delay_report_get_delay_100us(packet);
            if (++delay_revision==0) ++delay_revision;
            printf("DELAY_REPORT received: %d.%0d ms, local seid %d\n", 
                avdtp_subevent_signaling_delay_report_get_delay_100us(packet)/10, avdtp_subevent_signaling_delay_report_get_delay_100us(packet)%10,
                avdtp_subevent_signaling_delay_report_get_local_seid(packet));
            break;
        case AVDTP_SUBEVENT_SIGNALING_HEADER_COMPRESSION_CAPABILITY:
            printf("CAPABILITY - HEADER_COMPRESSION supported on remote: \n");
            printf("    - back_ch   %d\n", avdtp_subevent_signaling_header_compression_capability_get_back_ch(packet));
            printf("    - media     %d\n", avdtp_subevent_signaling_header_compression_capability_get_media(packet));
            printf("    - recovery  %d\n", avdtp_subevent_signaling_header_compression_capability_get_recovery(packet));
            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY:{
            // cache
            remote_seid = avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet);
            local_remote_seid_index = find_remote_seid(remote_seid);
            if (local_remote_seid_index < 0 || size > AVDTP_MAX_MEDIA_CODEC_CAPABILITES_EVENT_LEN) break;
            (void) memcpy(remote_seps[local_remote_seid_index].media_codec_event, packet, size);
            remote_seps[local_remote_seid_index].codec_type = AVDTP_CODEC_SBC;
            remote_seps[local_remote_seid_index].have_media_codec_capabilities = true;

            printf("CAPABILITY - MEDIA_CODEC: SBC, remote seid %u: \n", remote_seid);

            media_codec_information_sbc_t   sbc_capability;
            sbc_capability.sampling_frequency_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_sampling_frequency_bitmap(packet);
            sbc_capability.channel_mode_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_channel_mode_bitmap(packet);
            sbc_capability.block_length_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_block_length_bitmap(packet);
            sbc_capability.subbands_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_subbands_bitmap(packet);
            sbc_capability.allocation_method_bitmap = avdtp_subevent_signaling_media_codec_sbc_capability_get_allocation_method_bitmap(packet);
            sbc_capability.min_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_capability_get_min_bitpool_value(packet);
            sbc_capability.max_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_capability_get_max_bitpool_value(packet);
            dump_sbc_capability(sbc_capability);

            break;
        }

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AUDIO_CAPABILITY:
            // cache
            remote_seid = avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet);
            local_remote_seid_index = find_remote_seid(remote_seid);
            if (local_remote_seid_index < 0 || size > AVDTP_MAX_MEDIA_CODEC_CAPABILITES_EVENT_LEN) break;
            (void) memcpy(remote_seps[local_remote_seid_index].media_codec_event, packet, size);
            remote_seps[local_remote_seid_index].codec_type = AVDTP_CODEC_MPEG_1_2_AUDIO;
            remote_seps[local_remote_seid_index].have_media_codec_capabilities = true;
            printf("CAPABILITY - MEDIA_CODEC: MPEG AUDIO, remote seid %u: \n", remote_seid);

            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CAPABILITY: {
            // cache
            remote_seid = avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet);
            local_remote_seid_index = find_remote_seid(remote_seid);
            if (local_remote_seid_index < 0 || size > AVDTP_MAX_MEDIA_CODEC_CAPABILITES_EVENT_LEN) break;
            (void) memcpy(remote_seps[local_remote_seid_index].media_codec_event, packet, size);
            remote_seps[local_remote_seid_index].codec_type = AVDTP_CODEC_MPEG_2_4_AAC;
            remote_seps[local_remote_seid_index].have_media_codec_capabilities = true;
            printf("CAPABILITY - MEDIA_CODEC: MPEG AAC, remote seid %u: \n", remote_seid);

            avdtp_media_codec_capabilities_aac_t aac_capabilities;
            aac_capabilities.sampling_frequency_bitmap = a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_sampling_frequency_bitmap(packet);
            aac_capabilities.object_type_bitmap = a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_object_type_bitmap(packet);
            aac_capabilities.channels_bitmap = a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_channels_bitmap(packet);
            aac_capabilities.bit_rate = a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_bit_rate(packet);
            aac_capabilities.vbr = a2dp_subevent_signaling_media_codec_mpeg_aac_capability_get_vbr(packet);
            printf("A2DP Source: Received AAC capabilities! Sampling frequency bitmap: 0x%04x, object type %u, channel mode %u, bitrate %u, vbr: %u\n",
                   aac_capabilities.sampling_frequency_bitmap, aac_capabilities.object_type_bitmap, aac_capabilities.channels_bitmap,
                   aac_capabilities.bit_rate, aac_capabilities.vbr);
            break;
        }

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_ATRAC_CAPABILITY:
            // cache
            remote_seid = avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet);
            local_remote_seid_index = find_remote_seid(remote_seid);
            if (local_remote_seid_index < 0 || size > AVDTP_MAX_MEDIA_CODEC_CAPABILITES_EVENT_LEN) break;
            (void) memcpy(remote_seps[local_remote_seid_index].media_codec_event, packet, size);
            remote_seps[local_remote_seid_index].codec_type = AVDTP_CODEC_ATRAC_FAMILY;
            remote_seps[local_remote_seid_index].have_media_codec_capabilities = true;
            printf("CAPABILITY - MEDIA_CODEC: ATRAC, remote seid %u: \n", remote_seid);

            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY:
            if (size < 17) break;
            // cache
            remote_seid = avdtp_subevent_signaling_media_codec_sbc_capability_get_remote_seid(packet);
            local_remote_seid_index = find_remote_seid(remote_seid);
            if (local_remote_seid_index < 0 || size > AVDTP_MAX_MEDIA_CODEC_CAPABILITES_EVENT_LEN) break;
            (void) memcpy(remote_seps[local_remote_seid_index].media_codec_event, packet, size);
            remote_seps[local_remote_seid_index].codec_type  = AVDTP_CODEC_NON_A2DP;
            remote_seps[local_remote_seid_index].have_media_codec_capabilities = true;
            const uint8_t *media_info = avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information(packet);
            uint16_t info_len = avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(packet);
            if (info_len < 6 || media_info + info_len > packet + size) break;
            vendor_id = get_vendor_id(media_info);
            codec_id = get_codec_id(media_info);

            remote_seps[local_remote_seid_index].vendor_id = vendor_id;
            remote_seps[local_remote_seid_index].codec_id = codec_id;

            if (vendor_id == A2DP_CODEC_VENDOR_ID_APPLE && codec_id == A2DP_APPLE_CODEC_AAC_ELD)
            {
                have_aaceld_codec_capabilities = playback_eld_capability(media_info, info_len);
                printf("CAPABILITY - Apple AAC-ELD, remote seid %u\n", remote_seid);
                printf("  raw bytes (%u): ", info_len);
                for (int k = 0; k < info_len; k++) {
                    printf("%02x ", media_info[k]);
                }
                printf("\n");
            }
            else {
                printf("CAPABILITY - MEDIA_CODEC: OTHER, remote seid %u: \n", remote_seid);
                printf("vendor_id 0x%04x: \n", vendor_id);
                printf("codec_id: 0x%04x: \n", codec_id);
            }

            break;

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:{
            if (size < 18) break;
            if (playback_options.codec && playback_options.codec!=1) { playback_encoder_failed(&media_tracker); break; }
            printf("Set configuration and init encoder\n");
            avdtp_media_codec_configuration_sbc_t sbc_configuration;
            sbc_configuration.reconfigure = avdtp_subevent_signaling_media_codec_sbc_configuration_get_reconfigure(packet);
            sbc_configuration.num_channels = avdtp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
            sbc_configuration.sampling_frequency = avdtp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            sbc_configuration.block_length = avdtp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
            sbc_configuration.subbands = avdtp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
            sbc_configuration.min_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
            sbc_configuration.max_bitpool_value = avdtp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
            
            avdtp_channel_mode_t channel_mode = (avdtp_channel_mode_t) avdtp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet);
            avdtp_sbc_allocation_method_t allocation_method = (avdtp_sbc_allocation_method_t) avdtp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
            
            // Map Bluetooth spec definition to SBC Encoder expected input
            sbc_configuration.allocation_method = (btstack_sbc_allocation_method_t)(((uint8_t) allocation_method) - 1);
            switch (channel_mode){
                case AVDTP_CHANNEL_MODE_JOINT_STEREO:
                    sbc_configuration.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
                    sbc_configuration.num_channels = 2;
                    break;
                case AVDTP_CHANNEL_MODE_STEREO:
                    sbc_configuration.channel_mode = SBC_CHANNEL_MODE_STEREO;
                    sbc_configuration.num_channels = 2;
                    break;
                case AVDTP_CHANNEL_MODE_DUAL_CHANNEL:
                    sbc_configuration.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
                    sbc_configuration.num_channels = 2;
                    break;
                case AVDTP_CHANNEL_MODE_MONO:
                    sbc_configuration.channel_mode = SBC_CHANNEL_MODE_MONO;
                    sbc_configuration.num_channels = 1;
                    break;
                default:
                    sbc_configuration.num_channels = 0;
                    break;
            }
            if (!playback_format_supported(sbc_configuration.sampling_frequency, sbc_configuration.num_channels)) {
                playback_encoder_failed(&media_tracker);
                break;
            }
            a2dp_demo_timer_stop(&media_tracker);
            if (handleAAC != NULL) aacEncClose(&handleAAC);
            dump_sbc_configuration(sbc_configuration);
            current_sample_rate = 48000;
            btstack_sbc_encoder_init(&sbc_encoder_state, SBC_MODE_STANDARD, 
                sbc_configuration.block_length, sbc_configuration.subbands, 
                sbc_configuration.allocation_method, sbc_configuration.sampling_frequency, 
                sbc_configuration.max_bitpool_value,
                sbc_configuration.channel_mode);

            audio_timer_interval = 10;

            audio_slot_queue_configure_with_count(btstack_sbc_encoder_num_audio_frames(), AUDIO_SLOT_COUNT_SBC);

            acc_num_simples = btstack_sbc_encoder_num_audio_frames();
            cur_codec = 1;
            actual_bitrate=0; profile_pending=false;

            avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);
            break;
        }

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CONFIGURATION: {
            if (size < 18) break;
            if (!finish_setup_aac) break;
            if (playback_options.codec && playback_options.codec!=2) { playback_encoder_failed(&media_tracker); break; }
            uint32_t rate = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_sampling_frequency(packet);
            uint8_t channels = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_num_channels(packet);
            uint8_t object = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_object_type(packet);
            uint32_t bitrate = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_bit_rate(packet);
            uint8_t vbr = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_vbr(packet);
            if (!playback_format_supported(rate, channels) ||
                (object != AVDTP_AAC_MPEG2_LC && object != AVDTP_AAC_MPEG4_LC) ||
                vbr || bitrate < 96000 || bitrate > (uint32_t)max_aac_bit_rate ||
                (playback_options.bitrate && playback_options.bitrate!=bitrate)) {
                playback_encoder_failed(&media_tracker);
                break;
            }
            if (handleAAC != NULL) aacEncClose(&handleAAC);
            handleAAC = NULL;
            bool ready = aacEncOpen(&handleAAC, 0x01, channels) == AACENC_OK;
            const struct { AACENC_PARAM key; UINT value; } params[] = {
                {AACENC_AOT, AOT_AAC_LC}, {AACENC_BITRATE, bitrate},
                {AACENC_SAMPLERATE, rate}, {AACENC_CHANNELMODE, MODE_2},
                {AACENC_BITRATEMODE, 0}, {AACENC_AFTERBURNER, 0},
                {AACENC_TRANSMUX, TT_MP4_LATM_MCP1},
                {AACENC_HEADER_PERIOD, 1}, {AACENC_AUDIOMUXVER, 1}
            };
            for (size_t i = 0; ready && i < sizeof(params) / sizeof(params[0]); ++i) {
                ready = aacEncoder_SetParam(handleAAC, params[i].key, params[i].value) == AACENC_OK;
            }
            if (ready) ready = aacEncEncode(handleAAC, NULL, NULL, NULL, NULL) == AACENC_OK &&
                               aacEncInfo(handleAAC, &aacinf) == AACENC_OK;
            uint16_t frame_samples = ready ? playback_aac_frame_samples(rate, aacinf.inputChannels,
                                                                       aacinf.frameLength, false) : 0;
            if (frame_samples == 0) {
                if (handleAAC != NULL) aacEncClose(&handleAAC);
                handleAAC = NULL;
                playback_encoder_failed(&media_tracker);
                break;
            }
            acc_num_simples = frame_samples;
            current_sample_rate = rate;
            audio_timer_interval = aac_audio_timer_interval;
            audio_slot_queue_configure_with_count(acc_num_simples, AUDIO_SLOT_COUNT_AAC);
            cur_codec = 2;
            actual_bitrate=bitrate; profile_pending=false;
            avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);
            break;
        }

        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AUDIO_CONFIGURATION:
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_ATRAC_CONFIGURATION:
            // TODO: handle other configuration event
            printf("Config not handled for %s\n", codec_name_for_type(remote_seps[selected_remote_sep_index].codec_type));
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
            if (size < 19) break;
            printf("Received other configuration\n");
            uint8_t *codec_info = (uint8_t *) a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information(packet);
            uint16_t config_len = a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information_len(packet);
            if (config_len < 6 || codec_info + config_len > packet + size) break;
            vendor_id = get_vendor_id(codec_info);
            codec_id = get_codec_id(codec_info);

            // Apple AAC-ELD
            if (vendor_id == A2DP_CODEC_VENDOR_ID_APPLE && codec_id == A2DP_APPLE_CODEC_AAC_ELD) {
                printf("A2DP Source: Received Apple AAC-ELD configuration!\n");
                printf("  raw config bytes: ");
                uint16_t config_info_len = a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information_len(packet);
                for (int k = 0; k < config_info_len; k++) {
                    printf("%02x ", codec_info[k]);
                }
                printf("\n");

                if (!playback_options_eld_configuration(codec_info, config_info_len,playback_options)) {
                    printf("Unsupported AAC-ELD configuration\n");
                    aacp_disconnect();
                    a2dp_source_disconnect(media_tracker.avdtp_cid);
                    break;
                }
                aaceld_bitrate = playback_eld_bitrate(codec_info);
                if (!aaceld_encoder_init()) {
                    printf("AAC-ELD encoder initialization failed\n");
                    aacp_disconnect();
                    a2dp_source_disconnect(media_tracker.avdtp_cid);
                    break;
                }

                current_sample_rate = 48000;
                acc_num_simples = aacinf.frameLength;  // use actual frame size from encoder
                audio_timer_interval = 10;

                printf("AAC-ELD acc_num_simples=%d\n", acc_num_simples);

                audio_slot_queue_configure_with_count(acc_num_simples, AUDIO_SLOT_COUNT_ELD);

                aaceld_frame_sequence = 1;

                cur_codec = 4;  // new codec ID for AAC-ELD
                actual_bitrate=aaceld_bitrate; profile_pending=false;

                avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);
            }
            else
            {
                printf("Config not handled for %s\n", codec_name_for_type(remote_seps[selected_remote_sep_index].codec_type));
            }
            break;

        case AVDTP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
            if (avdtp_subevent_streaming_can_send_media_packet_now_get_avdtp_cid(packet) != media_tracker.avdtp_cid ||
                avdtp_subevent_streaming_can_send_media_packet_now_get_local_seid(packet) != media_tracker.local_seid) break;
            a2dp_demo_send_media_packet();
            break;  


        case AVDTP_SUBEVENT_SIGNALING_ACCEPT:
            if (size < 8) break;
            if (avdtp_subevent_signaling_accept_get_avdtp_cid(packet) != media_tracker.avdtp_cid) break;
            signal_identifier = avdtp_subevent_signaling_accept_get_signal_identifier(packet);
            if (is_cmd_triggered_localy){
                is_cmd_triggered_localy = 0;
                printf("AVDTP Source command accepted\n");
            }
            
            switch (signal_identifier){
                case AVDTP_SI_ACCEPT_START:
                    if (avdtp_subevent_signaling_accept_get_local_seid(packet) == media_tracker.local_seid)
                        avdtp_start_stream_accept(media_tracker.avdtp_cid, media_tracker.local_seid);
                    break;
                case AVDTP_SI_OPEN:
                    break;
                case AVDTP_SI_SET_CONFIGURATION:
                    break;
                case  AVDTP_SI_START:
                    if (avdtp_subevent_signaling_accept_get_local_seid(packet) != media_tracker.local_seid) break;
                    if (profile_pending || a2dp_connect_pending) break;
                    printf("Stream started.\n");
                    if (finish_scan_avdtp_codec){
                        is_streaming = a2dp_demo_timer_start(&media_tracker);
                        if (is_streaming) start_led_blink();
                    }
                    break;
                case AVDTP_SI_SUSPEND: {
                    if (avdtp_subevent_signaling_accept_get_local_seid(packet) != media_tracker.local_seid) break;
                    bool idle = playback_idle_phase == PLAYBACK_SUSPENDING;
                    bool recover = suspend_recovery_pending;
                    bool reset = recovery_reset_encoder;
                    a2dp_demo_timer_stop(&media_tracker);
                    if (idle) playback_idle_phase = PLAYBACK_SUSPENDED;
                    else if (recover) {
                        if (reset && !aaceld_encoder_init()) {
                            aacp_disconnect();
                            a2dp_source_disconnect(media_tracker.avdtp_cid);
                            break;
                        }
                        if (avdtp_source_start_stream(media_tracker.avdtp_cid, media_tracker.local_seid) != ERROR_CODE_SUCCESS) {
                            aacp_disconnect();
                            a2dp_source_disconnect(media_tracker.avdtp_cid);
                        }
                    }
                    break;
                }
                case AVDTP_SI_ABORT:
                case AVDTP_SI_CLOSE:
                    printf("Stream released.\n");
                    a2dp_demo_timer_stop(&media_tracker);
                    break;
                default:
                    break;
            }
            break;

        case AVDTP_SUBEVENT_SIGNALING_REJECT:
        case AVDTP_SUBEVENT_SIGNALING_GENERAL_REJECT:
            if (avdtp_subevent_signaling_reject_get_avdtp_cid(packet) != media_tracker.avdtp_cid) break;
            signal_identifier = avdtp_subevent_signaling_reject_get_signal_identifier(packet);
            if (signal_identifier == AVDTP_SI_SUSPEND || signal_identifier == AVDTP_SI_START) {
                a2dp_demo_timer_stop(&media_tracker);
                aacp_disconnect();
                a2dp_source_disconnect(media_tracker.avdtp_cid);
            }
            break;
        case AVDTP_SUBEVENT_STREAMING_CONNECTION_RELEASED:
            if (avdtp_subevent_streaming_connection_released_get_avdtp_cid(packet) != media_tracker.avdtp_cid) break;
            l2cap_set_flushable_local_cid(0);
            a2dp_demo_timer_stop(&media_tracker);
            printf("Streaming connection released.\n");
            set_led_mode_off();
            is_streaming = false;
            break;
        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            if (avdtp_subevent_signaling_connection_released_get_avdtp_cid(packet) != media_tracker.avdtp_cid) break;
            a2dp_demo_timer_stop(&media_tracker);
            aacp_disconnect();
            l2cap_set_flushable_local_cid(0);
            audio_slot_queue_init();
            if (handleAAC != NULL) aacEncClose(&handleAAC);
            a2dp_connect_pending = false;
            media_tracker.avdtp_cid = 0;
            media_tracker.local_seid = 0;
            allow_switch_slot = true;
            finish_scan_avdtp_codec = false;
            a2dp_is_connected_flag = false;
            cur_codec=0; actual_bitrate=0; reported_delay=UINT16_MAX; delay_revision=0;
            cur_capability = 0;
            set_led_mode_off();
            have_aaceld_codec_capabilities = false;
            finish_setup_aac = false;
            printf("Signaling connection released.\n");
            break;
        default:
            break;
    }
}

void increase_vol_by_key(){
    if (media_tracker.volume > 117){
         media_tracker.volume = 127;
    } else {
                media_tracker.volume += 10;
    }
    printf(" - volume up (via set absolute volume) %d%% (%d)\n",  media_tracker.volume * 100 / 127,  media_tracker.volume);
    avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
}


void decrease_vol_by_key(){

    if (media_tracker.volume < 10){
        media_tracker.volume = 0;
    } else {
        media_tracker.volume -= 10;
    }
    printf(" - volume down (via set absolute volume) %d%% (%d)\n",  media_tracker.volume * 100 / 127,  media_tracker.volume);
    avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
    
}

#ifdef HAVE_BTSTACK_STDIN

static void show_usage(void){
    bd_addr_t      iut_address;
    gap_local_bd_addr(iut_address);
    printf("\n--- Bluetooth AVDTP SOURCE Test Console %s ---\n", bd_addr_to_str(iut_address));
    printf("c      - create connection to addr %s\n", get_device_addr_string());
    printf("C      - disconnect\n");
    printf("D      - Delete linked list\n");
    printf("z      - select remote endpoint\n");
    printf("g      - set_next_capablity_and_start_stream  %u\n", media_tracker.remote_seid);
    printf("a      - get all capabilities for remote seid %u\n", media_tracker.remote_seid);
    printf("s      - set configuration    for remote seid %u\n", media_tracker.remote_seid);
    printf("f      - get configuration    for remote seid %u\n", media_tracker.remote_seid);
    printf("R      - reconfigure stream   for remote seid %u\n", media_tracker.remote_seid);
    printf("o      - open stream          for remote seid %u\n", media_tracker.remote_seid);
    printf("m      - start stream         for remote seid %u\n", media_tracker.remote_seid);
    printf("A      - abort stream         for remote seid %u\n", media_tracker.remote_seid);
    printf("S      - stop stream          for remote seid %u\n", media_tracker.remote_seid);
    printf("P      - suspend stream       for remote seid %u\n", media_tracker.remote_seid);
    printf("l      - set up ladc          for remote seid %u\n", media_tracker.remote_seid);
    printf("u      - set up sbc           for remote seid %u\n", media_tracker.remote_seid);
    printf("Ctrl-c - exit\n");
    printf("---\n");
}


void set_bt_volume(int16_t val){
    if (val < -50) val = -50;
    if (val > 0) val = 0;
    media_tracker.volume = (val * 2 + 100) * 127 / 100;
    avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
    //printf("(via set usb volume) %d%% (%d)\n",  media_tracker.volume * 100 / 127,  media_tracker.volume);
}

static void stdin_process(char cmd){
    uint8_t status = ERROR_CODE_SUCCESS;
    is_cmd_triggered_localy = 1;
    static bool enter_remote_seid_index = false;

    if (enter_remote_seid_index){
        if ((cmd < '0') || (cmd > '9')) return;
        uint8_t index = cmd - '0';
        if (index >= num_remote_seps){
            printf("Index too high, try again\n");
            return;
        }
        enter_remote_seid_index = false;
        selected_remote_sep_index = index;
        media_tracker.remote_seid = remote_seps[selected_remote_sep_index].seid;
        printf("Selected Remote Stream Endpoint with SEID %u\n",  media_tracker.remote_seid);
        return;
    }

    switch (cmd){
        case 'c':
            printf("Establish AVDTP Source connection to %s\n", get_device_addr_string());
            status = avdtp_source_connect((uint8_t *) get_device_addr(), &media_tracker.avdtp_cid);
            break;
        case 'C':
            printf("Disconnect AVDTP Source\n");
            status = avdtp_source_disconnect(media_tracker.avdtp_cid);
            break;
        case 'D':
            printf("Deleting all link keys\n");
            gap_delete_all_link_keys();
            printf("Finished\n");
            break;
        case 'g':
            printf("set_next_capablity_and_start_stream with seid %d\n", media_tracker.remote_seid);
            set_next_capablity_and_start_stream();
            break;
        case 'a':
            printf("Get all capabilities of stream endpoint with seid %d\n", media_tracker.remote_seid);
            status = avdtp_source_get_all_capabilities(media_tracker.avdtp_cid, media_tracker.remote_seid);
            break;
        case 'f':
            printf("Get configuration of stream endpoint with seid %d\n", media_tracker.remote_seid);
            status = avdtp_source_get_configuration(media_tracker.avdtp_cid, media_tracker.remote_seid);
            break;
        case 'z':
            if (num_remote_seps == 0){
                printf("Remote Stream Endpoints not discovered yet, please discover stream endpoints first\n");
                break;
            }
            dump_remote_sink_endpoints();
            printf("Please enter index of remote stream endpoint:\n");
            enter_remote_seid_index = true;
            break;

        case 's':{
            //status = set_configuration();
            break;
        }
        case 'R':{
            if (num_remote_seps == 0){
                printf("Remote Stream Endpoints not discovered yet, please discover stream endpoints first\n");
                break;
            }
            if (remote_seps[selected_remote_sep_index].have_media_codec_capabilities == false){
                printf("Remote Stream Endpoints Media Codec Capabilities not received yet, please get (all) capabilities for stream endpoint with seid %u first\n",  media_tracker.remote_seid);
                break;
            }
            printf("Reconfigure stream endpoint with seid %d\n", media_tracker.remote_seid);
            avdtp_media_codec_type_t media_codec_type = remote_seps[selected_remote_sep_index].codec_type;
            avdtp_capabilities_t new_configuration;
            new_configuration.media_codec.media_type = AVDTP_AUDIO;

            const uint16_t new_sampling_frequency = 48000;

            switch (media_codec_type){
                case AVDTP_CODEC_SBC:
                    avdtp_config_sbc_set_sampling_frequency(media_codec_config_data, new_sampling_frequency);
                    break;
                case AVDTP_CODEC_MPEG_1_2_AUDIO:
                    avdtp_config_mpeg_audio_set_sampling_frequency(media_codec_config_data, new_sampling_frequency);
                    break;
                case AVDTP_CODEC_MPEG_2_4_AAC:
                    avdtp_config_mpeg_aac_set_sampling_frequency(media_codec_config_data, new_sampling_frequency);
                    break;
                case AVDTP_CODEC_ATRAC_FAMILY:
                    avdtp_config_atrac_set_sampling_frequency(media_codec_config_data, new_sampling_frequency);
                    break;
                default:
                    printf("Reconfigure not implemented for %s\n", codec_name_for_type(media_codec_type));
                    return;
            }
            new_configuration.media_codec.media_type = AVDTP_AUDIO;
            new_configuration.media_codec.media_codec_type = media_codec_type;
            new_configuration.media_codec.media_codec_information_len = media_codec_config_len;
            new_configuration.media_codec.media_codec_information = media_codec_config_data;
            status = avdtp_source_reconfigure(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid, 1 << AVDTP_MEDIA_CODEC, new_configuration);
            break;
        }
        case 'o':
            printf("Establish stream between local %d and remote %d seid\n", media_tracker.local_seid, media_tracker.remote_seid);
            status = avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);
            break;
        case 'm':
            printf("Start stream between local %d and remote %d seid, \n", media_tracker.local_seid, media_tracker.remote_seid);
            status = avdtp_source_start_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
            break;
        case 'A':
            printf("Abort stream between local %d and remote %d seid\n", media_tracker.local_seid, media_tracker.remote_seid);
            status = avdtp_source_abort_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
            break;
        case 'S':
            printf("Release stream between local %d and remote %d seid\n", media_tracker.local_seid, media_tracker.remote_seid);
            status = avdtp_source_stop_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
            break;
        case 'P':
            printf("Susspend stream between local %d and remote %d seid\n", media_tracker.local_seid, media_tracker.remote_seid);
            status = avdtp_source_suspend(media_tracker.avdtp_cid, media_tracker.local_seid);
            break;
        case 'X':
            printf("Stop streaming\n");
            status = (media_tracker.avdtp_cid, media_tracker.local_seid);
            break;


        case 'v':
            if (media_tracker.volume > 117){
                media_tracker.volume = 127;
            } else {
                media_tracker.volume += 10;
            }
            printf(" - volume up (via set absolute volume) %d%% (%d)\n",  media_tracker.volume * 100 / 127,  media_tracker.volume);
            status = avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
            break;
        case 'V':
            if (media_tracker.volume < 10){
                media_tracker.volume = 0;
            } else {
                media_tracker.volume -= 10;
            }
            printf(" - volume down (via set absolute volume) %d%% (%d)\n",  media_tracker.volume * 100 / 127,  media_tracker.volume);
            status = avrcp_controller_set_absolute_volume(media_tracker.avrcp_cid, media_tracker.volume);
            break;

        case 'p':
            a2dp_demo_send_media_packet();
            break;

        case 'u':
            printf("Setup SBC codec\n");
            status = setup_sbc_configuration();
            break;

        case '\n':
        case '\r':
            break;
        default:
            show_usage();
            break;

    }
    if (status != ERROR_CODE_SUCCESS){
        printf("AVDTP Sink cmd \'%c\' failed, status 0x%02x\n", cmd, status);
    }
}
#endif


static int setup_sbc_configuration(){

    int sbc_num = -1;
    for (int i = 0; i < num_remote_seps; i++){
        if (remote_seps[i].have_media_codec_capabilities &&
            remote_seps[i].codec_type == AVDTP_CODEC_SBC){
            printf("found sbc!!! Remote Stream Endpoints ID is %d\n", i);
            selected_remote_sep_index = i;
            sbc_num = i;
            break;
        }
    }

    if (sbc_num < 0) return -1;

    // SBC endpoint persists across connections.
    if (stream_endpoint_sbc == NULL) {
        stream_endpoint_sbc = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC, (uint8_t *) media_sbc_codec_capabilities, sizeof(media_sbc_codec_capabilities), (uint8_t*) local_stream_endpoint_sbc_media_codec_configuration, sizeof(local_stream_endpoint_sbc_media_codec_configuration));
        btstack_assert(stream_endpoint_sbc != NULL);
        stream_endpoint_sbc->media_codec_configuration_info = local_stream_endpoint_sbc_media_codec_configuration;
        stream_endpoint_sbc->media_codec_configuration_len  = sizeof(local_stream_endpoint_sbc_media_codec_configuration);
    }
    avdtp_source_register_delay_reporting_category(avdtp_local_seid(stream_endpoint_sbc));
    avdtp_set_preferred_sampling_frequency(stream_endpoint_sbc, 48000);
    avdtp_set_preferred_channel_mode(stream_endpoint_sbc, AVDTP_SBC_STEREO);

    // set up local stream_endpoint; need change
    sc.local_stream_endpoint = stream_endpoint_sbc;

    // store local seid
    media_tracker.local_seid  = avdtp_local_seid(sc.local_stream_endpoint);
    media_tracker.remote_seid = remote_seps[sbc_num].seid;


    // choose SBC config params
    const uint8_t * packet = remote_seps[sbc_num].media_codec_event;

    // choose SBC config params
    avdtp_configuration_sbc_t configuration;
    configuration.sampling_frequency = avdtp_choose_sbc_sampling_frequency(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_sampling_frequency_bitmap(packet));
    configuration.channel_mode       = avdtp_choose_sbc_channel_mode(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_channel_mode_bitmap(packet));
    configuration.block_length       = avdtp_choose_sbc_block_length(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_block_length_bitmap(packet));
    configuration.subbands           = avdtp_choose_sbc_subbands(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_subbands_bitmap(packet));
    configuration.allocation_method  = avdtp_choose_sbc_allocation_method(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_allocation_method_bitmap(packet));
    configuration.max_bitpool_value  = avdtp_choose_sbc_max_bitpool_value(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_max_bitpool_value(packet));
    configuration.min_bitpool_value  = avdtp_choose_sbc_min_bitpool_value(sc.local_stream_endpoint, avdtp_subevent_signaling_media_codec_sbc_capability_get_min_bitpool_value(packet));

    if (configuration.sampling_frequency != 48000 ||
        configuration.channel_mode == AVDTP_CHANNEL_MODE_MONO) return -1;
    avdtp_config_sbc_store(media_codec_config_data, &configuration);
    media_codec_config_len = 4;

    avdtp_capabilities_t new_configuration;
    new_configuration.media_codec.media_type = AVDTP_AUDIO;
    new_configuration.media_codec.media_codec_type = remote_seps[selected_remote_sep_index].codec_type ;
    new_configuration.media_codec.media_codec_information_len = media_codec_config_len;
    new_configuration.media_codec.media_codec_information = media_codec_config_data;
    int status = avdtp_source_set_configuration(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid, 1 << AVDTP_MEDIA_CODEC, new_configuration);
    return status;
}


static int setup_aac_configuration(){

    #ifdef HAVE_AAC_FDK
    // - AAC
    if (stream_endpoint_aac == NULL) {
        stream_endpoint_aac = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_MPEG_2_4_AAC, (uint8_t *) media_aac_codec_capabilities, sizeof(media_aac_codec_capabilities), (uint8_t*) local_stream_endpoint_aac_media_codec_configuration, sizeof(local_stream_endpoint_aac_media_codec_configuration));
        btstack_assert(stream_endpoint_aac != NULL);
        stream_endpoint_aac->media_codec_configuration_info = local_stream_endpoint_aac_media_codec_configuration;
        stream_endpoint_aac->media_codec_configuration_len  = sizeof(local_stream_endpoint_aac_media_codec_configuration);
        avdtp_source_register_delay_reporting_category(avdtp_local_seid(stream_endpoint_aac));
    }
    #endif

    int aac_num = -1;
    for (int i = 0; i < num_remote_seps; i++){
        if (remote_seps[i].have_media_codec_capabilities &&
            remote_seps[i].codec_type == AVDTP_CODEC_MPEG_2_4_AAC){
            printf("found aac!!! Remote Stream Endpoints ID is %d\n", i);
            selected_remote_sep_index = i;
            aac_num = i;
            break;
        }
    }

    if (aac_num < 0) return -1;
    sc.local_stream_endpoint = stream_endpoint_aac;

    // store local seid
    media_tracker.local_seid  = avdtp_local_seid(sc.local_stream_endpoint);
    media_tracker.remote_seid = remote_seps[aac_num].seid;

    const uint8_t *cap = remote_seps[aac_num].media_codec_event;
    uint8_t object_bitmap = avdtp_subevent_signaling_media_codec_mpeg_aac_capability_get_object_type_bitmap(cap);
    uint16_t rate_bitmap = avdtp_subevent_signaling_media_codec_mpeg_aac_capability_get_sampling_frequency_bitmap(cap);
    uint8_t channels_bitmap = avdtp_subevent_signaling_media_codec_mpeg_aac_capability_get_channels_bitmap(cap);
    if (!(rate_bitmap & 8) || !(channels_bitmap & 4) || !(object_bitmap & 0x60)) return -1;
    avdtp_configuration_mpeg_aac_t configuration = {0};
    configuration.object_type = (object_bitmap & 0x20) ? AVDTP_AAC_MPEG4_LC : AVDTP_AAC_MPEG2_LC;
    configuration.sampling_frequency = 48000;
    configuration.channels = 2;
    uint32_t remote_bitrate = avdtp_subevent_signaling_media_codec_mpeg_aac_capability_get_bit_rate(cap);
    uint32_t desired=playback_options.bitrate ? playback_options.bitrate : (uint32_t)max_aac_bit_rate;
    if (playback_options.bitrate && remote_bitrate && desired>remote_bitrate) return -1;
    configuration.bit_rate = remote_bitrate == 0 || remote_bitrate > desired ? desired : remote_bitrate;
    if (configuration.bit_rate < 96000) return -1;
    configuration.vbr = 0;
    if (avdtp_config_mpeg_aac_store(media_codec_config_data, &configuration) != ERROR_CODE_SUCCESS) return -1;
    media_codec_config_len = 6;

    avdtp_capabilities_t new_configuration;
    new_configuration.media_codec.media_type = AVDTP_AUDIO;
    new_configuration.media_codec.media_codec_type =  2,    // Media Codec Type: A2DP_MEDIA_CT_AAC
    new_configuration.media_codec.media_codec_information_len = media_codec_config_len;
    new_configuration.media_codec.media_codec_information = media_codec_config_data;
    int status = avdtp_source_set_configuration(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid, 1 << AVDTP_MEDIA_CODEC, new_configuration);
    printf("Set AAC Connection Result is %d\n", status);
    finish_setup_aac = true;

    return status;
}



static int setup_aaceld_configuration(){
    int aaceld_num = -1;
    if (num_remote_seps == 0){
        printf("Remote Stream Endpoints not discovered yet\n");
        return -1;
    }
    for (int i = 0; i < num_remote_seps; i++){
        if (remote_seps[i].vendor_id == A2DP_CODEC_VENDOR_ID_APPLE && remote_seps[i].codec_id == A2DP_APPLE_CODEC_AAC_ELD){
            printf("found Apple AAC-ELD!!! Remote Stream Endpoints ID is %d\n", i);
            selected_remote_sep_index = i;
            aaceld_num = i;
            break;
        }
    }

    if (aaceld_num < 0){
        printf("not found Apple AAC-ELD!!!\n");
        return -1;
    }

    avdtp_media_codec_type_t codec_type = remote_seps[aaceld_num].codec_type;
    if (codec_type != AVDTP_CODEC_NON_A2DP) {
        printf("AAC-ELD codec type mismatch!!!\n");
        return -1;
    }

    // Create AAC-ELD stream endpoint (only once)
    if (stream_endpoint_aaceld == NULL) {
        stream_endpoint_aaceld = a2dp_source_create_stream_endpoint(
            AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
            (uint8_t *) media_aaceld_codec_capabilities, sizeof(media_aaceld_codec_capabilities),
            (uint8_t*) local_stream_endpoint_aaceld_media_codec_configuration,
            sizeof(local_stream_endpoint_aaceld_media_codec_configuration));
        btstack_assert(stream_endpoint_aaceld != NULL);
        stream_endpoint_aaceld->media_codec_configuration_info = local_stream_endpoint_aaceld_media_codec_configuration;
        stream_endpoint_aaceld->media_codec_configuration_len  = sizeof(local_stream_endpoint_aaceld_media_codec_configuration);
        avdtp_source_register_delay_reporting_category(avdtp_local_seid(stream_endpoint_aaceld));
    }

    sc.local_stream_endpoint = stream_endpoint_aaceld;

    media_tracker.local_seid  = avdtp_local_seid(sc.local_stream_endpoint);
    media_tracker.remote_seid = remote_seps[aaceld_num].seid;

    sc.local_stream_endpoint->remote_configuration_bitmap = store_bit16(sc.local_stream_endpoint->remote_configuration_bitmap, AVDTP_MEDIA_CODEC, 1);
    sc.local_stream_endpoint->remote_configuration.media_codec.media_type = AVDTP_AUDIO;
    sc.local_stream_endpoint->remote_configuration.media_codec.media_codec_type = codec_type;

    const uint8_t *event = remote_seps[aaceld_num].media_codec_event;
    const uint8_t *cap = avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information(event);
    uint16_t cap_len = avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(event);
    if (!playback_options_eld(cap, cap_len, playback_options,media_codec_config_data)) return -1;
    media_codec_config_len = PLAYBACK_ELD_CONFIG_SIZE;

    avdtp_capabilities_t new_configuration;
    new_configuration.media_codec.media_type = AVDTP_AUDIO;
    new_configuration.media_codec.media_codec_type = codec_type;
    new_configuration.media_codec.media_codec_information_len = media_codec_config_len;
    new_configuration.media_codec.media_codec_information = media_codec_config_data;
    int status = avdtp_source_set_configuration(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid, 1 << AVDTP_MEDIA_CODEC, new_configuration);

    printf("Set Apple AAC-ELD Connection Result is %d\n", status);
    return status;
}


bool avdtp_disconnect_and_scan(void){
    // Pair supersedes a profile reconnect, even if clearing the slot or inquiry fails.
    profile_pending = profile_reconnect = false;
    a2dp_accept_incoming = false;
    a2dp_demo_timer_stop(&media_tracker);
    aacp_disconnect();
    l2cap_set_flushable_local_cid(0);
    a2dp_connect_pending = false;
    a2dp_source_disconnect(media_tracker.avdtp_cid);
    if (media_tracker.avrcp_cid != 0) avrcp_disconnect(media_tracker.avrcp_cid);
    audio_slot_queue_init(); // reset all slots on disconnect
    uint8_t currect_slot = read_uint8_last_flash();

    printf("currect slot iss %d\n", currect_slot);

    uint8_t new_addr[6] = {0x0};

    bool cleared = (currect_slot == 1 && write_slot1_mac(new_addr)) ||
                   (currect_slot == 2 && write_slot2_mac(new_addr));
    if (!cleared) {
        printf("Pairing slot could not be cleared\n");
        set_led_mode_off();
        return false;
    }
    gap_drop_link_key_for_bd_addr((uint8_t *) get_device_addr());
    //gap_delete_all_link_keys();
    return gap_start_scanning();
}

void avdtp_source_establish_stream(void) {
    const uint8_t *addr = (const uint8_t *)get_device_addr();
    if (a2dp_is_connected_flag || a2dp_connect_pending || addr == NULL ||
        !peer_address_valid(addr)) return;
    a2dp_accept_incoming = true;
    a2dp_connect_pending = true;
    uint8_t status = avdtp_source_connect((uint8_t *)addr, &media_tracker.avdtp_cid);
    if (status != ERROR_CODE_SUCCESS) {
        a2dp_connect_pending = false;
        media_tracker.avdtp_cid = 0;
    }
}

void a2dp_source_reconnect(void) {
    avdtp_source_establish_stream();
}


int set_next_codec(uint8_t num){
    if (playback_options.codec==1) return setup_sbc_configuration();
    if (playback_options.codec==2) return setup_aac_configuration();
    if (playback_options.codec==3) return setup_aaceld_configuration();
    //return 0;
    //return setup_sbc_configuration();
    switch (num){
        case 0: // AAC-ELD > AAC > SBC
            if (have_aaceld_codec_capabilities){
                return setup_aaceld_configuration();
            } else {
                // Check if remote actually has AAC before trying
                bool have_aac = false;
                for (int i = 0; i < num_remote_seps; i++){
                    if (remote_seps[i].codec_type == AVDTP_CODEC_MPEG_2_4_AAC){
                        have_aac = true;
                        break;
                    }
                }
                if (have_aac) {
                    return setup_aac_configuration();
                } else {
                    printf("No AAC support, falling back to SBC\n");
                    return setup_sbc_configuration();
                }
            }
        case 1: // sbc
            return setup_sbc_configuration();
        default:
            return 1;
    }
}


void set_next_capablity_and_start_stream(void) {
    if (is_streaming || !a2dp_is_connected_flag) return;
    int result = set_next_codec(cur_capability & 1);
    if (result != ERROR_CODE_SUCCESS && !playback_options.codec && (cur_capability & 1) == 0) {
        result = setup_sbc_configuration();
    }
    if (result != ERROR_CODE_SUCCESS) {
        profile_pending=false;
        aacp_disconnect();
        a2dp_source_disconnect(media_tracker.avdtp_cid);
        return;
    }
    cur_capability = (cur_capability + 1) & 1;
    finish_scan_avdtp_codec = true;
}

static uint8_t playback_input(void) { return is_usb_streaming ? 1 : phone_active ? 2 : 0; }
static bool playback_game_eligible(void) {
    return a2dp_is_connected_flag && cur_codec==4 && aacp_model_game_supported() &&
        selected_remote_sep_index<num_remote_seps && remote_seps[selected_remote_sep_index].delay_reporting;
}
static bool playback_profile_preflight(playback_options_t next, const uint8_t peer[6]) {
    bool complete=a2dp_is_connected_flag && !memcmp(peer,cur_active_device,6) &&
        finish_scan_avdtp_codec && !remote_catalog_truncated && num_remote_seps &&
        cur_num_remote_seps==num_remote_seps;
    uint8_t matching=0;
    for (unsigned i=0;i<num_remote_seps && complete;i++) {
        if (!remote_seps[i].have_media_codec_capabilities) { complete=false; break; }
        const uint8_t *event=remote_seps[i].media_codec_event;
        if (remote_seps[i].codec_type==AVDTP_CODEC_SBC) matching|=1u<<1;
        else if (remote_seps[i].codec_type==AVDTP_CODEC_MPEG_2_4_AAC && next.codec==2) {
            if (playback_options_aac_supported(next,
                avdtp_subevent_signaling_media_codec_mpeg_aac_capability_get_object_type_bitmap(event),
                avdtp_subevent_signaling_media_codec_mpeg_aac_capability_get_sampling_frequency_bitmap(event),
                avdtp_subevent_signaling_media_codec_mpeg_aac_capability_get_channels_bitmap(event),
                avdtp_subevent_signaling_media_codec_mpeg_aac_capability_get_bit_rate(event))) matching|=1u<<2;
        } else if (remote_seps[i].codec_type==AVDTP_CODEC_NON_A2DP && next.codec==3 &&
                   remote_seps[i].vendor_id==A2DP_CODEC_VENDOR_ID_APPLE &&
                   remote_seps[i].codec_id==A2DP_APPLE_CODEC_AAC_ELD) {
            uint8_t negotiated[PLAYBACK_ELD_CONFIG_SIZE];
            if (playback_options_eld(
                avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information(event),
                avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(event),
                next,negotiated)) matching|=1u<<3;
        }
    }
    return playback_options_preflight(next,complete,matching);
}
uint8_t dongle_playback_configure(uint8_t codec, uint8_t rate_mode, uint32_t bitrate) {
    playback_options_t next={codec,rate_mode,bitrate};
    if (!playback_options_valid(next)) return DONGLE_UNSUPPORTED;
    if (playback_options.codec==codec && playback_options.rate_mode==rate_mode && playback_options.bitrate==bitrate)
        return DONGLE_ACCEPTED;
    if (is_usb_streaming || phone_active || phone_opened || usb_mic_requested() || aacp_mic_active() ||
        profile_pending || a2dp_connect_pending || playback_game.changed) return DONGLE_BUSY;
    const uint8_t *peer=(const uint8_t *)get_device_addr();
    if (!peer || !peer_address_valid(peer)) return DONGLE_DISCONNECTED;
    if (!playback_profile_preflight(next,peer)) return DONGLE_UNSUPPORTED;
    profile_previous=playback_options; playback_options=next;
    memcpy(profile_peer,peer,sizeof(profile_peer));
    profile_pending=true;
    profile_since=btstack_run_loop_get_time_ms();
    if (a2dp_is_connected_flag) {
        if (a2dp_source_disconnect(media_tracker.avdtp_cid)!=ERROR_CODE_SUCCESS) {
            playback_options=profile_previous; profile_pending=false; return DONGLE_BUSY;
        }
        profile_reconnect=true;
        a2dp_demo_timer_stop(&media_tracker);
    } else {
        profile_reconnect=false;
        avdtp_source_establish_stream();
        if (!a2dp_connect_pending) { playback_options=profile_previous; profile_pending=false; return DONGLE_DISCONNECTED; }
    }
    return DONGLE_ACCEPTED;
}
uint8_t dongle_playback_game_mode(bool enabled) {
    if (enabled && !playback_game_eligible()) return DONGLE_UNSUPPORTED;
    playback_game_request(&playback_game,enabled);
    return DONGLE_ACCEPTED;
}
void dongle_playback_poll(void) {
    const uint8_t *peer=(const uint8_t *)get_device_addr();
    uint32_t now=btstack_run_loop_get_time_ms();
    if (media_tracker.codec_ready_to_send == 2) playback_prepare_packet(&media_tracker, now);
    if (profile_pending && (!peer || memcmp(peer,profile_peer,6))) {
        profile_pending=profile_reconnect=false; playback_options=profile_previous;
    }
    if (profile_reconnect && !a2dp_is_connected_flag && !a2dp_connect_pending) {
        profile_reconnect=false; avdtp_source_establish_stream();
        if (!a2dp_connect_pending) profile_pending=false;
    }
    if (profile_pending && (uint32_t)(now-profile_since)>=15000) {
        profile_pending=profile_reconnect=false;
        a2dp_connect_pending=a2dp_accept_incoming=false;
        a2dp_demo_timer_stop(&media_tracker);
        aacp_disconnect();
        if (media_tracker.avdtp_cid) a2dp_source_disconnect(media_tracker.avdtp_cid);
    }
    if (!peer || memcmp(peer,game_peer,6)) {
        bool requested=playback_game.requested;
        memset(&playback_game,0,sizeof(playback_game)); playback_game.requested=requested;
        if (peer) memcpy(game_peer,peer,6); else memset(game_peer,0,6);
    }
    uint32_t session=aacp_control_session();
    if (session!=game_session) { playback_game_session_lost(&playback_game); game_session=session; }
    playback_game_observe_send(&playback_game,now,aacp_receive_buffer_serial(),aacp_receive_buffer_last(),delay_revision);
    bool active=media_tracker.streaming && playback_input()!=0 && !usb_mic_requested() && !aacp_mic_active();
    uint16_t target=playback_game_step(&playback_game,aacp_control_ready(),playback_game_eligible(),active,now,delay_revision,reported_delay);
    if (target && aacp_receive_buffer(target)==DONGLE_ACCEPTED)
        playback_game_queued(&playback_game,target,now,aacp_receive_buffer_serial());
    playback_idle_poll(now);
}
uint16_t dongle_playback_status(uint8_t *out, uint16_t capacity) {
    if (!out || capacity<20) return 0;
    memset(out,0,20); out[0]=1; out[1]=playback_options.codec; out[2]=dongle_output_codec();
    out[3]=playback_options.rate_mode; dongle_put32(out+4,playback_options.bitrate);
    dongle_put32(out+8,actual_bitrate); out[12]=playback_game.requested; out[13]=playback_game.phase;
    out[14]=playback_input();
    out[15]=(profile_pending ? 1 : 0) | (media_tracker.streaming ? 2 : 0) |
        ((usb_mic_requested() || aacp_mic_active()) ? 4 : 0) | (playback_game_eligible() ? 8 : 0);
    out[16]=(uint8_t)reported_delay; out[17]=(uint8_t)(reported_delay>>8); out[18]=100;
    return 20;
}
uint16_t dongle_playback_diagnostics(uint8_t *out, uint16_t capacity) {
    if (!out || capacity<64) return 0;
    memset(out,0,64); out[0]=1; out[1]=playback_input(); out[2]=dongle_output_codec();
    out[3]=(media_tracker.streaming ? 1 : 0) | ((usb_mic_requested() || aacp_mic_active()) ? 2 : 0) |
        (phone_sink_connected() ? 4 : 0) | (aacp_control_ready() ? 8 : 0);
    dongle_put32(out+4,btstack_run_loop_get_time_ms());
    if (playback_pcm_initialized) {
        critical_section_enter_blocking(&playback_pcm_lock);
        dongle_put32(out+8,playback_pcm.used); dongle_put32(out+12,playback_pcm.capacity);
        dongle_put32(out+16,playback_metrics.pcm_peak); dongle_put32(out+20,playback_metrics.pcm_underflows);
        dongle_put32(out+24,playback_metrics.pcm_drops);
        critical_section_exit(&playback_pcm_lock);
    }
    dongle_put32(out+28,playback_metrics.encoded_units); dongle_put32(out+32,playback_metrics.encoder_last_us);
    dongle_put32(out+36,playback_metrics.encoder_max_us); dongle_put32(out+40,playback_metrics.send_failures);
    dongle_put32(out+44,playback_metrics.recoveries);
    extern char end[], __StackLimit[];
    uint32_t heap=(uint32_t)((uintptr_t)__StackLimit-(uintptr_t)end);
    struct mallinfo allocations=mallinfo();
    dongle_put32(out+48,heap); dongle_put32(out+52,(uint32_t)allocations.uordblks);
    dongle_put32(out+56,(uint32_t)allocations.uordblks<=heap ? heap-(uint32_t)allocations.uordblks : UINT32_MAX);
    dongle_put32(out+60,(uint32_t)allocations.arena);
    return 64;
}


void start_led_blink(){
    set_led_mode_off();
    switch ( cur_codec ) {
        case 1:
            set_led_mode_playing_sbc();
            break;
        case 2:
            set_led_mode_playing_aac();
            break;
        case 4:  // AAC-ELD
            set_led_mode_playing_ldac();
            break;
        default:
            break;
    }

}

static bool _bt_sink_volume_changed = false;

// getter returns a pointer to that storage
bool * get_is_bt_sink_volume_changed_ptr(void){
    return &_bt_sink_volume_changed;
}

static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (phone_sink_handle_avrcp(packet_type, channel, packet, size)) return;
    UNUSED(channel);
    bd_addr_t event_addr;
    uint16_t local_cid;
    uint8_t  status = ERROR_CODE_SUCCESS;

    if (packet_type != HCI_EVENT_PACKET || size < 3 || packet[1] + 2u != size) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    
    switch (packet[2]){
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
            if (size < 14) return;
            local_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            status = avrcp_subevent_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                printf("AVRCP: Connection failed, local cid 0x%02x, status 0x%02x\n", local_cid, status);
                return;
            }
            avrcp_subevent_connection_established_get_bd_addr(packet, event_addr);
            const uint8_t *expected = a2dp_is_connected_flag ? cur_active_device : (const uint8_t *)get_device_addr();
            if (!a2dp_accept_incoming || expected == NULL ||
                memcmp(event_addr, expected, sizeof(bd_addr_t)) != 0) {
                avrcp_disconnect(local_cid);
                return;
            }
            media_tracker.avrcp_cid = local_cid;

            printf("AVRCP: Channel to %s successfully opened, avrcp_cid 0x%02x\n", bd_addr_to_str(event_addr), media_tracker.avrcp_cid);
             
            avrcp_target_support_event(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_target_support_event(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            avrcp_target_support_event(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            avrcp_target_set_now_playing_info(media_tracker.avrcp_cid, NULL, sizeof(tracks)/sizeof(avrcp_track_t));
            
            printf("Enable Volume Change notification\n");
            avrcp_controller_enable_notification(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            printf("Enable Battery Status Change notification\n");
            avrcp_controller_enable_notification(media_tracker.avrcp_cid, AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED);
            return;
        
        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            if (size < 5 || avrcp_subevent_connection_released_get_avrcp_cid(packet) != media_tracker.avrcp_cid) return;
            printf("AVRCP Target: Disconnected, avrcp_cid 0x%02x\n", avrcp_subevent_connection_released_get_avrcp_cid(packet));
            media_tracker.avrcp_cid = 0;
            return;
        default:
            break;
    }

    if (status != ERROR_CODE_SUCCESS){
        printf("Responding to event 0x%02x failed, status 0x%02x\n", packet[2], status);
    }
}


static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (phone_sink_handle_target(packet_type, channel, packet, size)) return;
    UNUSED(channel);
    uint8_t  status = ERROR_CODE_SUCCESS;

    if (packet_type != HCI_EVENT_PACKET || size < 5 || packet[1] + 2u != size) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (media_tracker.avrcp_cid == 0 || little_endian_read_16(packet, 3) != media_tracker.avrcp_cid) return;

    bool button_pressed;
    char const * button_state;
    avrcp_operation_id_t operation_id;

    switch (packet[2]){
        case AVRCP_SUBEVENT_PLAY_STATUS_QUERY:
            status = avrcp_target_play_status(media_tracker.avrcp_cid, play_info.song_length_ms, play_info.song_position_ms, play_info.status);            
            break;
        // case AVRCP_SUBEVENT_NOW_PLAYING_INFO_QUERY:
        //     status = avrcp_target_now_playing_info(avrcp_cid);
        //     break;
        case AVRCP_SUBEVENT_OPERATION:
            if (size < 8) break;
            operation_id = avrcp_subevent_operation_get_operation_id(packet);
            button_pressed = avrcp_subevent_operation_get_button_pressed(packet) > 0;
            button_state = button_pressed ? "PRESS" : "RELEASE";

            printf("AVRCP Target: operation %s (%s)\n", avrcp_operation2str(operation_id), button_state);

            if (!button_pressed){
                break;
            }
            if (phone_sink_owns_controls() && !is_usb_streaming && !usb_mic_requested()) {
                phone_sink_media_control((uint8_t)operation_id);
                break;
            }
            switch (operation_id) {
                case AVRCP_OPERATION_ID_PLAY: usb_media_enqueue(USB_MEDIA_PLAY); break;
                case AVRCP_OPERATION_ID_PAUSE: usb_media_enqueue(USB_MEDIA_PAUSE); break;
                case AVRCP_OPERATION_ID_FORWARD: usb_media_enqueue(USB_MEDIA_NEXT); break;
                case AVRCP_OPERATION_ID_BACKWARD: usb_media_enqueue(USB_MEDIA_PREVIOUS); break;
                case AVRCP_OPERATION_ID_STOP: usb_media_enqueue(USB_MEDIA_STOP); break;
                default: break;
            }
            break;
        default:
            break;
    }

    if (status != ERROR_CODE_SUCCESS){
        printf("Responding to event 0x%02x failed, status 0x%02x\n", packet[2], status);
    }
}

uint8_t get_bt_volume(){
    return media_tracker.volume;
}

static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (phone_sink_handle_controller(packet_type, channel, packet, size)) return;
    UNUSED(channel);
    
    if (packet_type != HCI_EVENT_PACKET || size < 5 || packet[1] + 2u != size) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (media_tracker.avrcp_cid == 0 || little_endian_read_16(packet, 3) != media_tracker.avrcp_cid) return;
    if (!media_tracker.avrcp_cid) return;
    
    switch (packet[2]){
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            if (size < 7) break;
            media_tracker.volume = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);
            
            if (phone_sink_owns_controls() && !is_usb_streaming && !usb_mic_requested())
                phone_sink_volume_changed(media_tracker.volume);
            else _bt_sink_volume_changed = true;
            printf("AVRCP Controller: Notification Absolute Volume %d %%\n", media_tracker.volume * 100 / 127);\

            break;
        case AVRCP_SUBEVENT_NOTIFICATION_EVENT_BATT_STATUS_CHANGED:
            if (size < 7) break;
            // see avrcp_battery_status_t
            printf("AVRCP Controller: Notification Battery Status 0x%02x\n", avrcp_subevent_notification_event_batt_status_changed_get_battery_status(packet));
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_STATE:
            if (size < 8) break;
            printf("AVRCP Controller: Notification %s - %s\n", 
                avrcp_event2str(avrcp_subevent_notification_state_get_event_id(packet)), 
                avrcp_subevent_notification_state_get_enabled(packet) != 0 ? "enabled" : "disabled");
            break;
        default:
            break;
    }  
}

int btstack_main(int argc, const char * argv[]){

    l2cap_init();
    aacp_init();
    ble_scan_init();
    bt_hci_init();

    // Initialize AVDTP Sink
    avdtp_source_init();
    avdtp_source_register_packet_handler(&packet_handler);

    // Initialize AVRCP Service
    avrcp_init();
    avrcp_register_packet_handler(&avrcp_packet_handler);
    // Initialize AVRCP Target
    avrcp_target_init();
    avrcp_target_register_packet_handler(&avrcp_target_packet_handler);

    // Initialize AVRCP Controller
    avrcp_controller_init();
    avrcp_controller_register_packet_handler(&avrcp_controller_packet_handler);


    // Initialize SDP
    sdp_init();

    // Device ID (DID) record advertising Apple as the vendor (0x004C). Several
    // AACP features are gated on the host's DID VendorID being Apple.
    static uint8_t sdp_device_id_service_buffer[100];
    memset(sdp_device_id_service_buffer, 0, sizeof(sdp_device_id_service_buffer));
    device_id_create_sdp_record(sdp_device_id_service_buffer, 0x10005,
                                DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH, 0x004C, 0x0000, 0x0000);
    sdp_register_service(sdp_device_id_service_buffer);

    memset(sdp_avdtp_source_service_buffer, 0, sizeof(sdp_avdtp_source_service_buffer));
    a2dp_source_create_sdp_record(sdp_avdtp_source_service_buffer, 0x10002, AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    sdp_register_service(sdp_avdtp_source_service_buffer);



    // Create AVRCP Target service record and register it with SDP. We receive Category 1 commands from the headphone, e.g. play/pause
    memset(sdp_avrcp_target_service_buffer, 0, sizeof(sdp_avrcp_target_service_buffer));
    uint16_t supported_features = AVRCP_FEATURE_MASK_CATEGORY_PLAYER_OR_RECORDER |
                                  AVRCP_FEATURE_MASK_CATEGORY_MONITOR_OR_AMPLIFIER;
#ifdef AVRCP_BROWSING_ENABLED
    supported_features |= AVRCP_FEATURE_MASK_BROWSING;
#endif
    // NOTE: handle must be unique — 0x10002 is already used by the A2DP source
    // record above; with a duplicate, sdp_register_service() rejects this record
    // (SDP_HANDLE_ALREADY_REGISTERED) and the AVRCP Target service is never advertised.
    avrcp_target_create_sdp_record(sdp_avrcp_target_service_buffer, 0x10004, supported_features, NULL, NULL);
    sdp_register_service(sdp_avrcp_target_service_buffer);

    // Create AVRCP Controller service record and register it with SDP. We send Category 2 commands to the headphone, e.g. volume up/down
    memset(sdp_avrcp_controller_service_buffer, 0, sizeof(sdp_avrcp_controller_service_buffer));
    uint16_t controller_supported_features = AVRCP_FEATURE_MASK_CATEGORY_MONITOR_OR_AMPLIFIER |
                                             AVRCP_FEATURE_MASK_CATEGORY_PLAYER_OR_RECORDER;
    avrcp_controller_create_sdp_record(sdp_avrcp_controller_service_buffer, 0x10003, controller_supported_features, NULL, NULL);
    sdp_register_service(sdp_avrcp_controller_service_buffer);

    if (!phone_sink_init()) return 1;


#ifdef HAVE_BTSTACK_STDIN
    btstack_stdin_setup(stdin_process);
#endif

    // turn on!
    hci_power_control(HCI_POWER_ON);

    return 0;

}
