#include <sys/cdefs.h>
#if AIRPODS_EXPERIMENTAL_RADIO0076
#include "../radio0076/radio0076.h"
#endif
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
#include <stdatomic.h>
#include "btstack.h"
#include "btstack_avdtp_source.h"
#include "pico/platform/sections.h"
#include "pico/sync.h"
#include <aacenc_lib.h>
#define PLAYBACK_PCM_Q31_TYPE INT_PCM
#include "playback_pcm.h"
#include "playback_duck.h"
#include "accessory_automation.h"
#include "../runtime_watch.h"
#include "playback_inspect.h"
#include "playback_eld.h"
#include "playback_continuity.h"
#include "playback_peer.h"
#include "playback_format.h"
#include "aaceld_quality.h"
#include "playback_options.h"
#include "playback_bitrate.h"
#include "pico/time.h"
#include "phone_sink.h"
#include "phone_call.h"
#include "phone_clock.h"

_Static_assert(PHONE_PCM_CAPACITY <= PLAYBACK_PCM_FRAMES, "Phone jitter reserve must fit the shared PCM ring");
#include "playback_adjust.h"
#include "playback_trace.h"
#include "playback_usb_clock.h"
#include "bluetooth_volume.h"
#include "avrcp_connection.h"
#include "media_session.h"
#include "a2dp_codec_identifiers.h"
#include "a2dp_codec_configuration.h"
#include "bluetooth_debug_console.h"
#include "../tinyusb/dongle_control.h"
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


_Static_assert(sizeof(INT_PCM)==sizeof(int32_t) && SAMPLE_BITS==32,"FDK and playback must share Q31 PCM");
const uint32_t airpods_eld_cbr_bandwidth_hz __attribute__((used)) = AACELD_CBR_BANDWIDTH;


#define A2DP_CODEC_VENDOR_ID_FRAUNHOFER 0x08A9
#define A2DP_FRAUNHOFER_CODEC_LC3PLUS 0x0001

#define VOLUME_REDUCTION 2

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

    uint32_t time_audio_data_sent; // ms
    uint32_t acc_num_missed_samples;
    uint32_t samples_ready;
    uint64_t sample_debt;
    btstack_timer_source_t audio_timer;
    uint8_t  streaming;
    int      max_media_payload_size;

    uint32_t rtp_timestamp;
    uint32_t send_wait_start;


    uint8_t  codec_storage[PLAYBACK_ELD_PAYLOAD_CEILING];
    uint16_t codec_storage_count;
    uint8_t  codec_ready_to_send;
    uint16_t codec_num_frames;
}  a2dp_media_sending_context_t;




#ifdef HAVE_AAC_FDK
static HANDLE_AACENCODER handleAAC;
static AACENC_InfoStruct aacinf;
enum { ENCODE_IDLE, ENCODE_PREPARED, ENCODE_RUNNING, ENCODE_DONE };
// One job exclusively owns the existing PCM buffer and unused packet tail.
static struct {
    atomic_uint epoch, state;
    HANDLE_AACENCODER handle;
    uint32_t epoch_at_prepare, session, usb_revision, unit_started, encode_us;
    uint32_t cancelled, deferred_epoch, deferred_session, accessory_epoch;
    uint16_t samples, offset, capacity, reset_cid, sample_rate;
    bool duck;
    int error, bytes, consumed;
    uint8_t codec, input, deferred_size, deferred_event[40], deferred_peer[6];
    bool close_pending, reset_pending;
} encoder_job = {.epoch = 1};
static playback_duck_t encoder_duck = {.gain=PLAYBACK_DUCK_UNITY, .target=PLAYBACK_DUCK_UNITY};
static uint32_t encoder_duck_epoch;
static uint32_t usb_ingress_frames;
_Static_assert(sizeof(encoder_job) <= 128, "AAC job must not consume another audio buffer");
static bool playback_encoder_busy(void) {
    return atomic_load_explicit(&encoder_job.state, memory_order_acquire) != ENCODE_IDLE;
}
static void playback_encoder_invalidate(void) {
    atomic_fetch_add_explicit(&encoder_job.epoch, 1u, memory_order_relaxed);
}
static void playback_encoder_close(void);
static bool playback_encoder_defer_configuration(const uint8_t *packet, uint16_t size);
static bool playback_encoder_scope_current(void);
static void playback_encoder_discard(bool current_job);
static bool playback_encoder_packet_current(void);
#endif

static a2dp_media_sending_context_t media_tracker;

static int current_sample_rate = 48000;

static uint8_t sdp_avdtp_source_service_buffer[150];

static uint8_t cur_capability = 0;
static bool is_streaming = false;
static bool is_usb_streaming;
static bool phone_opened, phone_active, phone_buffer_started;
static bool call_opened, call_active;
static bool phone_output_recovery;
static uint16_t phone_output_recovery_cid;
static uint32_t phone_output_recovery_since;
static uint8_t phone_output_recovery_peer[6];
static phone_clock_t phone_clock;
static btstack_resample_t phone_resampler;
static void playback_media_sent(void);
static uint8_t cur_codec = 0;
static playback_options_t playback_options;
static playback_bitrate_t playback_bitrate;
static uint32_t bitrate_pressure, bitrate_discontinuities, bitrate_radio_epoch;
static uint8_t bitrate_input;
static bool bitrate_game_requested;
static void playback_bitrate_game_packet(uint32_t wait_us, uint32_t gap_us);
static void playback_bitrate_transport_stalled(void);
static playback_game_t playback_game = {.requested = false, .phase = GAME_OFF};
static playback_usb_verification_t usb_game_verification;
static playback_adjust_t playback_adjust;
static playback_usb_clock_t usb_clock={.fill_q8=PLAYBACK_USB_RESERVE*256,.rate_q16=65536};
static playback_trace_t playback_trace={.stats={[TRACE_PCM_MIN]=UINT32_MAX}};
static uint32_t usb_last_arrival_us, media_last_sent_us, packet_oldest_us;
static bool playback_work_pending;
static bool playback_transport_stale;
static playback_continuity_t phone_continuity;
static bool mic_prepare_open, mic_recovery_pending, mic_recovery_started;
static uint32_t mic_prepare_serial, mic_prepare_session, mic_recovery_since, mic_recovery_revision;
static uint16_t mic_recovery_cid;
static uint32_t usb_recovery_drops;

static void playback_trace_add(unsigned stat, uint32_t value) {
    uint32_t irq=save_and_disable_interrupts();
    playback_trace.stats[stat]+=value;
    restore_interrupts(irq);
}
static void playback_trace_max(unsigned stat, uint32_t value) {
    uint32_t irq=save_and_disable_interrupts();
    if(value>playback_trace.stats[stat])playback_trace.stats[stat]=value;
    restore_interrupts(irq);
}
static void playback_trace_note(unsigned kind, uint32_t a, uint32_t b) {
    uint32_t irq=save_and_disable_interrupts();
    playback_trace_record(&playback_trace,time_us_32(),kind,a,b);
    restore_interrupts(irq);
}
#define PB_ADD(stat,value) playback_trace_add(stat,value)
#define PB_MAX(stat,value) playback_trace_max(stat,value)
#define PB_EVENT(kind,a,b) playback_trace_note(kind,a,b)
static uint32_t actual_bitrate, delay_revision, game_session;
static uint16_t reported_delay=UINT16_MAX;
static bool profile_pending, profile_reconnect;
static uint32_t profile_since;
static uint32_t reconnect_since, reconnect_delay = 1000;
static playback_options_t profile_previous;
static uint8_t profile_peer[6];
static uint8_t game_peer[6];
enum { PLAYBACK_RUNNING, PLAYBACK_SUSPENDING, PLAYBACK_SUSPENDED, PLAYBACK_STARTING, PLAYBACK_CLAIMING };
static uint8_t playback_idle_phase;
static struct {
    uint32_t session, revision;
    uint16_t cid;
    uint8_t seid, peer[6];
    bool active;
} playback_claim;
static bool playback_idle_counting;
static bool playback_peer_suspended;
static uint32_t usb_playback_revision, playback_peer_suspend_revision;
static uint32_t playback_idle_since, playback_idle_transition_since;
#define PLAYBACK_IDLE_MS 1000u
#define PLAYBACK_TRANSITION_MS 3000u
static struct {
    uint32_t pcm_peak, pcm_underflows, pcm_drops, encoded_units;
    uint32_t encoder_last_us, encoder_max_us, send_failures, recoveries;
} playback_metrics;
// Boot-lifetime modulo-32 totals; explicit reports do not reset them.
static struct {
    uint32_t aac_calls, aac_us, aac_jobs, aac_job_us;
    uint32_t sent_packets, sent_samples, send_wait_us;
    uint32_t cancelled_waits, cancelled_wait_us;
    uint32_t recovery_poll_ms, recovery_last_ms;
    bool recovery_active;
} playback_work_metrics;
_Static_assert(sizeof(playback_work_metrics) <= 48, "Output diagnostics must fit the existing memory reserve");
static void playback_work_cancel_wait(a2dp_media_sending_context_t *context) {
    if (!context->codec_ready_to_send) return;
    ++playback_work_metrics.cancelled_waits;
    playback_work_metrics.cancelled_wait_us +=
        (btstack_run_loop_get_time_ms() - context->send_wait_start) * 1000u;
}
static playback_inspect_t playback_inspect;
static volatile bool inspect_enabled;
static uint32_t inspect_deadline;

uint8_t dongle_output_codec(void) {
    return get_a2dp_connected_flag() ? (cur_codec == 4 ? 3 : cur_codec <= 2 ? cur_codec : 0) : 0;
}

static struct {
    avdtp_stream_endpoint_t * local_stream_endpoint;
}  sc;


static btstack_sbc_encoder_state_t sbc_encoder_state;
static uint8_t is_cmd_triggered_localy = 0;

static bool a2dp_is_connected_flag = false;
static bool a2dp_connect_pending;
static bool a2dp_accept_incoming = true;
static bool reconnect_policy_supported, reconnect_automatic, reconnect_auto_intent;
static struct {uint8_t peer[6];uint16_t cid;uint32_t since;} passive_attempt;
static bool playback_released;
static uint8_t release_peer[6];
static bool playback_release_pending;
static uint32_t release_revision, release_session;
static uint16_t release_cid;

static bool finish_scan_avdtp_codec = false;

static uint8_t audio_timer_interval = 5;

// on pico 2w the max stable aac bit rate under 512 simples without vbr is around 220000
static uint8_t aac_audio_timer_interval = 12;
static uint16_t acc_num_simples = 1024;


bool get_a2dp_connected_flag(){
    return a2dp_is_connected_flag;
}

static int a2dp_sample_rate(void){
    return current_sample_rate;
}


static void playback_prepare_packet(a2dp_media_sending_context_t *, uint32_t);
static void playback_clock_accrue(a2dp_media_sending_context_t *,uint32_t);
static void playback_clock_switch(a2dp_media_sending_context_t *,uint32_t,uint8_t,uint8_t);
static void playback_adjust_cancel_stream(a2dp_media_sending_context_t *,uint32_t,uint8_t);

static void a2dp_demo_send_media_packet(void) {
    a2dp_media_sending_context_t *context = &media_tracker;
    if (playback_encoder_busy()) return;
    if (!playback_encoder_packet_current()) return;
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
    uint32_t wait_us=(btstack_run_loop_get_time_ms()-context->send_wait_start)*1000u;
    ++playback_work_metrics.sent_packets;
    playback_work_metrics.sent_samples += samples;
    playback_work_metrics.send_wait_us += wait_us;
    PB_MAX(TRACE_SEND_WAIT_US,wait_us);
    uint32_t sent_us=time_us_32();
    if(cur_codec==4 && inspect_enabled)playback_inspect_media(&playback_inspect,
        context->codec_storage,context->codec_storage_count,sent_us);
    uint32_t gap_us=media_last_sent_us?sent_us-media_last_sent_us:0;
    if (cur_codec == 4 && (wait_us > 20000u || gap_us > 40000u)) ++bitrate_pressure;
    if (cur_codec == 4) playback_bitrate_game_packet(wait_us, gap_us);
    uint32_t receiver_ms=playback_game.changed?playback_game.target:150u;
    if (cur_codec == 4 && wait_us > receiver_ms * 1000u) playback_bitrate_transport_stalled();
    if (cur_codec==4 && phone_active && phone_buffer_started && !call_active &&
        !is_usb_streaming && aacp_audio_owned() && !aacp_mic_active() &&
        playback_idle_phase==PLAYBACK_RUNNING && !playback_adjust_busy(&playback_adjust) &&
        !playback_game.waiting_tx && !playback_game.waiting_report) {
        if (playback_continuity_sent(&phone_continuity, sent_us,
                context->codec_num_frames * 10000u, receiver_ms * 1000u))
            playback_transport_stale=true;
    } else playback_continuity_reset(&phone_continuity);
    if(cur_codec==4 && (is_usb_streaming || phone_active || call_active) && media_last_sent_us &&
       !playback_adjust_busy(&playback_adjust) && gap_us>receiver_ms*1000u)
        playback_transport_stale=true;
    if((is_usb_streaming || phone_active || call_active) && (wait_us>20000u || gap_us>40000u))
        PB_EVENT(TRACE_SEND_WAIT,wait_us,gap_us);
    if(media_last_sent_us)PB_MAX(TRACE_SEND_GAP_US,sent_us-media_last_sent_us);
    if(packet_oldest_us)PB_MAX(TRACE_PACKET_AGE_US,sent_us-packet_oldest_us);
    media_last_sent_us=sent_us;
    packet_oldest_us=0;
    // Keep the fork's sample clock; failed sends retain the same payload and timestamp.
    context->rtp_timestamp += samples;
    bool adjusting = playback_adjust.running;
    uint8_t old_interval=playback_adjust_interval(&playback_adjust);
    uint32_t now = btstack_run_loop_get_time_ms();
    // One unit can be due while the last encode finishes. A larger gap is not deliberate drift.
    bool caught_up = (int32_t)(now - playback_adjust.next_unit_ms) < old_interval;
    playback_adjust_sent(&playback_adjust, samples, now, caught_up);
    if (adjusting && !playback_adjust.running) {
        if (playback_adjust.deferred)
            printf("[GAME] clock deferred: target=%u elapsed_ms=%lu sent_frames=%lu\n",
                playback_adjust.target, (unsigned long)(now-playback_adjust.started),
                (unsigned long)playback_adjust.sent_frames);
        else printf("[GAME] clock done: target=%u elapsed_ms=%lu sent_frames=%lu failed=%u\n",
            playback_adjust.target, (unsigned long)(now-playback_adjust.started),
            (unsigned long)playback_adjust.sent_frames, playback_adjust.failed);
        if (playback_adjust.failed) playback_game.failed=true;
        playback_adjust_cancel_stream(context,now,old_interval);
    }
    playback_media_sent();
    context->codec_storage_count = 0;
    context->codec_ready_to_send = 0;
    context->codec_num_frames = 0;
    playback_work_pending=true;
}

// The debug console's manual send (media_session.h); the timer path calls the static function.
void media_session_send_media_packet(void) { a2dp_demo_send_media_packet(); }

static playback_pcm_t playback_pcm;
static playback_resample_t encoder_resample;
static critical_section_t playback_pcm_lock;
static bool playback_pcm_initialized;
// The unused core1 bank holds this serialized buffer; the linker rejects a core1 stack here.
static playback_encoder_pcm_t __scratch_x("encoder_pcm") encoder_pcm;
_Static_assert(sizeof(encoder_pcm.s16)==AUDIO_SLOT_MAX_INT16*sizeof(int16_t),"Legacy encoder staging size");
static uint16_t pcm_frame_samples = 128;
static uint8_t active_slot_count = AUDIO_SLOT_COUNT_SBC;
static uint32_t audio_slot_capacity(void) {
    uint32_t capacity=(uint32_t)pcm_frame_samples*active_slot_count;
    return capacity<PLAYBACK_USB_ACTIVE_LIMIT?PLAYBACK_USB_ACTIVE_LIMIT:capacity;
}

bd_addr_t cur_active_device;
static uint16_t aaceld_frame_sequence = 1;
static uint32_t aaceld_bitrate = PLAYBACK_ELD_BITRATE;
static bool aaceld_variable_allowed;
bool allow_switch_slot = true;

// Narrow session view for the units split out of this file (media_session.h).
uint16_t media_session_cid(void) { return media_tracker.avdtp_cid; }
bool media_session_accepts_incoming(void) { return a2dp_accept_incoming; }
bool media_session_profile_change_pending(void) { return profile_pending; }
const uint8_t *media_session_active_device(void) { return cur_active_device; }
uint8_t media_session_codec(void) { return cur_codec; }
playback_options_t media_session_playback_options(void) { return playback_options; }
bool media_session_codec_scan_finished(void) { return finish_scan_avdtp_codec; }
struct avdtp_stream_endpoint *media_session_local_endpoint(void) { return sc.local_stream_endpoint; }
void media_session_set_local_endpoint(struct avdtp_stream_endpoint *endpoint) { sc.local_stream_endpoint = endpoint; }
uint8_t media_session_local_seid(void) { return media_tracker.local_seid; }
void media_session_set_local_seid(uint8_t local_seid) { media_tracker.local_seid = local_seid; }
uint8_t media_session_remote_seid(void) { return media_tracker.remote_seid; }
void media_session_set_remote_seid(uint8_t remote_seid) { media_tracker.remote_seid = remote_seid; }
uint8_t media_session_connect_signaling(void) {
    return avdtp_source_connect((uint8_t *) get_device_addr(), &media_tracker.avdtp_cid);
}
void media_session_mark_local_command(void) { is_cmd_triggered_localy = 1; }
static btstack_timer_source_t suspend_recovery_timer;
static bool suspend_recovery_pending;
static bool recovery_reset_encoder;
#define SUSPEND_RECOVERY_TIMEOUT_MS 3000

