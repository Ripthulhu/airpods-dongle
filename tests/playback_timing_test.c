// SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include "../src/btstack/playback_eld.h"

#define HAVE_AAC_FDK
#define AVDTP_CODEC_SBC 1
#define AVDTP_CODEC_MPEG_1_2_AUDIO 2
#define AVDTP_CODEC_ATRAC_FAMILY 3
#define AVDTP_CODEC_NON_A2DP 4
#define AVDTP_CODEC_MPEG_2_4_AAC 5
#define A2DP_CODEC_VENDOR_ID_APPLE 76
#define A2DP_APPLE_CODEC_AAC_ELD 32769
#define ERROR_CODE_SUCCESS 0
#define SUSPEND_RECOVERY_TIMEOUT_MS 3000
#define AUDIO_SLOT_MAX_SAMPLES 1024
#define btstack_assert assert
#define IN_AUDIO_DATA 0
#define OUT_BITSTREAM_DATA 1
#define AACENC_OK 0
typedef int AACENC_ERROR;
typedef struct { int numBufs; void **bufs; int *bufferIdentifiers, *bufSizes, *bufElSizes; } AACENC_BufDesc;
typedef struct { unsigned numInSamples; } AACENC_InArgs;
typedef struct { int numOutBytes; } AACENC_OutArgs;
static void *handleAAC = (void *)1;
static struct { unsigned inputChannels; } aacinf = {2};
static struct { bool rate_mode; } playback_options;
static uint16_t aaceld_frame_sequence;
static uint32_t now, consumed, emitted, frame_size, sample_frames, send_failures;
static bool send_requested, phone_opened, recovery_reset_encoder, suspend_recovery_pending;
static bool allow_recovery, synchronous_request;
static unsigned request_depth;
static unsigned audio_timer_interval, acc_num_simples, cur_codec, aaceld_bitrate = 256000;
enum { PLAYBACK_RUNNING, PLAYBACK_SUSPENDING, PLAYBACK_SUSPENDED, PLAYBACK_STARTING };
static unsigned playback_idle_phase;
typedef struct { void *context; } btstack_timer_source_t;
typedef struct { uint8_t *media_codec_information; } adtvp_media_codec_capabilities_t;
typedef unsigned avdtp_media_codec_type_t;
typedef struct {
    unsigned avdtp_cid, local_seid;
    uint32_t time_audio_data_sent, acc_num_missed_samples, samples_ready;
    btstack_timer_source_t audio_timer;
    bool streaming;
    int max_media_payload_size;
    uint32_t send_wait_start, rtp_timestamp;
    uint8_t codec_storage[986];
    uint16_t codec_storage_count, codec_num_frames;
    uint8_t codec_ready_to_send;
} a2dp_media_sending_context_t;
static a2dp_media_sending_context_t media_tracker;
static bool a2dp_is_connected_flag = true;
static struct { unsigned encoded_units, recoveries, send_failures; } playback_metrics;
static int16_t encoder_pcm[2048];
static struct endpoint {
    struct { struct { unsigned media_codec_type; } media_codec; } remote_configuration;
    struct { struct { adtvp_media_codec_capabilities_t media_codec; } capabilities; } sep;
} endpoint;
static struct { struct endpoint *local_stream_endpoint; } sc = {&endpoint};
static btstack_timer_source_t suspend_recovery_timer;
static void btstack_run_loop_set_timer(void *a, unsigned b) { (void)a; (void)b; }
static void btstack_run_loop_add_timer(void *a) { (void)a; }
static void *btstack_run_loop_get_timer_context(btstack_timer_source_t *t) { return t->context; }
static uint32_t btstack_run_loop_get_time_ms(void) { return now; }
static unsigned a2dp_sample_rate(void) { return 48000; }
static void a2dp_demo_timer_stop(void *a) { (void)a; assert(allow_recovery); media_tracker.streaming = false; }
static unsigned avdtp_source_suspend(unsigned a, unsigned b) { (void)a; (void)b; assert(allow_recovery); return 0; }
static void aacp_disconnect(void) { assert(false); }
static void a2dp_source_disconnect(unsigned a) { (void)a; assert(false); }
static void suspend_recovery_handler(btstack_timer_source_t *a) { (void)a; }
static void btstack_run_loop_set_timer_handler(void *a, void (*b)(btstack_timer_source_t *)) { (void)a; (void)b; }
static unsigned btstack_sbc_encoder_num_audio_frames(void) { return sample_frames; }
static int btstack_sbc_encoder_sbc_buffer_length(void) { return (int)frame_size; }
static uint8_t *btstack_sbc_encoder_sbc_buffer(void) { static uint8_t bytes[128]; return bytes; }
static void btstack_sbc_encoder_process_data(void *a) { (void)a; }
static unsigned time_us_32(void) { return 0; }
static void playback_encode_measured(unsigned a) { (void)a; }
static void audio_pcm_read(unsigned frames) { consumed += frames; }
static void a2dp_demo_send_media_packet(void);
static void a2dp_source_stream_endpoint_request_can_send_now(unsigned a, unsigned b) {
    (void)a; (void)b;
    if (!synchronous_request) { send_requested = true; return; }
    assert(++request_depth == 1); // A failed send must not recursively request another callback.
    a2dp_demo_send_media_packet();
    --request_depth;
}
static unsigned get_vendor_id(void *a) { (void)a; return A2DP_CODEC_VENDOR_ID_APPLE; }
static unsigned get_codec_id(void *a) { (void)a; return A2DP_APPLE_CODEC_AAC_ELD; }
static void playback_media_sent(void) { }
static unsigned a2dp_source_stream_send_media_payload_rtp(unsigned a, unsigned b,
    unsigned c, unsigned timestamp, uint8_t *packet, unsigned length) {
    (void)a; (void)b; (void)c;
    assert(timestamp == emitted);
    assert(length <= (unsigned)media_tracker.max_media_payload_size);
    if (send_failures) { --send_failures; return 1; }
    if (cur_codec == 1) {
        assert(packet[0] >= 1 && packet[0] <= 15);
        assert(length == 1 + packet[0] * frame_size);
        emitted += packet[0] * sample_frames;
    } else {
        if (cur_codec == 4 && phone_opened) assert(media_tracker.codec_num_frames == 1);
        emitted += media_tracker.codec_num_frames * sample_frames;
    }
    return 0;
}
static int playback_encoder_failed(a2dp_media_sending_context_t *context) {
    (void)context; assert(false); return -1;
}
static AACENC_ERROR aacEncEncode(void *handle, AACENC_BufDesc *input, AACENC_BufDesc *output,
    AACENC_InArgs *args, AACENC_OutArgs *result) {
    (void)handle; (void)input;
    assert(args->numInSamples == sample_frames * 2);
    unsigned bytes = frame_size - (cur_codec == 4 ? 4 : 0);
    assert(output->bufSizes[0] >= (int)bytes);
    memset(output->bufs[0], 0, bytes); result->numOutBytes = (int)bytes;
    return AACENC_OK;
}
#include "playback_timing.inc"

