// Production job functions; callbacks run during the unlocked fake FDK call.
#define PLAYBACK_ENCODER_JOB
#include "playback_timing_test.c"
#undef playback_prepare_packet
#include "playback_encoder_lifecycle.inc"

static void ready_job(void) {
    initialize(4,480,324,10,986);
    is_usb_streaming=true;allow_recovery=true;
    media_tracker.avdtp_cid=42;media_tracker.local_seid=5;
    media_tracker.samples_ready=960;
    playback_game.changed=true;playback_game.target=60;
    assert(fill_aac_audio_buffer(&media_tracker)==480);
    assert(playback_encoder_busy() && consumed==480 && media_tracker.samples_ready==480);
    assert(!encode_calls && !media_tracker.codec_num_frames && !media_tracker.rtp_timestamp);
}
static void run_unlocked(void) {
    assert(bt_locked);bt_locked=false;dongle_playback_encode_run();bt_locked=true;
}
static void close_during_encode(void) {
    bt_locked=true;playback_encoder_close();
    assert(handleAAC && !encoder_closes && encoder_job.close_pending);
    bt_locked=false;
}
static void reset_during_encode(void) {
    bt_locked=true;
    assert(aaceld_encoder_init());
    assert(!encoder_closes && !encoder_resets && encoder_job.reset_pending);
    bt_locked=false;
}
static void invalidate_usb(void) {
    ++usb_playback_revision;playback_encoder_invalidate();
}
static void reset_at_commit_entry(void) {
    pcm_lock_hook=NULL;invalidate_usb();
}
static void timer_during_encode(void) {
    btstack_timer_source_t timer={.context=&media_tracker};
    bt_locked=true;now=10;avdtp_audio_timeout_handler(&timer);bt_locked=false;
}
static void defer_during_encode(void) {
    uint8_t event[6]={0,0,0,42,0,17};
    bt_locked=true;
    assert(playback_encoder_defer_configuration(event,sizeof(event)));
    event[5]=99; // The production helper must own a copy, not this stack buffer.
    assert(!configuration_callbacks && !encoder_closes);
    bt_locked=false;
}
static void defer_then_reset_during_encode(void) {
    defer_during_encode();
    bt_locked=true;
    assert(aaceld_encoder_init() && encoder_job.deferred_size && !encoder_job.reset_pending);
    bt_locked=false;
}
static void prepare_run_commit_is_one_unit(void) {
    ready_job();
    assert(fill_aac_audio_buffer(&media_tracker)==0 && consumed==480);
    a2dp_demo_send_media_packet();assert(!emitted && !encode_calls);
    encode_hook=timer_during_encode;run_unlocked();
    assert(encode_calls==1 && media_tracker.samples_ready==960);
    assert(!media_tracker.codec_num_frames && aaceld_frame_sequence==4095);
    dongle_playback_encode_complete();
    assert(!playback_encoder_busy() && media_tracker.samples_ready==960);
    assert(media_tracker.codec_num_frames==1 && media_tracker.codec_storage_count==324);
    assert(aaceld_frame_sequence==0 && !media_tracker.rtp_timestamp);
    encode_hook=NULL;
    synchronous_request=true;
    playback_prepare_packet(&media_tracker,now);run_unlocked();dongle_playback_encode_complete();
    assert(encode_calls==2 && consumed==960 && media_tracker.samples_ready==480);
    assert(!playback_encoder_busy() && encode_calls==2 && emitted==960);
    assert(media_tracker.rtp_timestamp==960 && aaceld_frame_sequence==1);
}
static void cancellation_fences_every_scope(void) {
    for(unsigned kind=0;kind<7;++kind) {
        ready_job();run_unlocked();
        if(kind==0)playback_encoder_invalidate();
        if(kind==1)++control_session;
        if(kind==2)++usb_playback_revision;
        if(kind==3)is_usb_streaming=false;
        if(kind==4)a2dp_is_connected_flag=false;
        if(kind==5)audio_owned=false;
        if(kind==6)pcm_lock_hook=reset_at_commit_entry;
        dongle_playback_encode_complete();
        assert(!media_tracker.codec_num_frames && !media_tracker.codec_storage_count);
        assert(!emitted && aaceld_frame_sequence==4095 && encoder_job.cancelled==1);
        audio_owned=true;a2dp_is_connected_flag=true;
    }
    ready_job();playback_encoder_invalidate();run_unlocked();dongle_playback_encode_complete();
    assert(!encode_calls && encoder_job.cancelled==1);
    initialize(4,480,324,10,986);is_usb_streaming=true;media_tracker.samples_ready=480;
    pcm_read_hook=invalidate_usb;
    assert(fill_aac_audio_buffer(&media_tracker)==480);
    run_unlocked();dongle_playback_encode_complete();
    assert(!encode_calls && !media_tracker.codec_num_frames && encoder_job.cancelled==1);
}
static void lifecycle_waits_until_encode_returns(void) {
    ready_job();encode_hook=reset_during_encode;run_unlocked();
    assert(!encoder_resets);dongle_playback_encode_complete();
    assert(encoder_resets==1 && !encoder_job.reset_pending && encoder_job.cancelled==1);
    dongle_playback_encode_complete();assert(encoder_resets==1);
    ready_job();encode_hook=defer_then_reset_during_encode;run_unlocked();
    dongle_playback_encode_complete();
    assert(configuration_callbacks==1 && !encoder_resets && !encoder_job.reset_pending);
    ready_job();encode_hook=close_during_encode;run_unlocked();
    assert(!encoder_closes);dongle_playback_encode_complete();
    assert(encoder_closes==1 && !handleAAC && encoder_job.cancelled==1);
    for(unsigned changed=0;changed<5;++changed) {
        ready_job();encode_hook=defer_during_encode;run_unlocked();
        if(changed==1)++control_session;
        if(changed==2)cur_active_device[0]^=1;
        if(changed==3)++media_tracker.avdtp_cid;
        if(changed==4)playback_encoder_invalidate();
        dongle_playback_encode_complete();
        assert(configuration_callbacks==(changed==0?1u:0u));
        if(!changed)assert(last_configuration==17);
        assert(!media_tracker.codec_num_frames && encoder_job.cancelled==1);
    }
}
static void failure_discards_partial_packet(void) {
    for(unsigned kind=0;kind<2;++kind) {
        ready_job();run_unlocked();dongle_playback_encode_complete();
        assert(media_tracker.codec_num_frames==1);
        media_tracker.samples_ready=480;
        assert(fill_aac_audio_buffer(&media_tracker)==480);
        allow_encoder_failure=true;
        if(kind==0)encode_error=1;else encode_consumed_delta=-2;
        run_unlocked();dongle_playback_encode_complete();
        assert(encoder_errors==1 && !media_tracker.streaming);
        assert(!media_tracker.codec_num_frames && !media_tracker.codec_storage_count);
        a2dp_demo_send_media_packet();assert(!emitted);
    }
    ready_job();encode_zero=true;run_unlocked();dongle_playback_encode_complete();
    assert(!encoder_errors && media_tracker.streaming && !media_tracker.codec_num_frames);
    assert(consumed==480 && media_tracker.samples_ready==480);
}
static void cancellation_discards_partial_packet(void) {
    for(unsigned kind=0;kind<4;++kind) {
        ready_job();run_unlocked();dongle_playback_encode_complete();
        assert(media_tracker.codec_num_frames==1 && media_tracker.codec_storage_count==324);
        media_tracker.samples_ready=480;
        assert(fill_aac_audio_buffer(&media_tracker)==480);
        run_unlocked();
        if(kind==0)++usb_playback_revision;
        if(kind==1)++control_session;
        if(kind==2)is_usb_streaming=false;
        if(kind==3)playback_encoder_invalidate();
        dongle_playback_encode_complete();
        assert(!media_tracker.codec_num_frames && !media_tracker.codec_storage_count);
        assert(!media_tracker.codec_ready_to_send && !phone_continuity.armed);
        assert(aaceld_frame_sequence==4095 && !media_tracker.rtp_timestamp);
        a2dp_demo_send_media_packet();assert(!emitted);
    }
}
static void cached_packet_scope_and_fresh_resumption(void) {
    ready_job();run_unlocked();dongle_playback_encode_complete();
    assert(media_tracker.codec_num_frames==1);
    ++usb_playback_revision;
    media_tracker.samples_ready=480;
    assert(fill_aac_audio_buffer(&media_tracker)==480);
    assert(!media_tracker.codec_storage_count && encoder_job.offset==4 && aaceld_frame_sequence==4095);
    run_unlocked();dongle_playback_encode_complete();
    assert(media_tracker.codec_num_frames==1 && aaceld_frame_sequence==0);
    media_tracker.samples_ready=480;
    assert(fill_aac_audio_buffer(&media_tracker)==480);
    run_unlocked();dongle_playback_encode_complete();
    assert(send_requested && media_tracker.codec_num_frames==2);
    send_requested=false;a2dp_demo_send_media_packet();
    assert(emitted==960 && media_tracker.rtp_timestamp==960 && aaceld_frame_sequence==1);

    ready_job();run_unlocked();dongle_playback_encode_complete();
    assert(fill_aac_audio_buffer(&media_tracker)==480);
    run_unlocked();dongle_playback_encode_complete();
    assert(send_requested && media_tracker.codec_ready_to_send);
    ++control_session;
    send_requested=false;a2dp_demo_send_media_packet();
    assert(!emitted && !media_tracker.codec_storage_count && !media_tracker.codec_ready_to_send);
    assert(aaceld_frame_sequence==4095 && !media_tracker.rtp_timestamp);
}
int main(void) {
    prepare_run_commit_is_one_unit();
    cancellation_fences_every_scope();
    lifecycle_waits_until_encode_returns();
    failure_discards_partial_packet();
    cancellation_discards_partial_packet();
    cached_packet_scope_and_fresh_resumption();
    puts("encoder job: unlocked FDK, single PCM debt, packet/RTP order, scope races, deferred lifecycle and failures passed");
}