static void playback_media_sent(void) {
    critical_section_enter_blocking(&playback_pcm_lock);
    playback_pcm_sent(&playback_pcm);
    uint32_t revision=usb_playback_revision;
    bool fresh_start=is_usb_streaming && playback_pcm.primed_on_wire && revision!=playback_game.usb_start_revision;
    critical_section_exit(&playback_pcm_lock);
    if (fresh_start && !phone_active && !call_active && !playback_released && !playback_claim.active &&
        !profile_pending && !a2dp_connect_pending && playback_game_eligible() &&
        aacp_audio_owned() && aacp_control_ready() && !aacp_mic_requested() && !aacp_mic_active()) {
        // Retry a failed Off once after real USB media resumes on the owned route.
        playback_game.usb_start_revision=revision;
        if (!playback_game.requested) playback_game_request(&playback_game,false);
    }
}
static bool playback_pcm_warm(void) {
    critical_section_enter_blocking(&playback_pcm_lock);
    bool warm = playback_pcm.primed_on_wire;
    critical_section_exit(&playback_pcm_lock);
    return warm;
}
static bool playback_usb_verify_scope(bool activity) {
    const uint8_t *peer = (const uint8_t *)get_device_addr();
    bool valid = cur_codec == 4 && a2dp_is_connected_flag && media_tracker.streaming &&
        playback_idle_phase == PLAYBACK_RUNNING && !profile_pending && !a2dp_connect_pending &&
        !phone_opened && !phone_active && !call_opened && !call_active &&
        !aacp_mic_requested() && !aacp_mic_active() && aacp_audio_owned() && aacp_control_ready() &&
        playback_game.requested && playback_game.target == 60 && !playback_game.failed &&
        !playback_game.restore_failed && usb_game_verification.session == aacp_control_session() &&
        usb_game_verification.cid == media_tracker.avdtp_cid &&
        usb_game_verification.seid == media_tracker.local_seid && peer &&
        !memcmp(peer, usb_game_verification.peer, 6) && !memcmp(cur_active_device, peer, 6);
    if (!valid || !activity) return valid;
    critical_section_enter_blocking(&playback_pcm_lock);
    valid = is_usb_streaming && usb_game_verification.usb_revision == usb_playback_revision;
    critical_section_exit(&playback_pcm_lock);
    return valid;
}
static void playback_usb_verify_cancel(void) {
    if (usb_game_verification.queued && usb_game_verification.session == aacp_control_session() &&
        usb_game_verification.cid == media_tracker.avdtp_cid && playback_game.target == 60 &&
        playback_game.tx_revision == usb_game_verification.tx_revision) {
        if (playback_game.waiting_tx) aacp_receive_buffer_cancel();
        playback_game.waiting_tx = playback_game.waiting_report = false;
        playback_game.phase = playback_game.requested ? GAME_CONFIRMED : GAME_RESTORING;
        playback_adjust_cancel_stream(&media_tracker, btstack_run_loop_get_time_ms(),
                                      playback_adjust_interval(&playback_adjust));
    }
    usb_game_verification.needed = usb_game_verification.queued = usb_game_verification.unverified = false;
}
static void playback_usb_verify_started(a2dp_media_sending_context_t *context) {
    playback_usb_verify_cancel();
    if (!playback_game.requested || !playback_game.changed || playback_game.target != 60 ||
        playback_game.failed || playback_game.restore_failed ||
        (playback_game.phase != GAME_CONFIRMED && !playback_game.waiting_tx && !playback_game.waiting_report))
        return;
    usb_game_verification.unverified = true;
    usb_game_verification.session = aacp_control_session();
    usb_game_verification.cid = context->avdtp_cid;
    usb_game_verification.seid = context->local_seid;
    memcpy(usb_game_verification.peer, cur_active_device, 6);
    if (!is_usb_streaming || phone_opened || phone_active || call_opened || call_active) return;
    critical_section_enter_blocking(&playback_pcm_lock);
    usb_game_verification.usb_revision = usb_playback_revision;
    critical_section_exit(&playback_pcm_lock);
    usb_game_verification.since = btstack_run_loop_get_time_ms();
    usb_game_verification.needed = true;
}
static bool playback_usb_verify_poll(uint32_t now) {
    if (!usb_game_verification.needed && !usb_game_verification.queued) {
        if (usb_game_verification.unverified &&
            (usb_game_verification.session != aacp_control_session() ||
             usb_game_verification.cid != media_tracker.avdtp_cid ||
             memcmp(usb_game_verification.peer, cur_active_device, 6))) {
            playback_usb_verify_cancel();
            return false;
        }
        // A warm phone-to-USB handoff need not produce a new AVDTP Start.
        if (usb_game_verification.unverified && is_usb_streaming && !phone_opened && !phone_active &&
            !call_opened && !call_active && media_tracker.streaming &&
            (playback_game.phase == GAME_CONFIRMED || playback_game.waiting_tx || playback_game.waiting_report))
            playback_usb_verify_started(&media_tracker);
        if (!usb_game_verification.needed) return false;
    }
    if (usb_game_verification.queued && !playback_game.waiting_tx && !playback_game.waiting_report &&
        !playback_adjust_busy(&playback_adjust) && playback_game.phase == GAME_CONFIRMED &&
        playback_usb_verify_scope(false)) {
        usb_game_verification.queued = false;
        usb_game_verification.unverified = false;
        return false;
    }
    if (!playback_usb_verify_scope(false)) {
        bool external = (phone_opened || phone_active || call_opened || call_active) &&
            playback_game.requested && playback_game.changed && playback_game.target == 60 &&
            !playback_game.failed && aacp_audio_owned();
        playback_usb_verify_cancel();
        usb_game_verification.unverified = external;
        return false;
    }
    if (!playback_usb_verify_scope(true)) {
        // Preserve an unfinished need across USB pause/resume, never its old queued proof.
        playback_usb_verify_cancel();
        usb_game_verification.needed = usb_game_verification.unverified = true;
        critical_section_enter_blocking(&playback_pcm_lock);
        usb_game_verification.usb_revision = usb_playback_revision;
        bool active = is_usb_streaming;
        critical_section_exit(&playback_pcm_lock);
        usb_game_verification.since = now;
        if (!active) return false;
    }
    if (!usb_game_verification.needed) return false;
    if ((uint32_t)(now - usb_game_verification.since) >= 2000u) {
        usb_game_verification.needed = usb_game_verification.unverified = false;
        return false;
    }
    return playback_pcm_warm() && !playback_game.waiting_tx && !playback_game.waiting_report &&
        !playback_adjust_busy(&playback_adjust);
}
static void playback_usb_verify_sent(void) {
    if (aacp_receive_buffer_last() != 60) {
        if (aacp_receive_buffer_last() == 150) playback_usb_verify_cancel();
        return;
    }
    bool live = is_usb_streaming && !phone_opened && !phone_active && !call_opened && !call_active &&
        media_tracker.streaming && playback_pcm_warm() && !aacp_mic_requested() && !aacp_mic_active();
    if (!live) {
        usb_game_verification.unverified = true;
        usb_game_verification.session = aacp_control_session();
        usb_game_verification.cid = media_tracker.avdtp_cid;
        usb_game_verification.seid = media_tracker.local_seid;
        memcpy(usb_game_verification.peer, cur_active_device, 6);
        return;
    }
    if (usb_game_verification.needed || usb_game_verification.queued) {
        if (!playback_usb_verify_scope(true)) { (void)playback_usb_verify_poll(btstack_run_loop_get_time_ms()); return; }
        usb_game_verification.needed = false;
        usb_game_verification.queued = true;
        usb_game_verification.tx_revision = playback_game.tx_revision;
    } else usb_game_verification.unverified = false; // Ordinary live ON already does this check.
}
static void playback_pcm_push_measured(const int16_t *src, uint32_t frames) {
    if (playback_pcm.capacity && src && frames) {
        uint32_t available=playback_pcm.capacity-playback_pcm.used;
        if (frames>available) {
            uint32_t dropped=frames-available;
            playback_metrics.pcm_drops+=dropped;
            if (phone_active && !call_active && !is_usb_streaming && playback_pcm.primed_on_wire)
                phone_sink_record_loss(true, dropped, playback_pcm.used, media_tracker.samples_ready);
            if (phone_active || call_active) {
                PB_ADD(playback_pcm.primed_on_wire?TRACE_ACTIVE_DROP:TRACE_STARTUP_DROP,dropped);
                if (playback_pcm.primed_on_wire) PB_EVENT(TRACE_DROP,dropped,playback_pcm.used);
            }
        }
    }
    playback_pcm_push(&playback_pcm,src,frames);
    if (playback_pcm.used>playback_metrics.pcm_peak) playback_metrics.pcm_peak=playback_pcm.used;
}
static void playback_encode_measured(uint32_t started) {
    uint32_t elapsed=time_us_32()-started;
    playback_metrics.encoder_last_us=elapsed;
    if (elapsed>playback_metrics.encoder_max_us) playback_metrics.encoder_max_us=elapsed;
    PB_MAX(TRACE_ENCODER_US,elapsed);
}

bool bridge_phone_open(void) {
    if (call_opened || call_active || is_usb_streaming || aacp_mic_requested() ||
        !peer_address_valid((const uint8_t *)get_device_addr())) return false;
    phone_opened = true;
    avdtp_source_establish_stream();
    return true;
}

void bridge_phone_start_requested(void) {
    const uint8_t *peer = (const uint8_t *)get_device_addr();
    if (call_opened || call_active || is_usb_streaming ||
        aacp_mic_requested() || aacp_mic_active() || !aacp_audio_owned() || profile_pending || a2dp_connect_pending ||
        !peer_address_valid(peer) || memcmp(peer, cur_active_device, 6)) return;
    if (!bridge_phone_open()) return;
    // A fresh authenticated phone Start is new demand; opening its profile is not.
    dongle_playback_ownership_changed(true);
}

bool bridge_phone_ready(void) {
    if (!phone_opened || is_usb_streaming || aacp_mic_requested() ||
        !a2dp_is_connected_flag || !media_tracker.streaming || playback_idle_phase != PLAYBACK_RUNNING) return false;
    return playback_pcm_warm();
}

static bool phone_output_recovery_owned(void) {
    const uint8_t *peer = (const uint8_t *)get_device_addr();
    return phone_output_recovery && phone_opened && phone_active &&
        !is_usb_streaming && !aacp_mic_requested() && !aacp_mic_active() &&
        a2dp_is_connected_flag && !profile_pending && !a2dp_connect_pending &&
        media_tracker.avdtp_cid == phone_output_recovery_cid && peer &&
        !memcmp(peer, phone_output_recovery_peer, 6) &&
        !memcmp(cur_active_device, phone_output_recovery_peer, 6);
}

bool bridge_phone_recovering(void) {
    return phone_output_recovery_owned() &&
        (uint32_t)(btstack_run_loop_get_time_ms() - phone_output_recovery_since) < PLAYBACK_TRANSITION_MS;
}

bool bridge_phone_start(void) {
    playback_encoder_invalidate();
    if (!bridge_phone_ready()) return false;
    playback_continuity_reset(&phone_continuity);
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
    playback_encoder_invalidate();
    critical_section_enter_blocking(&playback_pcm_lock);
    if (phone_active && !call_active) {
        playback_continuity_reset(&phone_continuity);
        playback_pcm_clear(&playback_pcm, PHONE_PCM_CAPACITY);
        phone_buffer_started = false;
        phone_clock_init(&phone_clock, acc_num_simples);
        btstack_resample_init(&phone_resampler, 2);
    }
    critical_section_exit(&playback_pcm_lock);
}

void bridge_phone_stop(void) {
    playback_encoder_invalidate();
    playback_continuity_reset(&phone_continuity);
    phone_output_recovery = false;
    critical_section_enter_blocking(&playback_pcm_lock);
    if (phone_active && !call_active)
        playback_pcm_clear(&playback_pcm, audio_slot_capacity());
    phone_opened = phone_active = false;
    if (!call_active) phone_buffer_started = false;
    critical_section_exit(&playback_pcm_lock);
}

static void bridge_input_pcm_push(const int16_t *src, uint16_t frames) {
    if (src == NULL) return;
    while (frames != 0) {
        uint16_t count = frames > 128 ? 128 : frames;
        int16_t adjusted[130 * 2];
        uint16_t output = btstack_resample_block(&phone_resampler, src, count, adjusted);
        if (output > 130) {
            if (call_active) bridge_call_stop();
            else bridge_phone_drop_pcm();
            return;
        }
        critical_section_enter_blocking(&playback_pcm_lock);
        playback_pcm_push_measured(adjusted, output);
        PB_MAX(TRACE_PCM_MAX, playback_pcm.used);
        critical_section_exit(&playback_pcm_lock);
        src += count * 2;
        frames -= count;
    }
}

void bridge_phone_pcm_push(const int16_t *src, uint16_t frames) {
    if (phone_active && (bridge_phone_ready() || bridge_phone_recovering())) bridge_input_pcm_push(src, frames);
}

static void call_clock_init(void) {
    uint32_t a = 360u, b = acc_num_simples;
    while (b) { uint32_t next = a % b; a = b; b = next; }
    // Reserve 20 ms after reads, plus the 7.5 ms SCO arrival sawtooth.
    phone_clock.target_q8 = (int32_t)(960u + (360u - a) / 2u) * 256;
    phone_clock.fill_q8 = phone_clock.target_q8;
    btstack_resample_init(&phone_resampler, 2);
    phone_buffer_started = false;
}

bool bridge_call_open(void) {
    if (is_usb_streaming || usb_mic_requested() ||
        !peer_address_valid((const uint8_t *)get_device_addr())) return false;
    if (!call_opened) {
        playback_encoder_invalidate();
        call_opened = true;
        phone_sink_pause_for_call();
    }
    if (!a2dp_is_connected_flag) avdtp_source_establish_stream();
    if (!call_active && a2dp_is_connected_flag && media_tracker.streaming &&
        playback_idle_phase == PLAYBACK_RUNNING && playback_pcm_warm()) {
        critical_section_enter_blocking(&playback_pcm_lock);
        playback_pcm_clear(&playback_pcm, PHONE_PCM_CAPACITY);
        call_clock_init();
        call_active = true;
        critical_section_exit(&playback_pcm_lock);
    }
    return true;
}

bool bridge_call_ready(void) {
    return call_opened && call_active && !is_usb_streaming && !usb_mic_requested() &&
        a2dp_is_connected_flag && media_tracker.streaming &&
        playback_idle_phase == PLAYBACK_RUNNING && playback_pcm_warm();
}

void bridge_call_stop(void) {
    if (call_opened || call_active) playback_encoder_invalidate();
    critical_section_enter_blocking(&playback_pcm_lock);
    if (call_active && !phone_active) playback_pcm_clear(&playback_pcm, audio_slot_capacity());
    call_opened = call_active = false;
    critical_section_exit(&playback_pcm_lock);
}

void bridge_call_pcm_push(const int16_t *src, uint16_t frames) {
    if (bridge_call_ready()) bridge_input_pcm_push(src, frames);
}


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
    playback_encoder_invalidate();
    if (!playback_pcm_initialized) {
        critical_section_init(&playback_pcm_lock);
        playback_pcm_initialized = true;
    }
    critical_section_enter_blocking(&playback_pcm_lock);
    playback_pcm_reset(&playback_pcm, audio_slot_capacity(), false);
    critical_section_exit(&playback_pcm_lock);
}

static void audio_slot_queue_configure_with_count(uint16_t samples_per_slot, uint8_t slot_count) {
    playback_encoder_invalidate();
    if (samples_per_slot == 0 || samples_per_slot > AUDIO_SLOT_MAX_SAMPLES ||
        slot_count == 0 || (uint32_t)samples_per_slot * slot_count > PLAYBACK_PCM_FRAMES) return;
    critical_section_enter_blocking(&playback_pcm_lock);
    pcm_frame_samples = samples_per_slot;
    active_slot_count = slot_count;
    playback_pcm_reset(&playback_pcm, audio_slot_capacity(), false);
    critical_section_exit(&playback_pcm_lock);
}

// Driverless accessory behavior is scoped to this audio owner, never a remembered device choice.
static struct {
    accessory_automation_t state;
    uint32_t session, host, epoch;
    uint16_t cid;
    uint8_t input;
    bool admitted;
} playback_accessory;
static atomic_uint usb_signal_stamp;
static atomic_uint usb_program_stamp;
static playback_duck_t sbc_duck = {.gain=PLAYBACK_DUCK_UNITY, .target=PLAYBACK_DUCK_UNITY};

static void playback_accessory_update(bool dispatch) {
    uint32_t session=aacp_control_session(), host=usb_media_generation();
    uint8_t input=is_usb_streaming ? 1 : phone_active ? 2 : usb_host_active() ? 1 : 0;
    bool allowed=input && (input!=1 || (host&1u)) && a2dp_is_connected_flag && aacp_control_ready() &&
        aacp_settings_model_known() && aacp_audio_owned() &&
        !call_active && !call_opened && !usb_mic_requested() && !aacp_mic_requested() && !aacp_mic_active() &&
        !mic_prepare_open && !mic_recovery_pending && !profile_pending && !playback_released;
    bool changed=playback_accessory.session!=session || playback_accessory.host!=host ||
        playback_accessory.cid!=media_tracker.avdtp_cid || playback_accessory.input!=input;
    if(changed || playback_accessory.admitted!=allowed) {
        bool same_session=playback_accessory.session==session;
        bool ear=playback_accessory.state.ear_enabled, ca=playback_accessory.state.ca_enabled;
        accessory_automation_reset(&playback_accessory.state);
        if(same_session) {
            playback_accessory.state.ear_enabled=ear;playback_accessory.state.ca_enabled=ca;
        }
        playback_accessory.session=session;playback_accessory.host=host;
        playback_accessory.cid=media_tracker.avdtp_cid;playback_accessory.input=input;
        playback_accessory.admitted=allowed;
        if(changed) {
            if(++playback_accessory.epoch==0)++playback_accessory.epoch;
            playback_duck_reset(&sbc_duck);
        }
        atomic_store_explicit(&usb_signal_stamp,0,memory_order_release);
        usb_media_cancel_automatic();
    }
    uint32_t now=btstack_run_loop_get_time_ms();
    if(allowed && !playback_accessory.state.have_presence) {
        // Wear can arrive before metadata or audio ownership; seed only this session's baseline.
        uint8_t cached[40];
        uint16_t length=aacp_research_cached(5,cached,sizeof(cached));
        if(length>=16 && cached[12]==6)
            accessory_automation_packet(&playback_accessory.state,cached+8,length-8,now);
    }
    if(!allowed || !dispatch)return;
    uint32_t stamp=atomic_load_explicit(&usb_signal_stamp,memory_order_acquire);
    uint32_t age=now-(stamp-1u);
    uint32_t receipt=usb_media_pause_sent();
    uint8_t action=accessory_automation_poll(&playback_accessory.state,now,
        input==1 && stamp && age<1000u,input==1 && stamp && age<150u,receipt);
    if(action&ACCESSORY_AUTOMATION_CANCEL)usb_media_cancel_automatic();
    if(input!=1)return;
    if(action&ACCESSORY_AUTOMATION_PAUSE)
        accessory_automation_sent(&playback_accessory.state,ACCESSORY_AUTOMATION_PAUSE,
            usb_media_enqueue(USB_MEDIA_AUTO_PAUSE),receipt,now);
    else if(action&ACCESSORY_AUTOMATION_PLAY)
        accessory_automation_sent(&playback_accessory.state,ACCESSORY_AUTOMATION_PLAY,
            usb_media_enqueue(USB_MEDIA_AUTO_PLAY),receipt,now);
}

static void playback_accessory_poll(void) {playback_accessory_update(true);}

void dongle_playback_accessory_event(const uint8_t *packet,uint16_t size) {
    if(!packet || size<6)return;
    playback_accessory_update(false);
    if(playback_accessory.admitted || (size==11 && packet[4]==9))
        accessory_automation_packet(&playback_accessory.state,packet,size,btstack_run_loop_get_time_ms());
    playback_accessory_poll();
}

void playback_accessory_cancel_automatic_media(void) {
    accessory_automation_cancel_media(&playback_accessory.state);
    usb_media_cancel_automatic();
}

static bool playback_accessory_duck(void) {
    return playback_accessory.admitted && accessory_automation_duck(&playback_accessory.state,
        btstack_run_loop_get_time_ms());
}

static void playback_accessory_signal(const void *src,uint16_t frames,bool floating) {
    bool any = false, program = false;
    for(unsigned i=0;i<2u*frames;++i) {
        if(floating) {
            float sample;memcpy(&sample,(const uint8_t *)src+i*sizeof(float),sizeof(sample));
            if (!isfinite(sample)) continue;
            any |= sample != 0;
            program = fabsf(sample) > 4.0f / 32768.0f;
        } else {
            int16_t sample;memcpy(&sample,(const uint8_t *)src+i*sizeof(sample),sizeof(sample));
            any |= sample != 0;
            program = sample < -4 || sample > 4;
        }
        if (program) break;
    }
    if (any) {
        uint32_t stamp = btstack_run_loop_get_time_ms() + 1u;
        if (!stamp) stamp = UINT32_MAX;
        atomic_store_explicit(&usb_signal_stamp, stamp, memory_order_release);
        if (program) atomic_store_explicit(&usb_program_stamp, stamp, memory_order_release);
    }
}

bool dongle_playback_defer_scan(void) {
    static uint8_t held_input;
    static uint32_t held_session, held_usb_revision, last_usb_signal;
    if (!held_input && !playback_game.requested) return false;

    uint8_t input = is_usb_streaming ? 1 : phone_active ? 2 : 0;
    uint32_t session = aacp_control_session();
    if (input != held_input || session != held_session ||
        (input == 1 && usb_playback_revision != held_usb_revision)) held_input = 0;

    if (input == 1) {
        // Windows may continue feeding zero PCM after Pause. Allow short silent passages.
        uint32_t stamp = atomic_load_explicit(&usb_signal_stamp, memory_order_acquire);
        if (stamp || !held_input) last_usb_signal = stamp;
        if (!last_usb_signal || btstack_run_loop_get_time_ms() - (last_usb_signal - 1u) >= 1000u) input = 0;
    }
    if (!input || !a2dp_is_connected_flag || !aacp_audio_owned() || playback_released ||
        playback_peer_suspended || profile_pending || call_active || call_opened || cur_codec != 4) {
        held_input = 0;
    } else if (!held_input && playback_game.requested && !playback_game.failed &&
               media_tracker.streaming && aacp_control_ready() && playback_game_eligible()) {
        // Starting the scan is audible too. Hold through Game Mode Off until this input pauses.
        held_input = input;
        held_session = session;
        held_usb_revision = usb_playback_revision;
    }
    return held_input != 0;
}

bool dongle_playback_priority_scope(uint16_t *media_cid) {
#if defined(AIRPODS_EXPERIMENTAL_RADIO0076) && AIRPODS_EXPERIMENTAL_RADIO0076
    // The stream can start before USB ownership or Game Mode is enabled.
    *media_cid = sc.local_stream_endpoint ? sc.local_stream_endpoint->l2cap_media_cid : 0;
    if (!*media_cid || !playback_game.requested || !usb_host_active() || phone_opened || phone_active ||
        call_active || call_opened || usb_mic_requested() || mic_prepare_open ||
        mic_recovery_pending || aacp_mic_requested() || aacp_mic_active() || profile_pending ||
        !a2dp_is_connected_flag || playback_released || !media_tracker.avdtp_cid || cur_codec != 4 ||
        memcmp(cur_active_device, (const uint8_t *)get_device_addr(), 6) ||
        !playback_game_eligible() || !aacp_control_ready() || !aacp_audio_owned())
        return false;
    return true;
#else
    *media_cid = 0;
    return false;
#endif
}