static void drain(void) {
    unsigned budget = 100;
    while (send_requested) {
        assert(budget--);
        send_requested = false;
        a2dp_demo_send_media_packet();
        if (media_tracker.codec_ready_to_send == 2) {
            assert(!send_requested); // Failed sends must return before the main loop retries.
            playback_prepare_packet(&media_tracker, now);
        }
    }
}
static void initialize(unsigned codec, unsigned samples, unsigned bytes, unsigned interval, unsigned mtu) {
    memset(&media_tracker, 0, sizeof(media_tracker));
    memset(&playback_metrics, 0, sizeof(playback_metrics));
    now = consumed = emitted = send_failures = 0;
    send_requested = false; phone_opened = false;
    allow_recovery = synchronous_request = false;
    request_depth = 0;
    cur_codec = codec; sample_frames = acc_num_simples = samples;
    frame_size = bytes; audio_timer_interval = interval;
    endpoint.remote_configuration.media_codec.media_codec_type =
        codec == 1 ? AVDTP_CODEC_SBC : codec == 2 ? AVDTP_CODEC_MPEG_2_4_AAC : AVDTP_CODEC_NON_A2DP;
    media_tracker.streaming = true;
    media_tracker.max_media_payload_size = (int)mtu;
}
static void timing(unsigned codec, unsigned samples, unsigned bytes, unsigned interval, unsigned mtu, bool phone) {
    initialize(codec, samples, bytes, interval, mtu);
    phone_opened = phone;
    btstack_timer_source_t timer = {&media_tracker};
    for (unsigned i = 0; i < 1000; ++i) {
        now += interval;
        avdtp_audio_timeout_handler(&timer);
        drain();
    }
    unsigned expected = now * 48;
    assert(consumed <= expected && expected - consumed < samples);
    assert(emitted + media_tracker.codec_num_frames * samples == consumed);
    // A delayed callback bounds stale work; subsequent packets remain valid.
    unsigned before = consumed;
    now += 1000;
    avdtp_audio_timeout_handler(&timer);
    drain();
    assert(consumed - before <= (samples * 4 > 1920 ? samples * 4 : 1920));
    assert(playback_metrics.recoveries == 0);
}
int main(void) {
    timing(1, 128, 118, 10, 656, false); // Ordinary stereo SBC, bitpool 53.
    timing(1, 128, 38, 10, 656, false);  // Valid bitpool 13 must respect the 15-frame header.
    timing(1, 16, 9, 10, 656, false);    // Small blocks need multiple sends per timer tick.
    timing(1, 128, 118, 10, 120, false); // A valid small MTU cannot reduce the playback rate.
    timing(2, 1024, 587, 12, 656, false);
    timing(4, 480, 324, 10, 986, false);
    timing(4, 480, 324, 10, 986, true); // Phone preparation uses one AU even when three would fit.
    initialize(1, 128, 118, 10, 656);
    btstack_timer_source_t timer = {&media_tracker};
    now = 10;
    avdtp_audio_timeout_handler(&timer);
    now = 20;
    avdtp_audio_timeout_handler(&timer);
    send_failures = 2;
    drain();
    assert(emitted && playback_metrics.send_failures == 2);
    initialize(1, 128, 118, 10, 656);
    now = 10; avdtp_audio_timeout_handler(&timer);
    now = 20; avdtp_audio_timeout_handler(&timer);
    send_requested = false; send_failures = 1000;
    a2dp_demo_send_media_packet();
    assert(!send_requested && media_tracker.codec_ready_to_send == 2);
    // A repeated failure returns to the poller without resetting its original deadline.
    unsigned started = media_tracker.send_wait_start;
    synchronous_request = true;
    playback_prepare_packet(&media_tracker, now);
    assert(request_depth == 0);
    assert(!send_requested && media_tracker.codec_ready_to_send == 2);
    assert(media_tracker.send_wait_start == started);
    now = started + 200; allow_recovery = true;
    avdtp_audio_timeout_handler(&timer);
    assert(playback_metrics.recoveries == 1 && suspend_recovery_pending && !media_tracker.streaming);
    puts("playback timing: SBC/AAC/ELD pacing, bounded debt, 15-frame headers, MTU and retries passed");
}