static void audio_slot_push_data(const void *src, uint16_t stereo_pair_count, bool floating) {
    critical_section_enter_blocking(&playback_pcm_lock);
    if (!phone_active && !call_active && src && stereo_pair_count) {
        playback_accessory_signal(src,stereo_pair_count,floating);
        usb_ingress_frames+=stereo_pair_count;
        if(inspect_enabled)playback_inspect_samples(&playback_inspect,src,2u*stereo_pair_count,floating,true,time_us_32());
        playback_pcm_push_data(&playback_pcm,src,stereo_pair_count,floating);
        if(playback_pcm.used>playback_metrics.pcm_peak)playback_metrics.pcm_peak=playback_pcm.used;
        bool running=playback_pcm.primed_on_wire && is_usb_streaming && media_tracker.streaming;
        uint32_t debt=4u*pcm_frame_samples;
        if(debt<PLAYBACK_USB_DEBT_LIMIT)debt=PLAYBACK_USB_DEBT_LIMIT;
        uint32_t limit=running?PLAYBACK_USB_RESERVE+debt:PLAYBACK_USB_MIN_FRAMES;
        // Give the worker one ELD packet to drain a burst before forcing stream recovery.
        if(running && cur_codec==4)limit+=PLAYBACK_ELD_FRAME_SAMPLES;
        uint32_t dropped=playback_pcm.dropped_pending+playback_pcm_limit(&playback_pcm,limit);
        playback_pcm.dropped_pending=0;
        playback_metrics.pcm_drops+=dropped;
        PB_ADD(running?TRACE_ACTIVE_DROP:TRACE_STARTUP_DROP,dropped);
        if(dropped && running) {
            usb_recovery_drops+=dropped;
            PB_EVENT(TRACE_DROP,dropped,playback_pcm.used);
        }
        PB_MAX(TRACE_PCM_MAX,playback_pcm.used);
    }
    critical_section_exit(&playback_pcm_lock);
}
void audio_slot_push_samples(const int16_t *src,uint16_t frames) {
    audio_slot_push_data(src,frames,false);
}
void audio_slot_push_float(const float *src,uint16_t frames) {
    audio_slot_push_data(src,frames,true);
}
void audio_slot_usb_reset(void) {
    playback_encoder_invalidate();
    critical_section_enter_blocking(&playback_pcm_lock);
    if(!phone_active && !call_active)playback_pcm_clear(&playback_pcm,audio_slot_capacity());
    playback_usb_clock_reset(&usb_clock);
    usb_recovery_drops=0;
    critical_section_exit(&playback_pcm_lock);
}

static uint16_t audio_pcm_read(uint16_t frames) {
    btstack_assert(cur_codec!=4 || frames==480);
    critical_section_enter_blocking(&playback_pcm_lock);
    bool external = phone_active || call_active;
    if (external && !phone_buffer_started && playback_pcm.priming == 0) {
        uint32_t prefill = call_active ? (uint32_t)frames + 960u : phone_clock_prefill(acc_num_simples);
        if (playback_pcm.used < prefill) {
            memset(&encoder_pcm, 0, (size_t)frames * 2 * (cur_codec==4?sizeof(int32_t):sizeof(int16_t)));
            critical_section_exit(&playback_pcm_lock);
            return frames;
        }
        phone_buffer_started = true;
    }
    bool correcting=cur_codec==4 && is_usb_streaming && !external &&
        !playback_pcm.priming && playback_adjust.running;
    uint16_t source_frames=frames;
    uint32_t rate=usb_clock.rate_q16*10u;
    if (correcting) {
        source_frames=playback_adjust_source_frames(&playback_adjust,usb_ingress_frames);
        // For a 480-frame unit this step consumes exactly source_frames at any inherited FIR phase.
        rate=(source_frames/3u)*4096u;
    }
    uint32_t resample_missing=0;
    if(is_usb_streaming && !external && playback_pcm.priming==0 &&
       playback_pcm_prepare_resample_fill(&playback_pcm,&encoder_resample,frames,rate,
                                          &resample_missing)) {
        // Never jump to undelayed raw PCM when fractional lookahead is short.
        // A shortage is audible source loss even if all whole output frames fit.
        playback_metrics.pcm_underflows+=resample_missing;
        bool active=playback_pcm.primed_on_wire;
        PB_ADD(active?TRACE_ACTIVE_ZERO:TRACE_STARTUP_ZERO,resample_missing);
        if(active && resample_missing)PB_EVENT(TRACE_ZERO,resample_missing,playback_pcm.used);
        if(playback_pcm.used<playback_trace.stats[TRACE_PCM_MIN])playback_trace.stats[TRACE_PCM_MIN]=playback_pcm.used;
        // Keep the FIR delay from the first real frame; only its clock feedback waits for priming.
        if(playback_pcm.primed_on_wire && !correcting) {
            uint32_t elapsed=btstack_run_loop_get_time_ms()-media_tracker.time_audio_data_sent;
            uint32_t due=media_tracker.samples_ready+(uint32_t)((uint64_t)elapsed*a2dp_sample_rate()/1000u);
            uint32_t queued=due>frames?due-frames:0;
            uint32_t remaining=playback_pcm.used>queued?playback_pcm.used-queued:0;
            bool frozen=queued>=frames || usb_recovery_drops!=0 || resample_missing!=0;
            playback_usb_clock_update(&usb_clock,remaining,frames,frozen);
            playback_trace.stats[TRACE_CLOCK_Q16]=usb_clock.rate_q16;
        }
        critical_section_exit(&playback_pcm_lock);
        if(cur_codec==4)playback_pcm_render_q31(&encoder_resample,encoder_pcm.q31);
        else playback_pcm_render(&encoder_resample,encoder_pcm.s16);
        return source_frames;
    }
    uint32_t priming=playback_pcm.priming<frames ? playback_pcm.priming : frames;
    uint32_t copied=cur_codec==4?playback_pcm_read_q31(&playback_pcm,encoder_pcm.q31,frames):
        playback_pcm_read(&playback_pcm,encoder_pcm.s16,frames);
    if (is_usb_streaming || external) playback_metrics.pcm_underflows+=frames-priming-copied;
    uint32_t missing=frames-priming-copied;
    bool active=(is_usb_streaming || external) && playback_pcm.primed_on_wire;
    PB_ADD(active?TRACE_ACTIVE_ZERO:TRACE_STARTUP_ZERO,missing);
    if(active && missing)PB_EVENT(TRACE_ZERO,missing,playback_pcm.used);
    if (active && missing && phone_active && !call_active && !is_usb_streaming)
        phone_sink_record_loss(false, missing, playback_pcm.used, media_tracker.samples_ready);
    if (external) {
        if (phone_buffer_started && playback_pcm.used < playback_trace.stats[TRACE_PCM_MIN])
            playback_trace.stats[TRACE_PCM_MIN] = playback_pcm.used;
        uint32_t factor = phone_clock_update(&phone_clock, playback_pcm.used, frames);
        btstack_resample_set_factor(&phone_resampler, factor);
        playback_trace.stats[TRACE_CLOCK_Q16] = factor;
    }
    critical_section_exit(&playback_pcm_lock);
    return frames;
}

bool check_is_streaming(){
    return is_streaming;
}
bool dongle_playback_transitioning(void) {
    return playback_idle_phase == PLAYBACK_SUSPENDING || playback_idle_phase == PLAYBACK_STARTING ||
        playback_idle_phase == PLAYBACK_CLAIMING ||
        playback_adjust_busy(&playback_adjust);
}

bool get_usb_streaming(void) { return is_usb_streaming; }

void set_usb_streaming(bool flag){
    if (!flag && !usb_mic_requested()) usb_media_cancel_volume();
    if (flag && (call_opened || call_active)) bridge_call_stop();
    if(is_usb_streaming!=flag) {
        atomic_store_explicit(&usb_program_stamp, 0, memory_order_release);
        playback_encoder_invalidate();
        critical_section_enter_blocking(&playback_pcm_lock);
        if (flag) {
            ++usb_playback_revision;
            atomic_store_explicit(&usb_signal_stamp, 0, memory_order_release);
        }
        playback_usb_clock_reset(&usb_clock);
        playback_pcm.fraction=0;
        usb_recovery_drops=0;
        critical_section_exit(&playback_pcm_lock);
    }
    if (is_usb_streaming && !flag && !phone_active && !call_active) {
        critical_section_enter_blocking(&playback_pcm_lock);
        playback_pcm_clear(&playback_pcm, audio_slot_capacity());
        critical_section_exit(&playback_pcm_lock);
    }
    is_usb_streaming = flag;
}

bool get_allow_switch_slot(){
    return allow_switch_slot;
}


static int fill_sbc_audio_buffer(a2dp_media_sending_context_t * context){
    if (playback_encoder_busy()) return 0;
    int total_num_bytes_read = 0;
    unsigned int num_audio_samples_per_sbc_buffer = btstack_sbc_encoder_num_audio_frames();

    while (context->samples_ready >= num_audio_samples_per_sbc_buffer &&
           context->codec_num_frames < 15 &&
           (context->max_media_payload_size - context->codec_storage_count - 1) >= btstack_sbc_encoder_sbc_buffer_length()){

        audio_pcm_read((uint16_t)num_audio_samples_per_sbc_buffer);
        playback_duck_s16(&sbc_duck,encoder_pcm.s16,num_audio_samples_per_sbc_buffer,
            (uint32_t)a2dp_sample_rate(),playback_accessory_duck());
        uint32_t started=time_us_32();
        if(!context->codec_storage_count)packet_oldest_us=started;
        btstack_sbc_encoder_process_data(encoder_pcm.s16);
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
    printf("[PLAYBACK] aborted: codec=%u phase=%u\n", cur_codec, playback_idle_phase);
    bt_hci_priority_transition(PRIORITY_CLOSE);
    a2dp_demo_timer_stop(context);
    finish_scan_avdtp_codec = false;
    aacp_disconnect();
    a2dp_source_disconnect(context->avdtp_cid);
    return -1;
}

static bool aaceld_encoder_init(void) {
    if (playback_encoder_busy()) {
        if (encoder_job.deferred_size) return true;
        playback_encoder_invalidate();
        encoder_job.reset_pending = true;
        encoder_job.deferred_session = aacp_control_session();
        encoder_job.reset_cid = media_tracker.avdtp_cid;
        memcpy(encoder_job.deferred_peer, cur_active_device, sizeof(encoder_job.deferred_peer));
        return true;
    }
    if (handleAAC != NULL) aacEncClose(&handleAAC);
    handleAAC = NULL;
    if (aacEncOpen(&handleAAC, 0x01, 2) != AACENC_OK) return false;
    const struct { AACENC_PARAM key; UINT value; } params[] = {
        {AACENC_AOT, 39}, {AACENC_BITRATE, playback_bitrate.current ? playback_bitrate.current : aaceld_bitrate},
        {AACENC_SAMPLERATE, 48000}, {AACENC_CHANNELMODE, MODE_2},
        {AACENC_GRANULE_LENGTH, PLAYBACK_ELD_FRAME_SAMPLES},
        {AACENC_SBR_MODE, 0}, {AACENC_BITRATEMODE, playback_options.rate_mode ? 5u : 0u},
        {AACENC_PEAK_BITRATE, playback_bitrate.current ? playback_bitrate.current : aaceld_bitrate}, {AACENC_AFTERBURNER, 0},
        {AACENC_TRANSMUX, TT_MP4_RAW}
    };
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); ++i) {
        if (aacEncoder_SetParam(handleAAC, params[i].key, params[i].value) != AACENC_OK) goto fail;
    }
    if (aacEncoder_SetParam(handleAAC,AACENC_BANDWIDTH,
        aaceld_bandwidth(aaceld_bitrate,playback_options.rate_mode))!=AACENC_OK) goto fail;
    if (aacEncEncode(handleAAC, NULL, NULL, NULL, NULL) != AACENC_OK ||
        aacEncInfo(handleAAC, &aacinf) != AACENC_OK ||
        playback_aac_frame_samples(48000, aacinf.inputChannels, aacinf.frameLength, true) == 0 ||
        aacEncoder_GetParam(handleAAC,AACENC_PEAK_BITRATE)!=(playback_bitrate.current ? playback_bitrate.current : aaceld_bitrate) ||
        aacEncoder_GetParam(handleAAC,AACENC_BITRATEMODE)!=(playback_options.rate_mode ?
            ((playback_bitrate.current ? playback_bitrate.current : aaceld_bitrate)<192000 ? 4u : 5u) : 0u)) goto fail;
    ++playback_inspect.stats[11];
    playback_inspect.stats[10]=0;
    return true;
fail:
    aacEncClose(&handleAAC);
    handleAAC = NULL;
    return false;
}

// A failed update is rolled back before another PCM unit can enter the encoder.
static int playback_eld_rate_update(uint32_t previous, uint32_t target) {
    if (!aaceld_variable_allowed || !handleAAC || previous < 128000u || previous > aaceld_bitrate ||
        target < 128000u || target > aaceld_bitrate ||
        (playback_options.rate_mode && (target % 800u || previous % 800u))) return 1;
    if (previous == target) return 0;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        uint32_t bitrate = attempt ? previous : target;
        AACENC_InfoStruct info;
        if (aacEncoder_SetParam(handleAAC, AACENC_BITRATE, bitrate) != AACENC_OK ||
            aacEncoder_SetParam(handleAAC, AACENC_PEAK_BITRATE, bitrate) != AACENC_OK ||
            aacEncoder_SetParam(handleAAC, AACENC_BITRATEMODE, playback_options.rate_mode ? 5u : 0u) != AACENC_OK ||
            aacEncEncode(handleAAC, NULL, NULL, NULL, NULL) != AACENC_OK ||
            aacEncInfo(handleAAC, &info) != AACENC_OK) continue;
        unsigned mode = aacEncoder_GetParam(handleAAC, AACENC_BITRATEMODE);
        if (info.inputChannels != 2 || info.frameLength != PLAYBACK_ELD_FRAME_SAMPLES ||
            info.confSize != aacinf.confSize || info.confSize > sizeof(info.confBuf) ||
            memcmp(info.confBuf, aacinf.confBuf, info.confSize) ||
            aacEncoder_GetParam(handleAAC, AACENC_SAMPLERATE) != 48000u ||
            aacEncoder_GetParam(handleAAC, AACENC_PEAK_BITRATE) != bitrate ||
            (playback_options.rate_mode ? (mode < 1 || mode > 5) : mode != 0)) continue;
        aacinf = info;
        return attempt ? 1 : 0;
    }
    return -1;
}

static bool playback_eld_steady_route(void) {
    return cur_codec == 4 && handleAAC && a2dp_is_connected_flag && media_tracker.streaming &&
        (is_usb_streaming || phone_active) && !call_active && !call_opened &&
        aacp_audio_owned() && playback_idle_phase == PLAYBACK_RUNNING &&
        !profile_pending && !a2dp_connect_pending && !aacp_mic_requested() && !aacp_mic_active() &&
        !mic_prepare_open && !mic_recovery_pending && !phone_output_recovery && !suspend_recovery_pending &&
        !playback_transport_stale && !playback_adjust_busy(&playback_adjust) &&
        !playback_game.waiting_tx && !playback_game.waiting_report && !playback_game.restore_failed &&
        (playback_game.phase == GAME_OFF || playback_game.phase == GAME_CONFIRMED) &&
        !usb_game_verification.needed && !usb_game_verification.queued;
}
static bool playback_bitrate_eligible(void) {
    return aaceld_variable_allowed && playback_eld_steady_route() && playback_pcm_warm();
}
void dongle_playback_airpods_credits(uint32_t gap_ms, uint16_t packets) {
    static uint32_t last_epoch, last_session, last_buffer_change;
    static bool steady_before;
    uint32_t epoch = atomic_load_explicit(&encoder_job.epoch, memory_order_relaxed);
    uint32_t session = aacp_control_session();
    // HCI starts a new ACL with gap zero. The media epoch changes on pause, restart and PCM reset.
    bool same_scope = gap_ms && steady_before && epoch == last_epoch &&
        session == last_session && playback_game.sent_since == last_buffer_change;
    bool late = gap_ms >= 40;
    bool steady = aacp_control_ready() && playback_eld_steady_route();
    // Warmth persists within the media epoch. Lock PCM only to arm or confirm an exceptional gap.
    if (steady && (!same_scope || late)) steady = playback_pcm_warm();
    if (late && steady && same_scope)
        PB_EVENT(TRACE_CREDIT_GAP, gap_ms, packets);
    last_epoch = epoch;
    last_session = session;
    last_buffer_change = playback_game.sent_since;
    steady_before = steady;
}
static void playback_bitrate_game_packet(uint32_t wait_us, uint32_t gap_us) {
    if (!playback_bitrate.active || playback_bitrate.frozen || playback_bitrate.urgent ||
        playback_bitrate.target != playback_bitrate.current || !is_usb_streaming || phone_active ||
        !bitrate_game_requested || bitrate_input != 1u || !playback_game.requested ||
        !playback_game.low_latency_confirmed || playback_game.phase != GAME_CONFIRMED ||
        playback_game.target != 60u) return;

    if (gap_us && gap_us <= 20000u && wait_us <= 10000u) {
        playback_bitrate.near_latched = false;
        return;
    }
    uint32_t now = btstack_run_loop_get_time_ms();
    if (playback_bitrate.near_latched) {
        // A burst straddling a poll is still one episode. A full second of ongoing stalls is fresh evidence.
        if ((uint32_t)(now - playback_bitrate.near_since) < 1000u) {
            playback_bitrate.near_seen = true;
            return;
        }
        playback_bitrate.near_latched = false;
    }
    // A 60 ms receiver target has at most one 10 ms ELD unit left at a 50 ms gap.
    // Require real can-send backpressure. A late encoder or a player pause alone is insufficient.
    if (wait_us < 30000u || wait_us > 60000u || gap_us < 50000u ||
        !playback_bitrate_eligible()) return;
    playback_bitrate.near_since = now;
    playback_bitrate.near_seen = playback_bitrate.near_latched = true;
    if (playback_bitrate.near_episodes < 2u) ++playback_bitrate.near_episodes;
}
static void playback_bitrate_transport_stalled(void) {
    if (!playback_bitrate_eligible()) return;
    bt_hci_radio_diagnostics_t radio;
    if (bt_hci_radio_diagnostics(&radio) && radio.epoch == bitrate_radio_epoch &&
        bitrate_input == (is_usb_streaming ? 1u : phone_active ? 2u : 0u) &&
        bitrate_game_requested == playback_game.requested)
        playback_bitrate_stalled(&playback_bitrate);
}
static bool playback_bitrate_apply(a2dp_media_sending_context_t *context) {
    if (context->codec_storage_count || playback_bitrate.target == playback_bitrate.current ||
        (playback_bitrate.frozen && !playback_bitrate.urgent) || playback_bitrate.fault || !playback_bitrate_eligible() ||
        bitrate_discontinuities != playback_trace.stats[TRACE_ACTIVE_DROP] +
            playback_trace.stats[TRACE_ACTIVE_ZERO] + playback_trace.stats[TRACE_RECOVERIES]) return true;
    int result = playback_eld_rate_update(playback_bitrate.current, playback_bitrate.target);
    playback_bitrate_applied(&playback_bitrate, btstack_run_loop_get_time_ms(), result == 0);
    return result >= 0;
}
static void playback_bitrate_poll(uint32_t now) {
    bool active = a2dp_is_connected_flag && media_tracker.streaming && aacp_audio_owned() &&
        (is_usb_streaming || phone_active) && !call_active && !call_opened &&
        !aacp_mic_requested() && !aacp_mic_active() && !mic_prepare_open && !mic_recovery_pending &&
        playback_idle_phase == PLAYBACK_RUNNING;
    bt_hci_radio_active(active);
    bt_hci_radio_diagnostics_t radio;
    bool known = bt_hci_radio_diagnostics(&radio);
    bool eligible = playback_bitrate_eligible();
    uint32_t discontinuities = playback_trace.stats[TRACE_ACTIVE_DROP] +
        playback_trace.stats[TRACE_ACTIVE_ZERO] + playback_trace.stats[TRACE_RECOVERIES];
    uint8_t input = is_usb_streaming ? 1u : phone_active ? 2u : 0u;
    bool changed = input != bitrate_input || bitrate_game_requested != playback_game.requested;
    bool same_epoch = known && bitrate_radio_epoch == radio.epoch;
    bool scope_changed = known && !same_epoch;
    if (scope_changed || !aaceld_variable_allowed || cur_codec != 4 || !a2dp_is_connected_flag ||
        changed || !input || !aacp_audio_owned() || call_active || call_opened ||
        aacp_mic_requested() || aacp_mic_active() || mic_prepare_open || mic_recovery_pending ||
        playback_game.restore_failed || profile_pending || a2dp_connect_pending ||
        (!known && playback_bitrate.urgent)) {
        playback_bitrate.urgent = false; playback_bitrate.target = playback_bitrate.current;
    } else if (same_epoch && active && playback_pcm_warm() &&
        !playback_adjust_busy(&playback_adjust) && !playback_game.waiting_tx && !playback_game.waiting_report &&
        (playback_game.phase == GAME_OFF || playback_game.phase == GAME_CONFIRMED) &&
        radio.flush_events != playback_bitrate.last_flushes &&
        (playback_transport_stale || discontinuities != bitrate_discontinuities)) {
        // Radio-backed decreases survive recovery; generic PCM starvation never bypasses warmup.
        playback_bitrate_stalled(&playback_bitrate);
    }
    bitrate_input = input; bitrate_game_requested = playback_game.requested;
    if (!same_epoch || changed || discontinuities != bitrate_discontinuities)
        eligible = false;
    if (known) bitrate_radio_epoch = radio.epoch;
    bitrate_discontinuities = discontinuities;
    playback_bitrate_observe(&playback_bitrate, now, eligible,
        known ? radio.flush_events : playback_bitrate.last_flushes, bitrate_pressure);
}

static void playback_clock_accrue(a2dp_media_sending_context_t *,uint32_t);
static void playback_clock_debit(a2dp_media_sending_context_t *,uint32_t,uint8_t);

static int fill_aac_audio_buffer(a2dp_media_sending_context_t *context) {
    if (playback_encoder_busy()) return 0;
    if(!aacp_audio_owned()) {
        playback_continuity_reset(&phone_continuity);
        return 0;
    }
    uint32_t now = btstack_run_loop_get_time_ms();
    if (playback_adjust.running) {
        int32_t late = (int32_t)(now - playback_adjust.next_unit_ms);
        if (late < 0) return 0;
        uint8_t interval = playback_adjust.interval;
        unsigned budget = PLAYBACK_ADJUST_CATCH_UP_UNITS * (interval > 10 ? interval : 10);
        if ((uint32_t)late >= budget) {
            if (interval == 11) {
                playback_adjust_cancel_stream(context,now,interval);
                playback_adjust.failed = true;
                return 0;
            }
            // The fast clock still has to earn its lead from actual sends.
            playback_adjust.next_unit_ms = now;
        }
    }
    (void)playback_encoder_packet_current();
    if (cur_codec == 4 && !playback_bitrate_apply(context)) return playback_encoder_failed(context);
    if(playback_adjust.running)playback_clock_accrue(context,btstack_run_loop_get_time_ms());
    unsigned frames = acc_num_simples;
    unsigned header = cur_codec == 4 ? PLAYBACK_ELD_HEADER_SIZE : 0;
    unsigned minimum = cur_codec == 4 ? playback_eld_au_ceiling(aaceld_bitrate,
        playback_options.rate_mode != 0) + header : 50u;
    if (!handleAAC) return playback_encoder_failed(context);
    if (context->samples_ready < frames ||
        context->max_media_payload_size - context->codec_storage_count < (int)minimum) return 0;
    btstack_assert(frames <= AUDIO_SLOT_MAX_SAMPLES);
    encoder_job.unit_started = time_us_32();
    critical_section_enter_blocking(&playback_pcm_lock);
    if (context->codec_storage_count && !playback_encoder_scope_current()) playback_encoder_discard(false);
    encoder_job.session = aacp_control_session();
    encoder_job.usb_revision = usb_playback_revision;
    encoder_job.epoch_at_prepare = atomic_load_explicit(&encoder_job.epoch, memory_order_relaxed);
    encoder_job.input = (is_usb_streaming ? 1u : 0u) | (phone_active ? 2u : 0u) | (call_active ? 4u : 0u);
    encoder_job.handle = handleAAC;
    encoder_job.codec = cur_codec;
    encoder_job.accessory_epoch=playback_accessory.epoch;
    encoder_job.duck=playback_accessory_duck();
    encoder_job.sample_rate=(uint16_t)a2dp_sample_rate();
    critical_section_exit(&playback_pcm_lock);
    runtime_watch_set(WATCH_PCM);
    uint8_t interval=playback_adjust_interval(&playback_adjust);
    uint16_t source_frames=audio_pcm_read((uint16_t)frames);
    critical_section_enter_blocking(&playback_pcm_lock);
    bool current=playback_encoder_scope_current();
    if (current && inspect_enabled) {
        critical_section_exit(&playback_pcm_lock);
        if(cur_codec==4)playback_inspect_samples_q31(&playback_inspect,encoder_pcm.q31,
            frames*aacinf.inputChannels,time_us_32());
        else playback_inspect_samples(&playback_inspect,encoder_pcm.s16,
            frames*aacinf.inputChannels,false,false,time_us_32());
        critical_section_enter_blocking(&playback_pcm_lock);
        current=playback_encoder_scope_current();
    }
    if(!current) {
        playback_encoder_discard(false);
        critical_section_exit(&playback_pcm_lock);
        return 0;
    }
    encoder_job.samples = (uint16_t)(frames * aacinf.inputChannels);
    encoder_job.offset = context->codec_storage_count + header;
    encoder_job.capacity = (uint16_t)(context->max_media_payload_size - encoder_job.offset);
    // The timer can accrue more debt while encoding; debit this unit exactly once.
    if (interval!=10) playback_clock_accrue(context,btstack_run_loop_get_time_ms());
    playback_clock_debit(context,source_frames,interval);
    // Keep phase for bounded catch-up. The worker still prepares one unit at a time.
    if (playback_adjust.running) playback_adjust.next_unit_ms += playback_adjust.interval;
    if (!context->codec_storage_count) packet_oldest_us = encoder_job.unit_started;
    atomic_store_explicit(&encoder_job.state, ENCODE_PREPARED, memory_order_release);
    critical_section_exit(&playback_pcm_lock);
    return (int)frames;
}

// Main thread only, after releasing the CYW43 lock. Lifecycle requests pin this handle.
void dongle_playback_encode_run(void) {
    if (atomic_load_explicit(&encoder_job.state, memory_order_acquire) != ENCODE_PREPARED) return;
    atomic_store_explicit(&encoder_job.state, ENCODE_RUNNING, memory_order_release);
    encoder_job.error = AACENC_OK; encoder_job.bytes = encoder_job.consumed = 0;
    encoder_job.encode_us = 0;
    if (encoder_job.epoch_at_prepare == atomic_load_explicit(&encoder_job.epoch, memory_order_relaxed)) {
        if(encoder_duck_epoch!=encoder_job.accessory_epoch) {
            playback_duck_reset(&encoder_duck);encoder_duck_epoch=encoder_job.accessory_epoch;
        }
        if(encoder_job.codec==4)playback_duck_q31(&encoder_duck,encoder_pcm.q31,
            encoder_job.samples/2u,encoder_job.sample_rate,encoder_job.duck);
        else playback_duck_s16(&encoder_duck,encoder_pcm.s16,
            encoder_job.samples/2u,encoder_job.sample_rate,encoder_job.duck);
        void *input_data = encoder_job.codec==4?(void *)encoder_pcm.q31:(void *)encoder_pcm.s16;
        void *output_data = media_tracker.codec_storage + encoder_job.offset;
        int input_id = IN_AUDIO_DATA, output_id = OUT_BITSTREAM_DATA;
        int input_element = encoder_job.codec==4?sizeof(int32_t):sizeof(int16_t), output_element = 1;
        int input_size = encoder_job.samples * input_element, output_size = encoder_job.capacity;
        AACENC_BufDesc input = {1, &input_data, &input_id, &input_size, &input_element};
        AACENC_BufDesc output = {1, &output_data, &output_id, &output_size, &output_element};
        AACENC_InArgs arguments = {0}; AACENC_OutArgs result = {0};
        arguments.numInSamples = encoder_job.samples;
        uint32_t started = time_us_32();
        runtime_watch_set(WATCH_ENCODER);
        encoder_job.error = aacEncEncode(encoder_job.handle, &input, &output, &arguments, &result);
        encoder_job.encode_us = time_us_32() - started;
        ++playback_work_metrics.aac_calls;
        playback_work_metrics.aac_us += encoder_job.encode_us;
        encoder_job.bytes = result.numOutBytes;
        encoder_job.consumed = result.numInSamples;
    }
    atomic_store_explicit(&encoder_job.state, ENCODE_DONE, memory_order_release);
}

static void playback_encoder_close(void) {
    playback_encoder_invalidate();
    encoder_job.reset_pending = false;
    encoder_job.deferred_size = 0;
    if (playback_encoder_busy()) { encoder_job.close_pending = true; return; }
    if (handleAAC) aacEncClose(&handleAAC);
}

static bool playback_encoder_defer_configuration(const uint8_t *packet, uint16_t size) {
    if (!playback_encoder_busy()) return false;
    a2dp_demo_timer_stop(&media_tracker);
    encoder_job.reset_pending = false;
    if (size > sizeof(encoder_job.deferred_event)) {
        encoder_job.deferred_size = 0;
        playback_encoder_failed(&media_tracker);
    } else {
        memcpy(encoder_job.deferred_event, packet, size);
        encoder_job.deferred_size = (uint8_t)size;
        encoder_job.deferred_epoch = atomic_load_explicit(&encoder_job.epoch, memory_order_relaxed);
        encoder_job.deferred_session = aacp_control_session();
        memcpy(encoder_job.deferred_peer, cur_active_device, sizeof(encoder_job.deferred_peer));
    }
    return true;
}

static bool playback_encoder_scope_current(void) {
    return encoder_job.epoch_at_prepare == atomic_load_explicit(&encoder_job.epoch, memory_order_relaxed) &&
        encoder_job.handle == handleAAC && encoder_job.codec == cur_codec &&
        encoder_job.session == aacp_control_session() && encoder_job.usb_revision == usb_playback_revision &&
        encoder_job.input == ((is_usb_streaming ? 1u : 0u) | (phone_active ? 2u : 0u) | (call_active ? 4u : 0u)) &&
        a2dp_is_connected_flag && media_tracker.streaming && aacp_audio_owned();
}

// Caller holds the PCM exclusion; every retained AU is still unsent while a job owns the packet.
static void playback_encoder_discard(bool current_job) {
    uint32_t discarded = media_tracker.codec_num_frames + (current_job ? 1u : 0u);
    uint32_t unsent_bytes = media_tracker.codec_storage_count;
    if (media_tracker.streaming && cur_codec == 4 && encoder_job.codec == 4 && encoder_job.handle == handleAAC)
        aaceld_frame_sequence = (aaceld_frame_sequence - media_tracker.codec_num_frames) & 0xfffu;
    playback_work_cancel_wait(&media_tracker);
    media_tracker.codec_storage_count = media_tracker.codec_num_frames = media_tracker.codec_ready_to_send = 0;
    media_tracker.send_wait_start = 0;
    packet_oldest_us = media_last_sent_us = 0;
    playback_continuity_reset(&phone_continuity);
    playback_work_pending = true;
    ++encoder_job.cancelled;
    PB_EVENT(TRACE_DROP, discarded * (encoder_job.samples / 2u), unsent_bytes);
}

static bool playback_encoder_packet_current(void) {
    if ((cur_codec != 2 && cur_codec != 4) || !media_tracker.codec_storage_count) return true;
    critical_section_enter_blocking(&playback_pcm_lock);
    bool current = playback_encoder_scope_current();
    if (!current) playback_encoder_discard(false);
    critical_section_exit(&playback_pcm_lock);
    return current;
}

static bool playback_aac_packet_submit(a2dp_media_sending_context_t *context, uint32_t now) {
    if (playback_encoder_busy() || context->codec_ready_to_send || !context->codec_storage_count) return false;
    if (!playback_encoder_packet_current()) return false;
    unsigned limit = playback_game.changed && playback_game.target == 60 ? 2u : 3u;
    if (phone_opened || call_opened) limit = 2;
    else if (is_usb_streaming) limit = 1;
    bool full = cur_codec == 2 || (cur_codec == 4 &&
        (context->codec_num_frames >= limit ||
         context->max_media_payload_size - context->codec_storage_count <
         (int)(playback_eld_au_ceiling(aaceld_bitrate, playback_options.rate_mode != 0) + 4)));
    if (!full) return false;
    context->codec_ready_to_send = 1;
    context->send_wait_start = now;
    a2dp_source_stream_endpoint_request_can_send_now(context->avdtp_cid, context->local_seid);
    return true;
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
void dongle_playback_encode_complete(void) {
    if (atomic_load_explicit(&encoder_job.state, memory_order_acquire) != ENCODE_DONE) return;
    ++playback_work_metrics.aac_jobs;
    playback_work_metrics.aac_job_us += time_us_32() - encoder_job.unit_started;
    bool failed = false;
    // USB reset/alternate IRQs share this exclusion; an old job cannot publish after reset.
    critical_section_enter_blocking(&playback_pcm_lock);
    bool current = playback_encoder_scope_current();
    atomic_store_explicit(&encoder_job.state, ENCODE_IDLE, memory_order_release);
    if (current) {
        failed = encoder_job.error != AACENC_OK || encoder_job.consumed != encoder_job.samples ||
            encoder_job.bytes < 0 || encoder_job.bytes > encoder_job.capacity ||
            (cur_codec == 4 && playback_options.rate_mode &&
             (unsigned)encoder_job.bytes > playback_bitrate.current / 800u);
        if (!failed && encoder_job.bytes) {
            if (cur_codec == 4) failed = !playback_eld_append(media_tracker.codec_storage,
                media_tracker.max_media_payload_size, &media_tracker.codec_storage_count,
                &aaceld_frame_sequence, (size_t)encoder_job.bytes);
            else media_tracker.codec_storage_count += (uint16_t)encoder_job.bytes;
            if (!failed) { ++media_tracker.codec_num_frames; ++playback_metrics.encoded_units; }
        }
        playback_work_pending = true;
    } else playback_encoder_discard(true);
    critical_section_exit(&playback_pcm_lock);
    if (encoder_job.encode_us) {
        playback_encode_measured(time_us_32() - encoder_job.encode_us);
        PB_MAX(TRACE_UNIT_US, time_us_32() - encoder_job.unit_started);
    }
    if (failed) playback_encoder_failed(&media_tracker);
    if (encoder_job.close_pending) {
        encoder_job.close_pending = false;
        if (handleAAC) aacEncClose(&handleAAC);
    }
    if (encoder_job.deferred_size) {
        uint8_t event[sizeof(encoder_job.deferred_event)];
        uint16_t size = encoder_job.deferred_size;
        memcpy(event, encoder_job.deferred_event, size); encoder_job.deferred_size = 0;
        if (a2dp_is_connected_flag && size >= 5 && little_endian_read_16(event, 3) == media_tracker.avdtp_cid &&
            encoder_job.deferred_epoch == atomic_load_explicit(&encoder_job.epoch, memory_order_relaxed) &&
            encoder_job.deferred_session == aacp_control_session() &&
            !memcmp(encoder_job.deferred_peer, cur_active_device, sizeof(encoder_job.deferred_peer)))
            packet_handler(HCI_EVENT_PACKET, 0, event, size);
    } else if (encoder_job.reset_pending) {
        encoder_job.reset_pending = false;
        if (cur_codec == 4 && a2dp_is_connected_flag && encoder_job.reset_cid == media_tracker.avdtp_cid &&
            encoder_job.deferred_session == aacp_control_session() &&
            !memcmp(encoder_job.deferred_peer, cur_active_device, sizeof(encoder_job.deferred_peer)) &&
            !aaceld_encoder_init())
            playback_encoder_failed(&media_tracker);
    }
    if (current && !failed) (void)playback_aac_packet_submit(&media_tracker, btstack_run_loop_get_time_ms());
}

uint32_t dongle_playback_encode_cancelled(void) { return encoder_job.cancelled; }

#endif

static void a2dp_demo_timer_stop(a2dp_media_sending_context_t *context);
static void playback_transport_recover(a2dp_media_sending_context_t *context) {
    if (!context->streaming || suspend_recovery_pending || phone_output_recovery) return;
    bool keep_phone = phone_active && bridge_phone_ready() && !aacp_mic_active() &&
        !profile_pending && !a2dp_connect_pending;
    ++playback_metrics.recoveries;
    a2dp_demo_timer_stop(context);
    if (keep_phone) {
        phone_output_recovery = true;
        phone_output_recovery_cid = context->avdtp_cid;
        phone_output_recovery_since = btstack_run_loop_get_time_ms();
        memcpy(phone_output_recovery_peer, cur_active_device, 6);
    }
    // The receiver must confirm the boundary before either codec is reset.
    if (avdtp_source_suspend(context->avdtp_cid,context->local_seid)!=ERROR_CODE_SUCCESS) {
        phone_output_recovery = false;
        aacp_disconnect();
        a2dp_source_disconnect(context->avdtp_cid);
        return;
    }
    recovery_reset_encoder=cur_codec==4;
    suspend_recovery_pending=true;
    btstack_run_loop_set_timer_handler(&suspend_recovery_timer,suspend_recovery_handler);
    btstack_run_loop_set_timer(&suspend_recovery_timer,SUSPEND_RECOVERY_TIMEOUT_MS);
    btstack_run_loop_add_timer(&suspend_recovery_timer);
}

static bool playback_phone_recovery_poll(void) {
    if (phone_output_recovery) {
        if (!phone_output_recovery_owned()) {
            const uint8_t *peer = (const uint8_t *)get_device_addr();
            if (media_tracker.avdtp_cid != phone_output_recovery_cid || !peer ||
                memcmp(peer, phone_output_recovery_peer, 6) ||
                memcmp(cur_active_device, phone_output_recovery_peer, 6)) {
                suspend_recovery_pending = recovery_reset_encoder = false;
                btstack_run_loop_remove_timer(&suspend_recovery_timer);
            }
            phone_output_recovery = false;
        }
        else if (!bridge_phone_recovering()) {
            playback_encoder_failed(&media_tracker);
            return true;
        }
        else if (bridge_phone_ready()) phone_output_recovery = false;
    }
    if ((phone_active || call_active) && playback_transport_stale) playback_transport_recover(&media_tracker);
    return false;
}

static void playback_audio_work(uint32_t now) {
    if (playback_encoder_busy()) return;
    static bool phone_overrun;
    a2dp_media_sending_context_t *context=&media_tracker;
    if(!context->streaming) {phone_overrun=false;return;}
    bool external = phone_active || call_active;
    if(!external || context->samples_ready<acc_num_simples)phone_overrun=false;
    bool recover=false;
    critical_section_enter_blocking(&playback_pcm_lock);
    uint32_t limit=4u*acc_num_simples;
    if(limit<PLAYBACK_USB_DEBT_LIMIT)limit=PLAYBACK_USB_DEBT_LIMIT;
    limit=limit*10u/playback_adjust_interval(&playback_adjust);
    // The USB FIFO already retains one extra ELD unit. Let a progressing stream encode it.
    if (context->samples_ready>limit && is_usb_streaming && !external && cur_codec==4 &&
        playback_pcm.primed_on_wire && !playback_adjust_busy(&playback_adjust) &&
        playback_pcm.used>=context->samples_ready && media_last_sent_us &&
        time_us_32()-media_last_sent_us<60000u)
        limit+=PLAYBACK_ELD_FRAME_SAMPLES;
    if(external && playback_pcm.dropped_pending) {
        // Credit physical losses only against overdue work that would otherwise
        // be trimmed again below. Cancelling healthy or future deadlines keeps
        // an overflowing FIFO full by preventing the reads that would drain it.
        uint32_t excess=!playback_pcm.priming && playback_pcm.primed_on_wire && phone_buffer_started &&
            context->samples_ready>limit?context->samples_ready-limit:0;
        uint32_t skipped=playback_pcm.dropped_pending<excess?playback_pcm.dropped_pending:excess;
        context->samples_ready-=skipped;
        playback_pcm.dropped_pending=0;
        PB_ADD(TRACE_DEBT_DROP,skipped);
        if(skipped) {
            if(!phone_overrun) {PB_ADD(TRACE_RECOVERIES,1);phone_overrun=true;}
            PB_EVENT(TRACE_RECOVERY,skipped,context->samples_ready);
        }
    }
    if(is_usb_streaming && !external &&
       (usb_recovery_drops || context->samples_ready>limit || playback_transport_stale)) {
        playback_trace_usb_recovery(&playback_trace,usb_recovery_drops!=0,
                                    context->samples_ready>limit,playback_transport_stale);
        uint32_t trimmed=playback_pcm_limit(&playback_pcm,PLAYBACK_USB_MIN_FRAMES);
        uint32_t skipped=usb_recovery_drops+trimmed;
        playback_metrics.pcm_drops+=trimmed;
        PB_ADD(TRACE_ACTIVE_DROP,trimmed);
        PB_ADD(TRACE_DEBT_DROP,context->samples_ready);
        PB_ADD(TRACE_RECOVERIES,1);
        PB_EVENT(TRACE_RECOVERY,skipped,context->samples_ready);
        context->samples_ready=0;
        context->acc_num_missed_samples=0;
        context->sample_debt=0;
        context->time_audio_data_sent=now;
        usb_recovery_drops=0;
        playback_transport_stale=false;
        playback_pcm.fraction=0;
        playback_usb_clock_reset(&usb_clock);
        if(playback_adjust_busy(&playback_adjust))playback_game.failed=true;
        playback_adjust_cancel(&playback_adjust);
        recover=true;
    } else if(external && (!phone_active || call_active) && context->samples_ready>limit) {
        uint32_t excess=context->samples_ready-limit;
        uint32_t keep=playback_pcm.used>excess?playback_pcm.used-excess:0;
        uint32_t skipped=playback_pcm_limit(&playback_pcm,keep);
        // Never forget phone source time while retaining the corresponding PCM.
        // If decoding is late, keep the remaining debt until its PCM arrives.
        context->samples_ready-=skipped;
        playback_metrics.pcm_drops+=skipped;
        PB_ADD(TRACE_ACTIVE_DROP,skipped);
        PB_ADD(TRACE_DEBT_DROP,skipped);
        if(skipped) {
            if(!phone_overrun) {PB_ADD(TRACE_RECOVERIES,1);phone_overrun=true;}
            PB_EVENT(TRACE_RECOVERY,skipped,context->samples_ready);
        }
    } else if(!external && context->samples_ready>limit)context->samples_ready=limit;
    // Retained phone PCM can repay a short radio stall; only actual FIFO loss retires its deadlines.
    critical_section_exit(&playback_pcm_lock);
    if(recover) { playback_transport_recover(context); return; }
    if(playback_work_pending || context->codec_ready_to_send==2 ||
       (!context->codec_ready_to_send && context->samples_ready>=acc_num_simples)) {
        playback_work_pending=false;
        playback_prepare_packet(context,now);
    }
}

static void a2dp_demo_timer_stop(a2dp_media_sending_context_t * context);

static void playback_clock_store(a2dp_media_sending_context_t *context,int64_t balance) {
    context->sample_debt=balance<0?(uint64_t)-balance:0;
    uint64_t credit=balance>0?(uint64_t)balance:0;
    context->samples_ready=(uint32_t)(credit/99000u);
    context->acc_num_missed_samples=(uint32_t)(credit%99000u);
}
static void playback_clock_accrue(a2dp_media_sending_context_t *context,uint32_t now) {
    uint32_t added=playback_adjust_samples(playback_adjust_interval(&playback_adjust),
        now-context->time_audio_data_sent,a2dp_sample_rate(),&context->acc_num_missed_samples);
    int64_t balance=(int64_t)(((uint64_t)context->samples_ready+added)*99000u+
        context->acc_num_missed_samples)-(int64_t)context->sample_debt;
    playback_clock_store(context,balance);
    context->time_audio_data_sent=now;
}
// Debit actual source time; retain a fractional/negative balance until the next USB tick.
static void playback_clock_debit(a2dp_media_sending_context_t *context,uint32_t consumed,uint8_t interval) {
    int64_t balance=(int64_t)((uint64_t)context->samples_ready*99000u+
        context->acc_num_missed_samples)-(int64_t)context->sample_debt;
    playback_clock_store(context,balance-(int64_t)((uint64_t)consumed*10u*(99000u/interval)));
}
static void playback_clock_switch(a2dp_media_sending_context_t *context,uint32_t now,
                                   uint8_t old_interval,uint8_t next_interval) {
    uint32_t added=playback_adjust_samples(old_interval,now-context->time_audio_data_sent,
        a2dp_sample_rate(),&context->acc_num_missed_samples);
    int64_t balance=(int64_t)(((uint64_t)context->samples_ready+added)*99000u+
        context->acc_num_missed_samples)-(int64_t)context->sample_debt;
    playback_clock_store(context,balance*old_interval/next_interval);
    context->time_audio_data_sent=now;
}


// Cancellation must leave both source-time debt and the timer in the normal domain.
static void playback_adjust_cancel_stream(a2dp_media_sending_context_t *context,
                                           uint32_t now, uint8_t old_interval) {
    if (playback_adjust_busy(&playback_adjust) || old_interval!=10)
        playback_continuity_reset(&phone_continuity);
    if (context->streaming && old_interval!=10) {
        playback_clock_switch(context,now,old_interval,10);
        btstack_run_loop_remove_timer(&context->audio_timer);
        btstack_run_loop_set_timer(&context->audio_timer,audio_timer_interval-1u);
        btstack_run_loop_add_timer(&context->audio_timer);
    }
    playback_adjust_cancel(&playback_adjust);
}

static void avdtp_audio_timeout_handler(btstack_timer_source_t * timer){
    a2dp_media_sending_context_t * context = (a2dp_media_sending_context_t *) btstack_run_loop_get_timer_context(timer);
    if (!context->streaming || !a2dp_is_connected_flag || sc.local_stream_endpoint == NULL) return;
    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t interval = playback_adjust.running ? playback_adjust.interval : audio_timer_interval;
    uint32_t late=now-(uint32_t)context->audio_timer.timeout;
    if(late<0x80000000u) {
        PB_MAX(TRACE_TIMER_LATE_US,late*1000u);
        if(late>=2)PB_EVENT(TRACE_LATE,late,context->samples_ready);
    }
    int32_t remaining = (int32_t)((uint32_t)context->audio_timer.timeout + interval - now);
    if (remaining <= 0 || remaining > (int32_t)interval) remaining = (int32_t)interval;
    // The pinned SDK adds one millisecond; retain the previous deadline's phase.
    btstack_run_loop_set_timer(&context->audio_timer, (uint32_t)remaining - 1u);
    btstack_run_loop_add_timer(&context->audio_timer);
    if (playback_idle_phase == PLAYBACK_SUSPENDING) return;
    playback_clock_accrue(context,now);

    // Bound stale work in time as well as frames; SBC's 128 samples are shorter than one tick.
    uint32_t max_ready = 4 * acc_num_simples;
    if (max_ready < (uint32_t)a2dp_sample_rate() / 25) max_ready = (uint32_t)a2dp_sample_rate() / 25;
    // Reconcile an overrun with retained source data in the main-loop worker.
    if(context->samples_ready>max_ready)playback_work_pending=true;

    //printf("num_samples is %d\n", num_samples);
    //printf("samples_ready is %d\n", context->samples_ready);

    if (context->codec_ready_to_send) {
        if ((uint32_t)(now - context->send_wait_start) >= 200) {
            playback_bitrate_transport_stalled();
            playback_transport_recover(context);
        }
        return;
    }

    playback_work_pending=true;
}

// A successful send can leave another whole packet owed by this timer interval.
static void playback_prepare_packet(a2dp_media_sending_context_t *context, uint32_t now) {
    if (playback_encoder_busy()) return;
    if (!context->streaming || !a2dp_is_connected_flag || sc.local_stream_endpoint == NULL ||
        playback_idle_phase != PLAYBACK_RUNNING) return;
    if (context->codec_ready_to_send == 2) {
        context->codec_ready_to_send = 1;
        a2dp_source_stream_endpoint_request_can_send_now(context->avdtp_cid, context->local_seid);
        return;
    }
    if (context->codec_ready_to_send) return;
    if (!context->codec_storage_count && playback_adjust.pending) {
        playback_clock_switch(context,now,10,playback_adjust.interval);
        playback_adjust_start(&playback_adjust, now);
        printf("[GAME] clock start: target=%u interval=%u/10 drift_frames=%lu\n",
            playback_adjust.target, playback_adjust.interval, (unsigned long)playback_adjust.goal_frames);
        btstack_run_loop_remove_timer(&context->audio_timer);
        btstack_run_loop_set_timer(&context->audio_timer, playback_adjust.interval - 1u);
        btstack_run_loop_add_timer(&context->audio_timer);
    }
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
            if (!playback_aac_packet_submit(context, now) && context->codec_storage_count == 0)
                if (fill_aac_audio_buffer(context) < 0) return;
            break;
#endif
        case AVDTP_CODEC_NON_A2DP:
            local_cap = sc.local_stream_endpoint->sep.capabilities.media_codec;
            uint32_t local_vendor_id = get_vendor_id(local_cap.media_codec_information);
            uint16_t local_codec_id = get_codec_id(local_cap.media_codec_information);

            // Playback encoding stays with its packet/transport state on core 0.
            if (local_vendor_id == A2DP_CODEC_VENDOR_ID_APPLE && local_codec_id == A2DP_APPLE_CODEC_AAC_ELD) {
                if (playback_aac_packet_submit(context, now)) return;
                if (fill_aac_audio_buffer(context) < 0) return;
            }
            break;
        default:
            break;
    }
}


static bool a2dp_demo_timer_start(a2dp_media_sending_context_t * context){
    playback_encoder_invalidate();
    playback_continuity_reset(&phone_continuity);
    playback_transport_stale=false;

    int mtu = avdtp_max_media_payload_size(context->avdtp_cid, context->local_seid);
    if (mtu <= 0) {
        aacp_disconnect();
        a2dp_source_disconnect(context->avdtp_cid);
        return false;
    }
    int ceiling = cur_codec == 4 ? PLAYBACK_ELD_PAYLOAD_CEILING : 0x290;
    context->max_media_payload_size = (mtu > 0 && mtu < ceiling) ? mtu : ceiling;
    if (cur_codec == 1) {
        int frame_bytes = btstack_sbc_encoder_sbc_buffer_length();
        // SBC also needs its one-byte packet header; an undersized link never drains.
        if (frame_bytes <= 0 || frame_bytes >= context->max_media_payload_size) {
            aacp_disconnect();
            a2dp_source_disconnect(context->avdtp_cid);
            return false;
        }
    }
    if (cur_codec == 4 && (handleAAC == NULL || context->max_media_payload_size <
                          (int)(playback_eld_au_ceiling(aaceld_bitrate, playback_options.rate_mode != 0) + 4))) {
        aacp_disconnect();
        a2dp_source_disconnect(context->avdtp_cid);
        return false;
    }
    critical_section_enter_blocking(&playback_pcm_lock);
    if (call_active) {
        playback_pcm_reset(&playback_pcm, PHONE_PCM_CAPACITY, cur_codec == 4);
        call_clock_init();
    } else if (bridge_phone_recovering()) {
        playback_pcm_reset(&playback_pcm, PHONE_PCM_CAPACITY, cur_codec == 4);
        phone_buffer_started = false;
        phone_clock_init(&phone_clock, acc_num_simples);
        btstack_resample_init(&phone_resampler, 2);
    } else if (playback_idle_phase == PLAYBACK_STARTING) {
        // USB may have supplied the onset while our confirmed idle stream restarts.
        playback_pcm.priming = cur_codec == 4 ? PLAYBACK_PRIMING_FRAMES : 0;
        playback_pcm.primed_on_wire = false;
        memset(playback_pcm.history,0,sizeof(playback_pcm.history));
    } else playback_pcm_reset(&playback_pcm, audio_slot_capacity(), cur_codec == 4);
    critical_section_exit(&playback_pcm_lock);
    playback_idle_reset();
    context->codec_num_frames = 0;
    context->time_audio_data_sent = btstack_run_loop_get_time_ms();
    context->samples_ready = 0;
    context->acc_num_missed_samples = 0;
    context->sample_debt = 0;
    context->codec_storage_count = 0;
    playback_work_cancel_wait(context);
    context->codec_ready_to_send = 0;
    context->streaming = 1;
    playback_usb_verify_started(context);
    media_last_sent_us=packet_oldest_us=0;
    btstack_run_loop_remove_timer(&context->audio_timer);
    btstack_run_loop_set_timer_handler(&context->audio_timer, avdtp_audio_timeout_handler);
    btstack_run_loop_set_timer_context(&context->audio_timer, context);
    btstack_run_loop_set_timer(&context->audio_timer, audio_timer_interval - 1u);
    btstack_run_loop_add_timer(&context->audio_timer);
    return true;
}

static void a2dp_demo_timer_stop(a2dp_media_sending_context_t * context){
    playback_work_cancel_wait(context);
    playback_encoder_invalidate();
    playback_continuity_reset(&phone_continuity);
    playback_usb_verify_cancel();
    phone_output_recovery = false;
    playback_transport_stale=false;
    playback_adjust_cancel(&playback_adjust);
    is_streaming = false;
    recovery_reset_encoder = false;
    suspend_recovery_pending = false;
    btstack_run_loop_remove_timer(&suspend_recovery_timer);
    context->time_audio_data_sent = 0;
    context->acc_num_missed_samples = 0;
    context->sample_debt = 0;
    context->samples_ready = 0;
    context->streaming = 0;
    context->codec_num_frames = 0;
    context->codec_storage_count = 0;
    context->codec_ready_to_send = 0;
    btstack_run_loop_remove_timer(&context->audio_timer);
    playback_idle_reset();
}

static void playback_idle_reset(void) {
    if (playback_claim.active && playback_claim.session == aacp_control_session() &&
        playback_claim.revision == aacp_routing_state().revision &&
        aacp_takeover_pending(playback_claim.peer)) aacp_routing_override();
    playback_idle_phase = PLAYBACK_RUNNING;
    playback_claim.active = false; // Retain the attempted intent across peer Suspend.
    playback_idle_counting = false;
    playback_peer_suspended = false;
}

static bool playback_claim_transport_current(void) {
    return a2dp_is_connected_flag && media_tracker.avdtp_cid == playback_claim.cid &&
        media_tracker.local_seid == playback_claim.seid && sc.local_stream_endpoint &&
        sc.local_stream_endpoint->l2cap_media_cid && !memcmp(cur_active_device, playback_claim.peer, 6);
}

static bool playback_claim_current(void) {
    const uint8_t *peer = (const uint8_t *)get_device_addr();
    aacp_routing_state_t state = aacp_routing_state();
    return playback_claim.active && cur_codec == 4 && playback_claim_transport_current() && peer &&
        !memcmp(peer, playback_claim.peer, 6) && finish_scan_avdtp_codec &&
        playback_claim.session == aacp_control_session() && playback_claim.revision == state.revision &&
        (state.flags & 1u) && aacp_control_ready() && !playback_released &&
        (!reconnect_auto_intent || dongle_playback_return_signal(peer,btstack_run_loop_get_time_ms())) &&
        !profile_pending && !a2dp_connect_pending && is_usb_streaming &&
        !phone_opened && !phone_active && !call_opened && !call_active && !phone_call_active() &&
        !usb_mic_requested() && !aacp_mic_requested() && !aacp_mic_active() &&
        (aacp_audio_owned() || (aacp_takeover_pending(peer) && state.stage == 3));
}

static uint8_t playback_request_start(void) {
    bt_hci_priority_transition(PRIORITY_START);
    return avdtp_source_start_stream(media_tracker.avdtp_cid, media_tracker.local_seid);
}

static bool playback_claim_begin(uint32_t now) {
    // shortcut: unowned Start is AAC-ELD only; extend after other codecs have capture/test coverage.
    const uint8_t *peer = (const uint8_t *)get_device_addr();
    aacp_routing_state_t state = aacp_routing_state();
    uint32_t session = aacp_control_session();
    if (cur_codec != 4 || aacp_audio_owned() || !peer || !peer_address_valid(peer) || !session ||
        !aacp_takeover_pending(peer) || state.stage != 3 ||
        (playback_claim.session == session && playback_claim.revision == state.revision) ||
        (playback_idle_phase != PLAYBACK_SUSPENDED && playback_idle_phase != PLAYBACK_RUNNING) ||
        media_tracker.streaming || !media_tracker.avdtp_cid || !media_tracker.local_seid) return false;
    uint32_t previous_session = playback_claim.session, previous_revision = playback_claim.revision;
    playback_claim.session = session; playback_claim.revision = state.revision;
    playback_claim.cid = media_tracker.avdtp_cid; playback_claim.seid = media_tracker.local_seid;
    memcpy(playback_claim.peer, peer, 6); playback_claim.active = true;
    if (!playback_claim_current()) {
        playback_claim.active = false;
        playback_claim.session = previous_session; playback_claim.revision = previous_revision;
        return false;
    }
    playback_peer_suspended = false;
    playback_idle_phase = PLAYBACK_STARTING;
    playback_idle_transition_since = now;
    // Negotiate only: the buds can report their source after accepting Start.
    if (playback_request_start() != ERROR_CODE_SUCCESS)
        playback_encoder_failed(&media_tracker);
    return true;
}

static bool playback_park_unowned_open(void) {
    // START can be rejected, or a peer SUSPEND can beat ours. BTstack keeps
    // the media CID and the SEP OPENED in either case. It can accept the next Start.
    if (aacp_audio_owned() || !finish_scan_avdtp_codec || !media_tracker.avdtp_cid || !media_tracker.local_seid ||
        !sc.local_stream_endpoint || !sc.local_stream_endpoint->l2cap_media_cid ||
        sc.local_stream_endpoint->state!=AVDTP_STREAM_ENDPOINT_OPENED ||
        !dongle_playback_auto_standby(cur_active_device)) return false;
    a2dp_demo_timer_stop(&media_tracker);
    playback_idle_phase=PLAYBACK_SUSPENDED;
    aacp_route_avdtp_event(4,0);
    return true;
}
static void playback_claim_suspend(uint32_t now) {
    bool current = playback_claim_transport_current();
    if (!current) { playback_idle_reset(); return; }
    if (playback_park_unowned_open()) return;
    a2dp_demo_timer_stop(&media_tracker);
    playback_idle_phase = PLAYBACK_SUSPENDING;
    playback_idle_transition_since = now;
    if (avdtp_source_suspend(media_tracker.avdtp_cid, media_tracker.local_seid) != ERROR_CODE_SUCCESS)
        playback_encoder_failed(&media_tracker);
}

static bool passive_start_ready(uint32_t now) {
    if (!reconnect_auto_intent) return true;
    return aacp_audio_owned() && (phone_opened || phone_active || call_opened || call_active ||
        aacp_mic_requested() || aacp_mic_active() || dongle_playback_return_signal(cur_active_device,now));
}

static void playback_idle_poll(uint32_t now) {
    if (playback_claim.active && !playback_claim_transport_current()) {
        playback_idle_reset();
        return;
    }
    if (playback_claim.active && playback_idle_phase == PLAYBACK_STARTING) {
        if ((uint32_t)(now - playback_idle_transition_since) >= PLAYBACK_TRANSITION_MS && !playback_park_unowned_open())
            playback_encoder_failed(&media_tracker);
        return;
    }
    if (playback_idle_phase == PLAYBACK_CLAIMING) {
        if (!playback_claim_current() || (uint32_t)(now - playback_idle_transition_since) >= PLAYBACK_TRANSITION_MS) {
            playback_claim_suspend(now);
            return;
        }
        if (!aacp_audio_owned()) return;
    }
    if (!a2dp_is_connected_flag || !finish_scan_avdtp_codec || !media_tracker.local_seid ||
        profile_pending || a2dp_connect_pending) return;
    if (playback_idle_phase == PLAYBACK_SUSPENDING || playback_idle_phase == PLAYBACK_STARTING) {
        if ((uint32_t)(now - playback_idle_transition_since) >= PLAYBACK_TRANSITION_MS && !playback_park_unowned_open())
            playback_encoder_failed(&media_tracker);
        return;
    }
    if (!aacp_audio_owned()) { (void)playback_claim_begin(now); return; }
    if (playback_peer_suspended && !media_tracker.streaming && playback_idle_phase != PLAYBACK_CLAIMING) {
        critical_section_enter_blocking(&playback_pcm_lock);
        bool restarted = is_usb_streaming && usb_playback_revision != playback_peer_suspend_revision;
        critical_section_exit(&playback_pcm_lock);
        if (restarted) {
            // The peer's stop wins until the host starts a genuinely new USB activity.
            // Reuse the confirmed-idle Start path, including its bounded ACK deadline.
            playback_peer_suspended = false;
            playback_idle_phase = PLAYBACK_SUSPENDED;
        }
    }
    bool wanted = is_usb_streaming || phone_opened || phone_active || call_opened || call_active || aacp_mic_requested() || aacp_mic_active();
    bool claimed = playback_idle_phase == PLAYBACK_CLAIMING;
    if (playback_idle_phase == PLAYBACK_SUSPENDED || claimed) {
        if (!wanted) return;
        // USB enumeration can start a continuous stream of zeros before a player starts.
        if (!passive_start_ready(now)) return;
        if (!claimed && playback_game_eligible() && !aacp_mic_requested() && !aacp_mic_active() && !playback_game.failed) {
            if (playback_game.requested && playback_game.phase != GAME_CONFIRMED) return;
            if (!playback_game.requested && (playback_game.target != 150 || playback_game.changed ||
                playback_game.waiting_tx || playback_game.waiting_report)) return;
        }
        playback_idle_phase = PLAYBACK_STARTING;
        if (!claimed) playback_idle_transition_since = now;
        if (cur_codec == 4 && !aaceld_encoder_init()) {
            playback_encoder_failed(&media_tracker);
        } else if (claimed) {
            is_streaming = a2dp_demo_timer_start(&media_tracker);
            if (is_streaming) set_led_mode_on();
        } else if (playback_request_start() != ERROR_CODE_SUCCESS) {
            playback_encoder_failed(&media_tracker);
        }
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

bool dongle_playback_auto_standby(const uint8_t peer[6]) {
    const uint8_t *selected = (const uint8_t *)get_device_addr();
    return reconnect_automatic && !playback_released && a2dp_is_connected_flag && peer && selected &&
        !memcmp(peer,selected,6) && !memcmp(peer,cur_active_device,6) && !profile_pending && !a2dp_connect_pending &&
        !phone_opened && !phone_active && !call_opened && !call_active && !phone_call_active() &&
        !usb_mic_requested() && !aacp_mic_requested() && !aacp_mic_active() &&
        !mic_prepare_open && !mic_recovery_pending && !suspend_recovery_pending && !phone_output_recovery &&
        aacp_secure_control_peer(peer);
}
uint32_t dongle_playback_return_signal(const uint8_t peer[6], uint32_t now) {
    if (!is_usb_streaming || !finish_scan_avdtp_codec) return 0;
    uint32_t stamp = atomic_load_explicit(&usb_program_stamp, memory_order_acquire);
    return stamp && (uint32_t)(now - (stamp - 1u)) < 150u && dongle_playback_auto_standby(peer) ? stamp : 0;
}

void dongle_playback_ownership_changed(bool owned) {
    if (owned) {
        usb_media_cancel_pause();
        // A passive media transport may already be open while control was resolved.
        if (a2dp_is_connected_flag && finish_scan_avdtp_codec && media_tracker.local_seid &&
            sc.local_stream_endpoint && sc.local_stream_endpoint->l2cap_media_cid &&
            !media_tracker.streaming && playback_idle_phase == PLAYBACK_RUNNING)
            playback_idle_phase = PLAYBACK_SUSPENDED;
        return;
    }
    aacp_receive_buffer_cancel();
    bool requested = playback_game.requested;
    memset(&playback_game, 0, sizeof(playback_game));
    playback_game.requested = requested;
    playback_game.phase = requested ? GAME_PENDING : GAME_OFF;
    mic_prepare_open = mic_recovery_pending = mic_recovery_started = false;
    uint8_t phase = playback_idle_phase;
    uint32_t transition_since = playback_idle_transition_since;
    bool running = media_tracker.streaming;
    bool claiming = phase == PLAYBACK_CLAIMING && playback_claim_transport_current();
    if (running && !call_active && !phone_call_active() && !usb_mic_requested()) {
        if (is_usb_streaming) {
            if (!reconnect_automatic) (void)usb_media_enqueue(USB_MEDIA_PAUSE_ONLY);
        }
        else if (phone_active) phone_sink_pause_for_takeover();
    }
    a2dp_demo_timer_stop(&media_tracker);
    critical_section_enter_blocking(&playback_pcm_lock);
    playback_pcm_clear(&playback_pcm, audio_slot_capacity());
    usb_recovery_drops=0; // The discarded USB stream no longer needs overrun recovery.
    critical_section_exit(&playback_pcm_lock);
    // Stop encoding immediately, but keep an in-flight command and its deadline.
    // A late accepted Start is suspended below, never allowed to restart audio.
    if (phase == PLAYBACK_SUSPENDING || phase == PLAYBACK_STARTING) {
        playback_idle_phase = phase;
        playback_idle_transition_since = transition_since;
        return;
    }
    if ((running || claiming) && a2dp_is_connected_flag && media_tracker.local_seid) {
        if (playback_park_unowned_open()) return;
        playback_idle_phase = PLAYBACK_SUSPENDING;
        playback_idle_transition_since = btstack_run_loop_get_time_ms();
        if (avdtp_source_suspend(media_tracker.avdtp_cid, media_tracker.local_seid) != ERROR_CODE_SUCCESS)
            playback_encoder_failed(&media_tracker);
    }
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






static void signaling_connect_failed(uint16_t cid,const uint8_t address[6]) {
    if (!a2dp_connect_pending || a2dp_is_connected_flag || !cid || media_tracker.avdtp_cid!=cid) return;
    if (passive_attempt.cid==cid && memcmp(passive_attempt.peer,address,6)) return;
    a2dp_connect_pending=false; media_tracker.avdtp_cid=0; passive_attempt.cid=0;
}

static void signaling_connection_established(uint8_t *packet, uint16_t size) {
    uint8_t status;
    uint16_t avdtp_cid;
    bd_addr_t address;
    if (size < 14) return;
    avdtp_subevent_signaling_connection_established_get_bd_addr(packet, address);
    // The raw AVDTP event is shared with the phone's independent sink role.
    if (bt_hci_phone_peer_allowed(address)) return;
    // print info
    printf("Current Device found: %s \n",  bd_addr_to_str(address));
    avdtp_cid = avdtp_subevent_signaling_connection_established_get_avdtp_cid(packet);
    status = avdtp_subevent_signaling_connection_established_get_status(packet);
    const uint8_t *selected=(const uint8_t *)get_device_addr();
    if (selected && !memcmp(address,selected,6)) aacp_route_avdtp_event(1,status);
    if (status != 0){
        signaling_connect_failed(avdtp_cid,address);
        printf("AVDTP source signaling connection failed: status %d\n", status);
        return;
    }
    if (!playback_peer_accept((const uint8_t *)get_device_addr(), address,
                              media_tracker.avdtp_cid, avdtp_cid,
                              a2dp_accept_incoming,
                              a2dp_is_connected_flag || a2dp_connect_pending)) {
        a2dp_source_disconnect(avdtp_cid);
        return;
    }
    if (!a2dp_connect_pending && reconnect_automatic && !aacp_takeover_pending(address) && !profile_pending)
        reconnect_auto_intent=true;
    a2dp_connect_pending = false;
    playback_encoder_close();
    aaceld_variable_allowed = false;
    memcpy(cur_active_device, address, sizeof(bd_addr_t));
    cur_codec=0; actual_bitrate=0; reported_delay=UINT16_MAX; delay_revision=0;
    a2dp_codec_configuration_reset();
    finish_scan_avdtp_codec=false;
    media_tracker.avdtp_cid = avdtp_subevent_signaling_connection_established_get_avdtp_cid(packet);
    printf("AVDTP source signaling connection established: avdtp_cid 0x%02x\n", avdtp_cid);

    allow_switch_slot = false;
    set_led_mode_on();
    // seid selected per argv
    status = avdtp_source_discover_stream_endpoints(media_tracker.avdtp_cid);
    a2dp_is_connected_flag = true;

    // Open the AACP channel (PSM 0x1001) to the same AirPods once
    // A2DP signaling is up. Runs after a short settle delay.
    aacp_connect(cur_active_device);
}

static void media_connection_established(uint8_t *packet, uint16_t size) {
    uint8_t status;
    uint16_t avdtp_cid;
    bd_addr_t address;
    if (size != 14 || !a2dp_is_connected_flag || !media_tracker.avdtp_cid) return;
    if (avdtp_subevent_streaming_connection_established_get_avdtp_cid(packet) != media_tracker.avdtp_cid) return;
    avdtp_subevent_streaming_connection_established_get_bd_addr(packet, address);
    if (memcmp(address, cur_active_device, 6) ||
        memcmp(address, (const uint8_t *)get_device_addr(), 6)) return;
    status = avdtp_subevent_streaming_connection_established_get_status(packet);
    if (status != 0){
        printf("Streaming connection failed: status %d\n", status);
        // The SDK keeps signaling open after a failed media channel; retire it for normal retry.
        playback_encoder_failed(&media_tracker);
        return;
    }
    avdtp_cid = avdtp_subevent_streaming_connection_established_get_avdtp_cid(packet);
    media_tracker.local_seid = avdtp_subevent_streaming_connection_established_get_local_seid(packet);
    media_tracker.remote_seid = avdtp_subevent_streaming_connection_established_get_remote_seid(packet);
    // Encoder recovery reuses this media channel. Start the wire timeline only on a new channel.
    media_tracker.rtp_timestamp = 0;
    aaceld_frame_sequence = 1;
    a2dp_is_connected_flag = true;

    printf("Streaming connection established, avdtp_cid 0x%02x\n", avdtp_cid);

    if (sc.local_stream_endpoint != NULL) {
        l2cap_set_flushable_local_cid(sc.local_stream_endpoint->l2cap_media_cid);
    }
    // Resolve AACP ownership before initial Start; two powered dongles
    // may retain their transports, but only the active host sends audio.
    if (aacp_audio_owned()) playback_idle_phase = PLAYBACK_SUSPENDED;
    avrcp_reconnect_schedule(true);
    avrcp_reconnect_poll(btstack_run_loop_get_time_ms());
}

static void delay_report_received(uint8_t *packet, uint16_t size) {
    if (size!=8 || !media_tracker.local_seid ||
        avdtp_subevent_signaling_delay_report_get_avdtp_cid(packet)!=media_tracker.avdtp_cid ||
        avdtp_subevent_signaling_delay_report_get_local_seid(packet)!=media_tracker.local_seid) return;
    playback_game_observe_send(&playback_game,btstack_run_loop_get_time_ms(),
        aacp_receive_buffer_serial(),aacp_receive_buffer_last(),delay_revision);
    reported_delay=avdtp_subevent_signaling_delay_report_get_delay_100us(packet);
    if (++delay_revision==0) ++delay_revision;
    printf("DELAY_REPORT received: %d.%0d ms, local seid %d\n", 
        avdtp_subevent_signaling_delay_report_get_delay_100us(packet)/10, avdtp_subevent_signaling_delay_report_get_delay_100us(packet)%10,
        avdtp_subevent_signaling_delay_report_get_local_seid(packet));
}

static void sbc_configuration_received(uint8_t *packet, uint16_t size) {
    aaceld_variable_allowed = false;
    if (size < 18) return;
    if (playback_options.codec && playback_options.codec!=DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_SBC) { playback_encoder_failed(&media_tracker); return; }
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
        return;
    }
    a2dp_demo_timer_stop(&media_tracker);
    playback_encoder_close();
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
}

static void aac_configuration_received(uint8_t *packet, uint16_t size) {
    aaceld_variable_allowed = false;
    if (size < 18) return;
    if (!a2dp_codec_configuration_aac_offered()) return;
    if (playback_options.codec && playback_options.codec!=DONGLE_AUDIO_PROFILE_REQUESTED_CODEC_AAC) { playback_encoder_failed(&media_tracker); return; }
    uint32_t rate = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_sampling_frequency(packet);
    uint8_t channels = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_num_channels(packet);
    uint8_t object = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_object_type(packet);
    uint32_t bitrate = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_bit_rate(packet);
    uint8_t vbr = a2dp_subevent_signaling_media_codec_mpeg_aac_configuration_get_vbr(packet);
    if (!playback_format_supported(rate, channels) ||
        (object != AVDTP_AAC_MPEG2_LC && object != AVDTP_AAC_MPEG4_LC) ||
        vbr || bitrate < 96000 || bitrate > (uint32_t)a2dp_codec_configuration_maximum_aac_bitrate() ||
        (playback_options.bitrate && playback_options.bitrate!=bitrate)) {
        playback_encoder_failed(&media_tracker);
        return;
    }
    a2dp_demo_timer_stop(&media_tracker);
    playback_encoder_close();
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
        playback_encoder_close();
        handleAAC = NULL;
        playback_encoder_failed(&media_tracker);
        return;
    }
    acc_num_simples = frame_samples;
    current_sample_rate = rate;
    audio_timer_interval = aac_audio_timer_interval;
    audio_slot_queue_configure_with_count(acc_num_simples, AUDIO_SLOT_COUNT_AAC);
    cur_codec = 2;
    actual_bitrate=bitrate; profile_pending=false;
    avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);
}

static void vendor_codec_configuration_received(uint8_t *packet, uint16_t size) {
    uint32_t vendor_id;
    uint16_t codec_id;
    if (size < 19) return;
    printf("Received other configuration\n");
    uint8_t *codec_info = (uint8_t *) a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information(packet);
    uint16_t config_len = a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information_len(packet);
    if (config_len < 6 || codec_info + config_len > packet + size) return;
    vendor_id = get_vendor_id(codec_info);
    codec_id = get_codec_id(codec_info);

    // Apple AAC-ELD
    if (vendor_id == A2DP_CODEC_VENDOR_ID_APPLE && codec_id == A2DP_APPLE_CODEC_AAC_ELD) {
        aaceld_variable_allowed = false;
        printf("A2DP Source: Received Apple AAC-ELD configuration!\n");
        printf("  raw config bytes: ");
        uint16_t config_info_len = a2dp_subevent_signaling_media_codec_other_configuration_get_media_codec_information_len(packet);
        for (int k = 0; k < config_info_len; k++) {
            printf("%02x ", codec_info[k]);
        }
        printf("\n");

        if (!playback_options_eld_configuration(codec_info, config_info_len,playback_options) ||
            !playback_eld_peer_configuration(
                a2dp_subevent_signaling_media_codec_other_configuration_get_local_seid(packet),
                a2dp_subevent_signaling_media_codec_other_configuration_get_remote_seid(packet),
                codec_info, config_info_len)) {
            printf("Unsupported AAC-ELD configuration\n");
            aacp_disconnect();
            a2dp_source_disconnect(media_tracker.avdtp_cid);
            return;
        }
        a2dp_demo_timer_stop(&media_tracker);
        playback_eld_bind_configuration(
            a2dp_subevent_signaling_media_codec_other_configuration_get_local_seid(packet),
            a2dp_subevent_signaling_media_codec_other_configuration_get_remote_seid(packet));
        aaceld_variable_allowed = playback_eld_variable(codec_info);
        aaceld_bitrate = playback_options_eld_rate(codec_info, config_info_len, playback_options);
        playback_bitrate_reset(&playback_bitrate, aaceld_bitrate, playback_options.rate_mode != 0);
        if (!aaceld_encoder_init()) {
            printf("AAC-ELD encoder initialization failed\n");
            aacp_disconnect();
            a2dp_source_disconnect(media_tracker.avdtp_cid);
            return;
        }

        current_sample_rate = 48000;
        acc_num_simples = aacinf.frameLength;  // use actual frame size from encoder
        audio_timer_interval = 10;

        printf("AAC-ELD acc_num_simples=%d\n", acc_num_simples);

        audio_slot_queue_configure_with_count(acc_num_simples, AUDIO_SLOT_COUNT_ELD);

        cur_codec = 4;  // new codec ID for AAC-ELD
        actual_bitrate=aaceld_bitrate; profile_pending=false;

        avdtp_source_open_stream(media_tracker.avdtp_cid, media_tracker.local_seid, media_tracker.remote_seid);
    }
    else
    {
        printf("Config not handled: vendor=0x%08lx codec=0x%04x\n", (unsigned long)vendor_id, codec_id);
    }
}

static void signaling_command_accepted(uint8_t *packet, uint16_t size) {
    uint8_t signal_identifier;
    if (size < 8) return;
    if (avdtp_subevent_signaling_accept_get_avdtp_cid(packet) != media_tracker.avdtp_cid) return;
    signal_identifier = avdtp_subevent_signaling_accept_get_signal_identifier(packet);
    if (is_cmd_triggered_localy){
        is_cmd_triggered_localy = 0;
        printf("AVDTP Source command accepted\n");
    }

    switch (signal_identifier){
        case AVDTP_SI_ACCEPT_START:
            if (avdtp_subevent_signaling_accept_get_local_seid(packet) == media_tracker.local_seid) {
                if (!passive_start_ready(btstack_run_loop_get_time_ms())) {
                    avdtp_start_stream_reject(media_tracker.avdtp_cid,media_tracker.local_seid);
                    break;
                }
                bt_hci_priority_transition(PRIORITY_START);
                avdtp_start_stream_accept(media_tracker.avdtp_cid, media_tracker.local_seid);
            }
            break;
        case AVDTP_SI_OPEN:
            break;
        case AVDTP_SI_SET_CONFIGURATION:
            break;
        case  AVDTP_SI_START:
            if (encoder_job.deferred_size) break;
            if (avdtp_subevent_signaling_accept_get_local_seid(packet) != media_tracker.local_seid) break;
            if (playback_claim.active) {
                uint32_t now = btstack_run_loop_get_time_ms();
                if (playback_idle_phase == PLAYBACK_STARTING && playback_claim_current() &&
                    (uint32_t)(now - playback_idle_transition_since) < PLAYBACK_TRANSITION_MS)
                    playback_idle_phase = PLAYBACK_CLAIMING;
                else playback_claim_suspend(now);
                break;
            }
            if (profile_pending || a2dp_connect_pending || !finish_scan_avdtp_codec) break;
            if (!aacp_audio_owned() || !passive_start_ready(btstack_run_loop_get_time_ms())) {
                a2dp_demo_timer_stop(&media_tracker);
                playback_idle_phase = PLAYBACK_SUSPENDING;
                playback_idle_transition_since = btstack_run_loop_get_time_ms();
                if (avdtp_source_suspend(media_tracker.avdtp_cid, media_tracker.local_seid) != ERROR_CODE_SUCCESS)
                    playback_encoder_failed(&media_tracker);
                break;
            }
            printf("Stream started.\n");
            if (finish_scan_avdtp_codec){
                bool mic_repaired=mic_recovery_pending && mic_recovery_started &&
                    mic_recovery_cid==media_tracker.avdtp_cid &&
                    playback_idle_phase==PLAYBACK_STARTING;
                is_streaming = a2dp_demo_timer_start(&media_tracker);
                if (is_streaming) {
                    reconnect_auto_intent=false;
                    set_led_mode_on();
                    if (mic_repaired) {
                        mic_recovery_pending=mic_recovery_started=false;
                        if (++mic_recovery_revision==0) ++mic_recovery_revision;
                    }
                }
            }
            break;
        case AVDTP_SI_SUSPEND: {
            if (avdtp_subevent_signaling_accept_get_local_seid(packet) != media_tracker.local_seid) break;
            bt_hci_priority_transition(PRIORITY_SUSPEND);
            bool idle = playback_idle_phase == PLAYBACK_SUSPENDING;
            bool recover = suspend_recovery_pending;
            bool reset = recovery_reset_encoder;
            bool keep_phone = bridge_phone_recovering();
            critical_section_enter_blocking(&playback_pcm_lock);
            uint32_t usb_revision = usb_playback_revision;
            critical_section_exit(&playback_pcm_lock);
            a2dp_demo_timer_stop(&media_tracker);
            if (idle) playback_idle_phase = PLAYBACK_SUSPENDED;
            else if (recover) {
                phone_output_recovery = keep_phone;
                if (reset && !aaceld_encoder_init()) {
                    phone_output_recovery = false;
                    aacp_disconnect();
                    a2dp_source_disconnect(media_tracker.avdtp_cid);
                    break;
                }
                playback_idle_phase=PLAYBACK_STARTING;
                playback_idle_transition_since=btstack_run_loop_get_time_ms();
                if (playback_request_start() != ERROR_CODE_SUCCESS) {
                    phone_output_recovery = false;
                    aacp_disconnect();
                    a2dp_source_disconnect(media_tracker.avdtp_cid);
                }
            }
            else {
                playback_peer_suspended = true;
                playback_peer_suspend_revision = usb_revision;
            }
            break;
        }
        case AVDTP_SI_ABORT:
        case AVDTP_SI_CLOSE:
            if (avdtp_subevent_signaling_accept_get_local_seid(packet) != media_tracker.local_seid) break;
            bt_hci_priority_transition(PRIORITY_CLOSE);
            printf("Stream released.\n");
            a2dp_demo_timer_stop(&media_tracker);
            break;
        default:
            break;
    }
}

static void signaling_command_rejected(uint8_t *packet, uint16_t size) {
    uint8_t signal_identifier;
    if (size != 8 || !a2dp_is_connected_flag || !media_tracker.avdtp_cid ||
        avdtp_subevent_signaling_reject_get_is_initiator(packet) != 1 ||
        memcmp(cur_active_device, (const uint8_t *)get_device_addr(), 6)) return;
    if (avdtp_subevent_signaling_reject_get_avdtp_cid(packet) != media_tracker.avdtp_cid) return;
    signal_identifier = avdtp_subevent_signaling_reject_get_signal_identifier(packet);
    bool connection_query = signal_identifier == AVDTP_SI_DISCOVER ||
        signal_identifier == AVDTP_SI_GET_CAPABILITIES || signal_identifier == AVDTP_SI_GET_ALL_CAPABILITIES;
    if (!connection_query &&
        avdtp_subevent_signaling_reject_get_local_seid(packet) != media_tracker.local_seid) return;
    if (signal_identifier==AVDTP_SI_START || signal_identifier==AVDTP_SI_SUSPEND) {
        aacp_route_avdtp_event(5,signal_identifier);
        if (playback_park_unowned_open()) return;
    }
    if (connection_query || signal_identifier == AVDTP_SI_SET_CONFIGURATION ||
        signal_identifier == AVDTP_SI_OPEN || signal_identifier == AVDTP_SI_SUSPEND ||
        signal_identifier == AVDTP_SI_START)
        playback_encoder_failed(&media_tracker);
}

static void media_connection_released(uint8_t *packet, uint16_t size) {
    if (size != 6 || !a2dp_is_connected_flag || !media_tracker.avdtp_cid ||
        avdtp_subevent_streaming_connection_released_get_local_seid(packet) != media_tracker.local_seid ||
        memcmp(cur_active_device, (const uint8_t *)get_device_addr(), 6)) return;
    if (avdtp_subevent_streaming_connection_released_get_avdtp_cid(packet) != media_tracker.avdtp_cid) return;
    bt_hci_priority_transition(PRIORITY_CLOSE);
    l2cap_set_flushable_local_cid(0);
    // Media Close resets the SDK endpoint; a live signaling link alone cannot restart it.
    aacp_route_avdtp_event(6,0);
    playback_encoder_failed(&media_tracker);
    printf("Streaming connection released.\n");
}

static void signaling_connection_released(uint8_t *packet, uint16_t size) {
    UNUSED(size);
    if (avdtp_subevent_signaling_connection_released_get_avdtp_cid(packet) != media_tracker.avdtp_cid) return;
    aacp_route_avdtp_event(2,0);
    bt_hci_priority_transition(PRIORITY_CLOSE);
    aaceld_variable_allowed = false;
    avrcp_reconnect_reset();
    mic_prepare_open=mic_recovery_pending=mic_recovery_started=false;
    a2dp_demo_timer_stop(&media_tracker);
    aacp_media_disconnected();
    l2cap_set_flushable_local_cid(0);
    audio_slot_queue_init();
    playback_encoder_close();
    a2dp_connect_pending = false;
    media_tracker.avdtp_cid = 0;
    media_tracker.local_seid = 0;
    media_tracker.remote_seid = 0;
    allow_switch_slot = true;
    finish_scan_avdtp_codec = false;
    a2dp_is_connected_flag = false;
    cur_codec=0; actual_bitrate=0; reported_delay=UINT16_MAX; delay_revision=0;
    cur_capability = 0;
    set_led_mode_off();
    a2dp_codec_configuration_released();
    printf("Signaling connection released.\n");
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (packet_type != HCI_EVENT_PACKET || size < 3 || packet[1] + 2u != size) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVDTP_META) return;
    if (packet[2] != AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED &&
        (size < 5 || little_endian_read_16(packet, 3) != media_tracker.avdtp_cid)) return;
    if ((packet[2] == AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION ||
         packet[2] == AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CONFIGURATION ||
         packet[2] == AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION) &&
        playback_encoder_defer_configuration(packet, size)) return;
    UNUSED(channel);
    UNUSED(size);

    //printf("Current packet event is 0x%02x\n", packet[2]);
    if (a2dp_codec_configuration_discovery_event(packet, size)) return;

    switch (packet[2]){
        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
            signaling_connection_established(packet, size);
            break;
        case AVDTP_SUBEVENT_STREAMING_CONNECTION_ESTABLISHED:
            media_connection_established(packet, size);
            break;
        case AVDTP_SUBEVENT_SIGNALING_DELAY_REPORT:
            delay_report_received(packet, size);
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
            sbc_configuration_received(packet, size);
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AAC_CONFIGURATION:
            aac_configuration_received(packet, size);
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_MPEG_AUDIO_CONFIGURATION:
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_ATRAC_CONFIGURATION:
            // TODO: handle other configuration event
            printf("MPEG audio/ATRAC configuration not supported\n");
            break;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
            vendor_codec_configuration_received(packet, size);
            break;
        case AVDTP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
            if (avdtp_subevent_streaming_can_send_media_packet_now_get_avdtp_cid(packet) != media_tracker.avdtp_cid ||
                avdtp_subevent_streaming_can_send_media_packet_now_get_local_seid(packet) != media_tracker.local_seid) break;
            a2dp_demo_send_media_packet();
            break;
        case AVDTP_SUBEVENT_SIGNALING_ACCEPT:
            signaling_command_accepted(packet, size);
            break;
        case AVDTP_SUBEVENT_SIGNALING_REJECT:
        case AVDTP_SUBEVENT_SIGNALING_GENERAL_REJECT:
            signaling_command_rejected(packet, size);
            break;
        case AVDTP_SUBEVENT_STREAMING_CONNECTION_RELEASED:
            media_connection_released(packet, size);
            break;
        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            signaling_connection_released(packet, size);
            break;
        default:
            break;
    }
}






bool avdtp_disconnect_and_scan(void){
    reconnect_auto_intent=false;
    aacp_routing_override();
    playback_released=playback_release_pending=false;
    avrcp_reconnect_reset();
    // Pair supersedes a profile reconnect, even if clearing the slot or inquiry fails.
    profile_pending = profile_reconnect = false;
    a2dp_accept_incoming = false;
    a2dp_demo_timer_stop(&media_tracker);
    aacp_disconnect();
    l2cap_set_flushable_local_cid(0);
    a2dp_connect_pending = false;
    a2dp_source_disconnect(media_tracker.avdtp_cid);
    if (avrcp_connection_cid() != 0) avrcp_disconnect(avrcp_connection_cid());
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
    bt_hci_retire_selected_peer();
    //gap_delete_all_link_keys();
    return gap_start_scanning();
}

static void connect_selected_media(const uint8_t addr[6]) {
    a2dp_accept_incoming = true;
    if (a2dp_is_connected_flag || a2dp_connect_pending) return;
    a2dp_connect_pending = true;
    uint8_t status = avdtp_source_connect((uint8_t *)addr, &media_tracker.avdtp_cid);
    aacp_route_avdtp_event(0,status);
    if (status != ERROR_CODE_SUCCESS) {
        a2dp_connect_pending = false;
        media_tracker.avdtp_cid = 0;
    }
}

void avdtp_source_establish_stream(void) {
    const uint8_t *addr = (const uint8_t *)get_device_addr();
    if (addr == NULL || !peer_address_valid(addr)) return;
    if (!aacp_takeover_pending(addr) &&
        !(profile_pending && !memcmp(profile_peer, addr, 6) &&
          (uint32_t)(btstack_run_loop_get_time_ms() - profile_since) < 15000u)) return;
    connect_selected_media(addr);
}

void a2dp_source_reconnect(void) {
    avdtp_source_establish_stream();
}

void dongle_playback_connect_requested(void) {
    playback_game.returning_to_usb=false;
    reconnect_auto_intent=false;
    playback_released=playback_release_pending=false;
    a2dp_accept_incoming=true;
}

void dongle_playback_automatic_returned(void) {
    playback_game.returning_to_usb=playback_game.requested && is_usb_streaming;
}

static void playback_release_close(void) {
    playback_release_pending=false;
    avrcp_reconnect_reset();
    a2dp_demo_timer_stop(&media_tracker);
    aacp_disconnect();
    l2cap_set_flushable_local_cid(0);
    if (media_tracker.avdtp_cid) a2dp_source_disconnect(media_tracker.avdtp_cid);
    if (avrcp_connection_cid()) avrcp_disconnect(avrcp_connection_cid());
}

uint16_t dongle_routing_snapshot(uint8_t *out, uint16_t capacity) {
    if (!out || capacity<32) return 0;
    const uint8_t *peer=(const uint8_t *)get_device_addr();
    aacp_routing_state_t state=aacp_routing_state();
    bool held=playback_released && peer && !memcmp(peer,release_peer,6);
    bool released=held && !aacp_audio_owned() && !a2dp_is_connected_flag && !a2dp_connect_pending && !media_tracker.avdtp_cid;
    memset(out,0,32);out[0]=1;out[1]=32;
    out[2]=state.flags|(released?8u:0u)|(held&&!released?16u:0u)|
        (a2dp_is_connected_flag?32u:0u)|(aacp_control_ready()?64u:0u)|
        ((aacp_mic_requested()||aacp_mic_active()||call_active||call_opened)?128u:0u);
    out[3]=state.stage;dongle_put32(out+4,state.revision);
    dongle_put32(out+8,aacp_control_session());
    if(peer)memcpy(out+12,peer,6);
    out[18]=state.action;dongle_put32(out+20,btstack_run_loop_get_time_ms());
    dongle_put32(out+24,state.age_ms);dongle_put32(out+28,state.nonce);
    return 32;
}

uint8_t dongle_routing_request(uint8_t action, uint32_t expected, uint32_t nonce) {
    if (action>1 || !expected || !nonce) return DONGLE_MALFORMED;
    aacp_routing_state_t state=aacp_routing_state();
    if (state.revision!=expected || state.nonce==nonce) return DONGLE_STALE;
    const uint8_t *peer=(const uint8_t *)get_device_addr();
    if (!peer || !peer_address_valid(peer)) return DONGLE_DISCONNECTED;
    if (state.flags&4) return DONGLE_BUSY;
    if (aacp_mic_requested() || aacp_mic_active() || call_active || call_opened ||
        profile_pending || profile_reconnect || a2dp_connect_pending ||
        (playback_released && (a2dp_is_connected_flag || media_tracker.avdtp_cid)) || mic_prepare_open ||
        mic_recovery_pending || phone_output_recovery || suspend_recovery_pending ||
        (playback_game.changed && (action || playback_game.phase!=GAME_CONFIRMED)) ||
        playback_game.waiting_tx || playback_game.waiting_report ||
        playback_game.restore_failed) return DONGLE_BUSY;
    if (action) {
        if (!aacp_takeover_request(peer)) return DONGLE_BUSY;
        aacp_routing_receipt(nonce,2);
        avdtp_source_establish_stream();
        return DONGLE_ACCEPTED;
    }
    reconnect_auto_intent=false;
    aacp_routing_override();
    aacp_routing_receipt(nonce,1);
    memcpy(release_peer,peer,6);playback_released=true;a2dp_accept_incoming=false;
    if (playback_game.changed) {
        // Restore only this owned receiver before closing; retain its requested preference.
        playback_release_pending=true;release_revision=aacp_routing_state().revision;
        release_session=aacp_control_session();release_cid=media_tracker.avdtp_cid;
        playback_usb_verify_cancel();
        playback_adjust_cancel_stream(&media_tracker,btstack_run_loop_get_time_ms(),playback_adjust_interval(&playback_adjust));
        playback_game.phase=GAME_RESTORING;
    } else playback_release_close();
    return DONGLE_ACCEPTED;
}

static void playback_reconnect_policy_load(void) {
    reconnect_policy_supported=read_reconnect_policy(&reconnect_automatic);
    if (!reconnect_policy_supported) reconnect_automatic=false;
}

uint8_t dongle_reconnect_policy(bool set, bool automatic, bool *current) {
    if (!current) return DONGLE_MALFORMED;
    *current=false;
    if (set) {
        if (is_usb_streaming || phone_opened || phone_active || media_tracker.streaming ||
            usb_mic_requested() || aacp_mic_requested() || aacp_mic_active() || call_active || call_opened ||
            profile_pending || profile_reconnect || a2dp_connect_pending || playback_release_pending ||
            mic_prepare_open || mic_recovery_pending || phone_output_recovery || suspend_recovery_pending ||
            playback_game.waiting_tx || playback_game.waiting_report || playback_game.restore_failed ||
            playback_adjust_busy(&playback_adjust) || playback_idle_phase==PLAYBACK_SUSPENDING ||
            playback_idle_phase==PLAYBACK_STARTING || playback_idle_phase==PLAYBACK_CLAIMING) return DONGLE_BUSY;
        bool stored;
        bool readable=read_reconnect_policy(&stored);
        if (!readable || !write_reconnect_policy(automatic)) {
            // Persistence may have partly succeeded: do not claim the old mode is still stored.
            reconnect_policy_supported=reconnect_automatic=false;
            if (reconnect_auto_intent) {
                if (!a2dp_is_connected_flag) aacp_disconnect();
                aacp_routing_override();reconnect_auto_intent=false;
            }
            return readable?DONGLE_TRANSPORT:DONGLE_UNSUPPORTED;
        }
        reconnect_policy_supported=true;reconnect_automatic=automatic;
        if (!automatic && reconnect_auto_intent) {
            if (!a2dp_is_connected_flag) aacp_disconnect();
            aacp_routing_override();reconnect_auto_intent=false;
        }
    }
    if (!reconnect_policy_supported) return DONGLE_UNSUPPORTED;
    *current=reconnect_automatic;
    return DONGLE_ACCEPTED;
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

static uint8_t playback_input(void) {
    return is_usb_streaming ? DONGLE_INPUT_OWNERSHIP_USB :
        (phone_active || call_active) ? DONGLE_INPUT_OWNERSHIP_PHONE : DONGLE_INPUT_OWNERSHIP_IDLE;
}
uint8_t dongle_playback_configure(uint8_t codec, uint8_t rate_mode, uint32_t bitrate) {
    playback_options_t next={codec,rate_mode,bitrate};
    if (!playback_options_valid(next)) return DONGLE_UNSUPPORTED;
    if (playback_options.codec==codec && playback_options.rate_mode==rate_mode && playback_options.bitrate==bitrate)
        return DONGLE_ACCEPTED;
    if (is_usb_streaming || phone_active || phone_opened || aacp_mic_requested() || aacp_mic_active() ||
        profile_pending || a2dp_connect_pending || playback_game.changed) return DONGLE_BUSY;
    const uint8_t *peer=(const uint8_t *)get_device_addr();
    if (!peer || !peer_address_valid(peer)) return DONGLE_DISCONNECTED;
    if (!playback_profile_preflight(next,peer)) return DONGLE_UNSUPPORTED;
    if (!a2dp_is_connected_flag) { playback_options=next; return DONGLE_ACCEPTED; }
    if (!aacp_audio_owned()) return DONGLE_BUSY;
    profile_previous=playback_options; playback_options=next;
    memcpy(profile_peer,peer,sizeof(profile_peer));
    profile_pending=true;
    profile_since=btstack_run_loop_get_time_ms();
    if (a2dp_source_disconnect(media_tracker.avdtp_cid)!=ERROR_CODE_SUCCESS) {
        playback_options=profile_previous; profile_pending=false; return DONGLE_BUSY;
    }
    profile_reconnect=true;
    a2dp_demo_timer_stop(&media_tracker);
    return DONGLE_ACCEPTED;
}

void dongle_playback_mic_request_changed(void) {
    playback_encoder_invalidate();
    mic_prepare_open=false;
}
bool dongle_playback_mic_ready(void) {
    if (!mic_prepare_open || mic_prepare_session!=aacp_control_session() || !aacp_model_known() ||
        !a2dp_is_connected_flag || !media_tracker.streaming ||
        playback_idle_phase!=PLAYBACK_RUNNING || mic_recovery_pending ||
        playback_adjust_busy(&playback_adjust) || playback_game.changed ||
        playback_game.waiting_tx || playback_game.waiting_report || playback_game.restore_failed)
        return false;
    if (!playback_game_eligible()) return true;
    return aacp_receive_buffer_serial()!=mic_prepare_serial &&
        aacp_receive_buffer_last()==150 && playback_game.target==150 &&
        (uint32_t)(btstack_run_loop_get_time_ms()-playback_game.sent_since)>=500u;
}
bool dongle_playback_mic_recover(void) {
    if (!a2dp_is_connected_flag || !media_tracker.local_seid || !finish_scan_avdtp_codec)
        return false;
    if (!mic_recovery_pending) {
        mic_recovery_pending=true; mic_recovery_started=false;
        mic_recovery_cid=media_tracker.avdtp_cid;
        mic_recovery_since=btstack_run_loop_get_time_ms();
    }
    return true;
}
uint32_t dongle_playback_mic_recovery_revision(void) {
    return mic_recovery_revision;
}
void dongle_playback_mic_recovery_abort(void) {
    mic_recovery_pending=mic_recovery_started=false;
    aacp_mic_recovery_failed();
    aacp_disconnect();
    if (media_tracker.avdtp_cid) a2dp_source_disconnect(media_tracker.avdtp_cid);
}

uint8_t dongle_playback_game_mode(bool enabled) {
    if (enabled && !playback_game_eligible()) return DONGLE_UNSUPPORTED;
    bool changed = playback_game.requested != enabled;
    if (changed) {
        playback_usb_verify_cancel();
        playback_continuity_reset(&phone_continuity);
    }
    if (changed)
        playback_adjust_cancel_stream(&media_tracker,btstack_run_loop_get_time_ms(),
                                      playback_adjust_interval(&playback_adjust));
    playback_game_request(&playback_game,enabled);
    if (changed) bt_hci_priority_transition(enabled ? PRIORITY_ARM : PRIORITY_OFF);
    return DONGLE_ACCEPTED;
}
uint8_t dongle_playback_game_toggle(void) {
    bool next=!playback_game.requested;
    uint8_t result=dongle_playback_game_mode(next);
    if(result==DONGLE_ACCEPTED)set_led_game_feedback(next);
    return result;
}
#if AIRPODS_HEAD_TRACKING
bool dongle_playback_game_timing(void) {
    return playback_game.requested || playback_game.changed || playback_game.phase == GAME_RESTORING;
}
#endif
void dongle_playback_receive_buffer_sent(uint32_t now) {
    printf("[GAME] receive buffer sent: %u ms\n", aacp_receive_buffer_last());
    playback_game_observe_send(&playback_game, now, aacp_receive_buffer_serial(),
                              aacp_receive_buffer_last(), delay_revision);
    playback_usb_verify_sent();
}
static void playback_reconnect_poll(uint32_t now) {
    const uint8_t *peer = (const uint8_t *)get_device_addr();
    if (passive_attempt.cid) {
        uint16_t cid=passive_attempt.cid;
        if (!a2dp_connect_pending || a2dp_is_connected_flag || media_tracker.avdtp_cid!=cid ||
            !peer || memcmp(peer,passive_attempt.peer,6) || profile_pending || playback_released)
            passive_attempt.cid=0;
        else if ((uint32_t)(now-passive_attempt.since)>=60000u && !aacp_takeover_pending(peer)) {
            // BTstack can leave the AVDTP object waiting when allocating its
            // signaling L2CAP channel failed after SDP. Retire only this pre-open profile.
            const avdtp_connection_t *pending=avdtp_get_connection_for_avdtp_cid(cid);
            passive_attempt.since=now;
            if (pending && !memcmp(pending->remote_addr,peer,6) &&
                (pending->state==AVDTP_SIGNALING_CONNECTION_OPENED ||
                 pending->state==AVDTP_SIGNALING_CONNECTION_W4_L2CAP_DISCONNECTED)) return;
            passive_attempt.cid=0;
            a2dp_connect_pending=false;
            media_tracker.avdtp_cid=0;
            reconnect_since=now;
            if (pending && !memcmp(pending->remote_addr,peer,6)) {
                uint8_t status=avdtp_disconnect(cid); // Emits failure synchronously before finalizing.
                aacp_route_avdtp_event(3,status);
            }
            return;
        }
    }
    if (a2dp_is_connected_flag) reconnect_delay = 1000;
    if (a2dp_is_connected_flag || a2dp_connect_pending || !a2dp_accept_incoming ||
        playback_released || (!reconnect_automatic && !aacp_takeover_pending(peer)) ||
        profile_pending || profile_reconnect || !peer || !peer_address_valid(peer)) {
        reconnect_since = now;
        return;
    }
    if ((uint32_t)(now - reconnect_since) < reconnect_delay) return;
    reconnect_since = now;
    if (reconnect_delay < 8000) reconnect_delay *= 2;
    if ((reconnect_auto_intent || !aacp_takeover_pending(peer)) &&
        (usb_mic_requested() || aacp_mic_requested() || aacp_mic_active() || call_active || call_opened ||
            mic_prepare_open || mic_recovery_pending || phone_output_recovery || suspend_recovery_pending ||
            playback_game.waiting_tx || playback_game.waiting_report || playback_game.restore_failed ||
            playback_adjust_busy(&playback_adjust) || playback_idle_phase==PLAYBACK_SUSPENDING ||
            playback_idle_phase==PLAYBACK_STARTING || playback_idle_phase==PLAYBACK_CLAIMING)) return;
    if (!aacp_takeover_pending(peer)) {
        uint8_t saved[6], slot=read_uint8_last_flash();
        if (slot==1) read_slot1_mac(saved);
        else if (slot==2) read_slot2_mac(saved);
        else return;
        link_key_t key;
        link_key_type_t type;
        // BTstack's lookup takes a non-const address it never writes; hand it a copy.
        uint8_t lookup_address[6];
        memcpy(lookup_address, peer, sizeof lookup_address);
        if (memcmp(saved,peer,6) || !gap_get_link_key_for_bd_addr(lookup_address,key,&type)) return;
        reconnect_auto_intent=true;
        connect_selected_media(peer);
        if (a2dp_connect_pending && media_tracker.avdtp_cid) {
            memcpy(passive_attempt.peer,peer,6);
            passive_attempt.cid=media_tracker.avdtp_cid;
            passive_attempt.since=now;
        }
        return;
    }
    avdtp_source_establish_stream();
}
void dongle_playback_poll(void) {
    playback_accessory_poll();
    const uint8_t *peer=(const uint8_t *)get_device_addr();
    uint32_t now=btstack_run_loop_get_time_ms();
    if (playback_release_pending && (!playback_released || !peer || memcmp(peer,release_peer,6) ||
        aacp_routing_state().revision!=release_revision || aacp_control_session()!=release_session ||
        media_tracker.avdtp_cid!=release_cid)) playback_release_pending=playback_released=false;
    if (playback_release_pending && (aacp_mic_requested() || aacp_mic_active() || call_active || call_opened)) {
        playback_release_pending=playback_released=false;
        aacp_routing_override();
    }
    // Recovery duty time is sampled here, with one poll interval of boundary uncertainty.
    if (playback_work_metrics.recovery_active)
        playback_work_metrics.recovery_poll_ms += now - playback_work_metrics.recovery_last_ms;
    playback_work_metrics.recovery_last_ms = now;
    playback_work_metrics.recovery_active = suspend_recovery_pending || phone_output_recovery;
    (void)playback_usb_verify_poll(now); // Revoke a stale verification before worker callbacks.
    if(inspect_enabled && (int32_t)(now-inspect_deadline)>=0)inspect_enabled=false;
    playback_bitrate_poll(now);
    playback_audio_work(now);
    if (playback_phone_recovery_poll()) { bt_hci_airpods_flush_timeout(false); return; }
    // PCM preparation can accrue a newer clock tick before policy changes its interval.
    now=btstack_run_loop_get_time_ms();
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
        playback_adjust_cancel_stream(&media_tracker,now,playback_adjust_interval(&playback_adjust));
        bool requested=playback_game.requested;
        memset(&playback_game,0,sizeof(playback_game)); playback_game.requested=requested;
        if (peer) memcpy(game_peer,peer,6); else memset(game_peer,0,6);
    }
    uint32_t session=aacp_control_session();
    bool fresh_open=!mic_prepare_open;
    if (!aacp_mic_requested()) mic_prepare_open=false;
    else if (fresh_open || mic_prepare_session!=session) {
        mic_prepare_open=true; mic_prepare_session=session;
        mic_prepare_serial=aacp_receive_buffer_serial();
        // A fresh USB open may retry one failed normal-buffer receipt, never old uplink cleanup.
        if (fresh_open && playback_game.restore_failed && !aacp_mic_active()) {
            playback_game.restore_failed=false;
            playback_game.waiting_tx=playback_game.waiting_report=false;
        }
    }
    if (!a2dp_is_connected_flag || (mic_recovery_pending && mic_recovery_cid!=media_tracker.avdtp_cid))
        mic_recovery_pending=mic_recovery_started=false;
    if (mic_recovery_pending) {
        if ((uint32_t)(now-mic_recovery_since)>=8000u) {
            dongle_playback_mic_recovery_abort();
            bt_hci_airpods_flush_timeout(false);
            return;
        }
        if (!mic_recovery_started && (playback_idle_phase==PLAYBACK_SUSPENDED ||
            (media_tracker.streaming && playback_idle_phase==PLAYBACK_RUNNING))) {
            mic_recovery_started=true;
            if (media_tracker.streaming) playback_transport_recover(&media_tracker);
        }
    }
    if (session!=game_session) {
        playback_adjust_cancel_stream(&media_tracker,now,playback_adjust_interval(&playback_adjust));
        playback_game_session_lost(&playback_game); game_session=session;
    }
    // USB/Game Mode receive-buffer policy; suspended preparation remains unchanged.
    bool claiming=playback_claim.active;
    bool live=!playback_released && is_usb_streaming && !phone_active && !call_active && media_tracker.streaming && playback_pcm_warm() &&
        playback_game_eligible() && aacp_control_ready() && !aacp_mic_requested() && !aacp_mic_active();
    if (!live) playback_adjust_cancel_stream(&media_tracker,now,playback_adjust_interval(&playback_adjust));
    uint8_t previous_interval=playback_adjust_interval(&playback_adjust);
    playback_adjust_poll(&playback_adjust,now);
    if (playback_adjust.failed) {
        playback_adjust_cancel_stream(&media_tracker,now,previous_interval);
        playback_game.failed=true;
    }
    if (live && !usb_game_verification.needed && playback_game.waiting_report &&
        (uint32_t)(now-playback_game.sent_since)<2000 &&
        delay_revision!=playback_game.report_revision && reported_delay && reported_delay!=UINT16_MAX) {
        playback_continuity_reset(&phone_continuity);
        playback_adjust_request(&playback_adjust,reported_delay,playback_game.target);
        playback_adjust.automatic_refill=playback_game.returning_to_usb &&
            playback_adjust.pending && playback_adjust.interval==9 && playback_game.target==60;
        playback_game.returning_to_usb=false;
    }
    // USB can adjust live; a phone source still prepares while suspended.
    bool active=!claiming && !playback_released && !aacp_mic_requested() && !aacp_mic_active() &&
        (!media_tracker.streaming || playback_game.changed || live);
    bool available=!claiming && !profile_pending && !a2dp_connect_pending &&
        aacp_control_ready() && aacp_audio_owned();
    uint16_t target=playback_game_step(&playback_game,available,playback_game_eligible(),active,now,delay_revision,reported_delay);
    bool verify_usb = playback_usb_verify_poll(now);
    if (!target && verify_usb) target = 60;
    // A capture opened before Game Mode ever ran still needs a fresh normal-buffer receipt.
    if (mic_prepare_open && playback_game_eligible() && aacp_control_ready() &&
        !aacp_mic_uplink_active() && !playback_game.changed &&
        !playback_game.waiting_tx && !playback_game.waiting_report && !playback_game.restore_failed &&
        (aacp_receive_buffer_serial()==mic_prepare_serial || aacp_receive_buffer_last()!=150)) target=150;
    if (target && !claiming) {
        playback_continuity_reset(&phone_continuity);
        playback_adjust_cancel_stream(&media_tracker,now,playback_adjust_interval(&playback_adjust));
        playback_game_t previous=playback_game;
        // BTstack can send synchronously; arm the receipt before enqueueing.
        playback_game_queued(&playback_game,target,now,aacp_receive_buffer_serial());
        if (verify_usb && target == 60) {
            usb_game_verification.needed = false;
            usb_game_verification.queued = true;
            usb_game_verification.tx_revision = playback_game.tx_revision;
        }
        if (aacp_receive_buffer(target)!=DONGLE_ACCEPTED) {
            playback_game=previous;
            if (verify_usb && target == 60) {
                usb_game_verification.needed = true;
                usb_game_verification.queued = false;
            }
        }
    }
    if (playback_adjust_busy(&playback_adjust))
        playback_game.phase=playback_adjust.target==60 ? GAME_PENDING : GAME_RESTORING;
    else if (!playback_game.waiting_tx && !playback_game.waiting_report) {
        if (playback_game.failed) playback_game.phase=GAME_FAILED;
        else if (playback_game.phase==GAME_PENDING && playback_game.changed)
            playback_game.phase=GAME_CONFIRMED;
    }
    if (playback_release_pending && !playback_game.changed && !playback_game.waiting_tx &&
        !playback_game.waiting_report && !playback_game.restore_failed && !playback_adjust_busy(&playback_adjust))
        playback_release_close();
    bt_hci_airpods_flush_timeout(available && playback_game_eligible() &&
        playback_game.requested && playback_game.low_latency_confirmed &&
        !playback_game.failed && !playback_game.restore_failed &&
        !aacp_mic_requested() && !aacp_mic_active());
    playback_idle_poll(now);
    playback_reconnect_poll(now);
    avrcp_reconnect_poll(now);
}
void dongle_playback_work_report(void) {
    uint32_t now = btstack_run_loop_get_time_ms();
    bool eld = cur_codec == 4 && handleAAC && a2dp_is_connected_flag;
    printf("[OUTPUT_WORK] ms=%lu codec=%u input=%u aac_calls=%lu aac_us=%lu aac_jobs=%lu aac_job_us=%lu sent_packets=%lu sent_samples=%lu send_wait_us=%lu cancelled_waits=%lu cancelled_wait_us=%lu wait_pending=%u wait_ms=%lu recoveries=%lu recovery_poll_ms=%lu recovery_active=%u\n",
        (unsigned long)now, (unsigned)dongle_output_codec(), (unsigned)playback_input(),
        (unsigned long)playback_work_metrics.aac_calls, (unsigned long)playback_work_metrics.aac_us,
        (unsigned long)playback_work_metrics.aac_jobs, (unsigned long)playback_work_metrics.aac_job_us,
        (unsigned long)playback_work_metrics.sent_packets, (unsigned long)playback_work_metrics.sent_samples,
        (unsigned long)playback_work_metrics.send_wait_us, (unsigned long)playback_work_metrics.cancelled_waits,
        (unsigned long)playback_work_metrics.cancelled_wait_us, (unsigned)(media_tracker.codec_ready_to_send != 0),
        (unsigned long)(media_tracker.codec_ready_to_send ? now - media_tracker.send_wait_start : 0),
        (unsigned long)playback_metrics.recoveries, (unsigned long)playback_work_metrics.recovery_poll_ms,
        (unsigned)playback_work_metrics.recovery_active);
    printf("[OUTPUT_RATE] eld=%u current_bps=%lu target_bps=%lu ceiling_bps=%lu changes=%lu errors=%lu active=%u frozen=%u reason=%u\n",
        (unsigned)eld, (unsigned long)(eld ? playback_bitrate.current : 0),
        (unsigned long)(eld ? playback_bitrate.target : 0),
        (unsigned long)(eld ? playback_bitrate.ceiling : 0), (unsigned long)playback_bitrate.changes,
        (unsigned long)playback_bitrate.errors, (unsigned)playback_bitrate.active,
        (unsigned)playback_bitrate.frozen, (unsigned)playback_bitrate.reason);
    printf("[TAKEOVER] phase=%u claim_active=%u session=%lu revision=%lu cid=0x%04x seid=%u age_ms=%lu\n",
        (unsigned)playback_idle_phase, (unsigned)playback_claim.active,
        (unsigned long)playback_claim.session, (unsigned long)playback_claim.revision,
        (unsigned)playback_claim.cid, (unsigned)playback_claim.seid,
        (unsigned long)(playback_claim.active ? now-playback_idle_transition_since : 0));
    phone_sink_loss_report();
}

uint16_t dongle_playback_adaptive_snapshot(uint8_t *out, uint16_t capacity) {
    if (!out || capacity < 64) return 0;
    memset(out, 0, 64); out[0] = 1;
    bool eld = cur_codec == 4 && handleAAC && a2dp_is_connected_flag;
    out[1] = eld ? (playback_bitrate.active ? 1u : 0u) | (playback_bitrate.frozen ? 2u : 0u) |
        (playback_bitrate.fault ? 4u : 0u) | (playback_bitrate.variable ? 8u : 0u) : 0;
    out[2] = eld ? playback_bitrate.reason : PLAYBACK_RATE_OFF;
    dongle_put32(out + 4, eld ? playback_bitrate.current : 0);
    dongle_put32(out + 8, eld ? playback_bitrate.ceiling : 0);
    dongle_put32(out + 12, eld ? playback_bitrate.target : 0);
    dongle_put32(out + 16, playback_bitrate.changes);
    dongle_put32(out + 20, playback_bitrate.errors);
    dongle_put32(out + 24, playback_bitrate.last_change);
    dongle_put32(out + 28, playback_bitrate.window_flushes);
    dongle_put32(out + 32, playback_bitrate.window_pressure);
    dongle_put32(out + 36, playback_bitrate.window_elapsed);
    dongle_put32(out + 40, 128000u);
    dongle_put32(out + 44, playback_bitrate.active && !playback_bitrate.frozen ?
        btstack_run_loop_get_time_ms() - playback_bitrate.clean_since : 0);
    dongle_put32(out + 52, bitrate_pressure);
    dongle_put32(out + 56, playback_bitrate.last_flushes);
    dongle_put32(out + 60, bitrate_radio_epoch);
    return 64;
}
uint16_t dongle_playback_status(uint8_t *out, uint16_t capacity) {
    if (!out || capacity<DONGLE_AUDIO_PROFILE_SIZE) return 0;
    memset(out,0,DONGLE_AUDIO_PROFILE_SIZE);
    out[DONGLE_AUDIO_PROFILE_OFFSET_VERSION]=DONGLE_AUDIO_PROFILE_VERSION;
    out[DONGLE_AUDIO_PROFILE_OFFSET_REQUESTED_CODEC]=playback_options.codec;
    out[DONGLE_AUDIO_PROFILE_OFFSET_ACTUAL_CODEC]=dongle_output_codec();
    out[DONGLE_AUDIO_PROFILE_OFFSET_RATE_MODE]=playback_options.rate_mode;
    dongle_put32(out+DONGLE_AUDIO_PROFILE_OFFSET_REQUESTED_BITRATE,playback_options.bitrate);
    dongle_put32(out+DONGLE_AUDIO_PROFILE_OFFSET_ACTUAL_BITRATE,actual_bitrate);
    out[DONGLE_AUDIO_PROFILE_OFFSET_GAME_MODE_REQUESTED]=playback_game.requested;
    out[DONGLE_AUDIO_PROFILE_OFFSET_GAME_MODE_STATE]=playback_game.phase;
    out[DONGLE_AUDIO_PROFILE_OFFSET_INPUT_OWNERSHIP]=playback_input();
    out[DONGLE_AUDIO_PROFILE_OFFSET_FLAGS]=(profile_pending ? DONGLE_AUDIO_PROFILE_FLAG_PROFILE_PENDING : 0) |
        (media_tracker.streaming ? DONGLE_AUDIO_PROFILE_FLAG_STREAMING : 0) |
        ((aacp_mic_requested() || aacp_mic_active()) ? DONGLE_AUDIO_PROFILE_FLAG_MICROPHONE : 0) |
        (playback_game_eligible() ? DONGLE_AUDIO_PROFILE_FLAG_GAME_MODE_SUPPORTED : 0);
    // reported_delay is UINT16_MAX until a report arrives, which is the protocol's unknown delay.
    out[DONGLE_AUDIO_PROFILE_OFFSET_DELAY_TENTHS_MILLISECONDS]=(uint8_t)reported_delay;
    out[DONGLE_AUDIO_PROFILE_OFFSET_DELAY_TENTHS_MILLISECONDS+1]=(uint8_t)(reported_delay>>8);
    out[DONGLE_AUDIO_PROFILE_OFFSET_STARTUP_PRIMING_MILLISECONDS]=DONGLE_AUDIO_PROFILE_STARTUP_PRIMING_MILLISECONDS;
    return DONGLE_AUDIO_PROFILE_SIZE;
}
uint16_t dongle_playback_inspect(uint8_t *out,uint16_t capacity) {
#ifdef HAVE_AAC_FDK
    if(cur_codec!=4)return 0;
    critical_section_enter_blocking(&playback_pcm_lock);
    uint16_t size=playback_inspect_pack(&playback_inspect,out,capacity,time_us_32(),
        media_tracker.rtp_timestamp,aacinf.confBuf,aacinf.confSize);
    if(size && inspect_enabled)out[2]|=2u;
    critical_section_exit(&playback_pcm_lock);
    return size;
#else
    (void)out;(void)capacity;return 0;
#endif
}
void dongle_playback_inspect_enable(bool enabled) {
    critical_section_enter_blocking(&playback_pcm_lock);
    inspect_deadline=btstack_run_loop_get_time_ms()+120000u;
    inspect_enabled=enabled;
    critical_section_exit(&playback_pcm_lock);
}
uint16_t dongle_playback_diagnostics(uint8_t *out, uint16_t capacity) {
    if (!out || capacity<DONGLE_PERFORMANCE_SIZE) return 0;
    memset(out,0,DONGLE_PERFORMANCE_SIZE);
    out[DONGLE_PERFORMANCE_OFFSET_VERSION]=DONGLE_PERFORMANCE_VERSION;
    out[DONGLE_PERFORMANCE_OFFSET_INPUT_OWNERSHIP]=playback_input();
    out[DONGLE_PERFORMANCE_OFFSET_ACTUAL_CODEC]=dongle_output_codec();
    out[DONGLE_PERFORMANCE_OFFSET_FLAGS]=(media_tracker.streaming ? DONGLE_PERFORMANCE_FLAG_STREAMING : 0) |
        ((aacp_mic_requested() || aacp_mic_active()) ? DONGLE_PERFORMANCE_FLAG_MICROPHONE : 0) |
        (phone_sink_connected() ? DONGLE_PERFORMANCE_FLAG_PHONE_CONNECTED : 0) |
        (aacp_control_ready() ? DONGLE_PERFORMANCE_FLAG_AACP_READY : 0);
    dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_UPTIME_MILLISECONDS,btstack_run_loop_get_time_ms());
    if (playback_pcm_initialized) {
        critical_section_enter_blocking(&playback_pcm_lock);
        dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_PCM_FRAMES_USED,playback_pcm.used);
        dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_PCM_FRAMES_CAPACITY,playback_pcm.capacity);
        dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_PCM_FRAMES_HIGH_WATER_MARK,playback_metrics.pcm_peak);
        dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_PCM_UNDERFLOW_FRAMES,playback_metrics.pcm_underflows);
        dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_PCM_DROPPED_FRAMES,playback_metrics.pcm_drops);
        critical_section_exit(&playback_pcm_lock);
    }
    dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_ENCODED_UNITS,playback_metrics.encoded_units);
    dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_LAST_ENCODE_MICROSECONDS,playback_metrics.encoder_last_us);
    dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_MAXIMUM_ENCODE_MICROSECONDS,playback_metrics.encoder_max_us);
    dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_SEND_FAILURES,playback_metrics.send_failures);
    dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_RECOVERY_REQUESTS,playback_metrics.recoveries);
    extern char end[], __StackLimit[];
    uint32_t heap=(uint32_t)((uintptr_t)__StackLimit-(uintptr_t)end);
    struct mallinfo allocations=mallinfo();
    dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_LINKED_HEAP_CAPACITY,heap);
    dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_HEAP_ALLOCATED_BYTES,(uint32_t)allocations.uordblks);
    dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_HEAP_REMAINING_BYTES,(uint32_t)allocations.uordblks<=heap ?
        heap-(uint32_t)allocations.uordblks : DONGLE_PERFORMANCE_UNAVAILABLE);
    dongle_put32(out+DONGLE_PERFORMANCE_OFFSET_ALLOCATOR_ARENA_BYTES,(uint32_t)allocations.arena);
    return DONGLE_PERFORMANCE_SIZE;
}

void dongle_playback_usb_packet(uint32_t us, uint16_t frames, uint8_t format, uint8_t error) {
    uint32_t irq=save_and_disable_interrupts();
    if(error) {
        ++playback_trace.stats[error==2?TRACE_USB_SHORT:TRACE_USB_BAD];
        playback_trace_record(&playback_trace,us,TRACE_USB_ERROR,error,frames);
    } else {
        if(usb_last_arrival_us) {
            uint32_t gap=us-usb_last_arrival_us;
            if(gap>playback_trace.stats[TRACE_USB_GAP_US])playback_trace.stats[TRACE_USB_GAP_US]=gap;
        }
        usb_last_arrival_us=us;
        ++playback_trace.stats[TRACE_USB_PACKETS];
        playback_trace.stats[TRACE_USB_FRAMES]+=frames;
    }
    (void)format;
    restore_interrupts(irq);
}
void dongle_playback_usb_format(uint8_t format) {
    uint32_t irq=save_and_disable_interrupts();
    if(playback_trace.stats[TRACE_USB_FORMAT]!=format)
        playback_trace_record(&playback_trace,time_us_32(),TRACE_FORMAT,format,0);
    playback_trace.stats[TRACE_USB_FORMAT]=format;
    restore_interrupts(irq);
}
void dongle_playback_controller_event(const uint8_t *p,uint16_t size) {
    if(!p || size<2 || (unsigned)p[1]+2!=size)return;
    if(p[0]==0x11 && size==4) {
        PB_ADD(TRACE_FLUSHES,1);
        PB_EVENT(TRACE_FLUSH,(uint32_t)p[2]|(uint32_t)p[3]<<8,0);
    } else if(p[0]==0x13 && size>=3 && size==3u+4u*p[2]) {
        uint32_t count=0;
        for(unsigned i=0;i<p[2];++i)count+=(uint32_t)p[5+4*i]|(uint32_t)p[6+4*i]<<8;
        PB_ADD(TRACE_COMPLETED,count);
    }
}
uint16_t dongle_playback_trace(uint8_t *out,uint16_t capacity) {
    if(!out || capacity<PLAYBACK_TRACE_SIZE)return 0;
    ble_scan_stats_t scan;
    ble_scan_get_stats(&scan);
    uint32_t irq=save_and_disable_interrupts();
    playback_trace.stats[TRACE_BLE_SCANNING]=scan.scan_requested;
    uint16_t result=playback_trace_pack(&playback_trace,out,capacity,time_us_32(),
        playback_pcm.used,media_tracker.samples_ready,playback_game.phase,media_tracker.rtp_timestamp);
    restore_interrupts(irq);
    return result;
}


int btstack_main(int argc, const char * argv[]){
    // BTstack's conventional entry signature; main.c passes no arguments.
    (void)argc;
    (void)argv;

    playback_reconnect_policy_load();

#if AIRPODS_EXPERIMENTAL_RADIO0076
    radio0076_register_gate();
#endif
    l2cap_init();
    rfcomm_init();
    aacp_init();
    ble_scan_init();
    bt_hci_init();

    // Initialize AVDTP Sink
    avdtp_source_init();
    avdtp_source_register_packet_handler(&packet_handler);

    avrcp_connection_init();

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

    avrcp_connection_register_service_records();

    if (!phone_sink_init()) return 1;
    if (!phone_call_init()) return 1;


#ifdef HAVE_BTSTACK_STDIN
    bluetooth_debug_console_init();
#endif

    // turn on!
    hci_power_control(HCI_POWER_ON);

    return 0;

}
