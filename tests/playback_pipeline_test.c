// Exercise the production PCM ingress/read, timer, encoding preparation and worker together.
#define PLAYBACK_PIPELINE
#include "playback_timing_test.c"
#include "../src/realtime_metrics.h"

static uint32_t generated,encoded,previous;
static uint64_t ingress_fraction;
static int drift_ppm;
static unsigned encode_cost,blocked_until;
static unsigned filter_cost,loop_cost;
static unsigned timer_phase;
static unsigned unlocked_timer_callbacks, encode_timer_callbacks;
static unsigned request_wait,next_wait;
static void pipeline_request(void) {
    if(request_wait && now>=next_wait) {
        // Match the measured send-wait clock, which starts before the last encode.
        blocked_until=media_tracker.send_wait_start+request_wait;
        next_wait=now+500;
    }
}
static bool check_sequence,have_previous;
static bool usb_jitter,variable_cost;
static unsigned delayed_frames;
static unsigned sink_frames,sink_underruns;
static bool sink_started;
static unsigned phone_arrivals,phone_queued,phone_decode_due,phone_decoded;
static bool phone_decoding,phone_output_ready;
static unsigned phone_priming_left,phone_priming_encoded;
static bool precise_input_fixture;
static bool waveform_fixture,waveform_float,waveform_antiphase,waveform_sub_lsb;
static double waveform_hz[2];
static float waveform_output[48000*10][2];
static unsigned waveform_frames;
static unsigned waveform_tempo_cost,waveform_tempo_jobs;
static unsigned waveform_tempo_hundredths,waveform_encoder_hundredths;
static unsigned waveform_tempo_remainder,waveform_encoder_remainder;
static unsigned waveform_previous_tempo_ms,waveform_previous_tempo_interval;
static uint32_t waveform_previous_tempo_generation;
static unsigned stale_tempo_kind,stale_tempo_hooks;
static uint32_t stale_tempo_read,stale_tempo_used;
static int16_t encoder_sample_s16(unsigned index) {
    return cur_codec==4?playback_pcm_quantize((float)encoder_pcm.q31[index]/2147483648.0f):encoder_pcm.s16[index];
}
static bool encoder_matches_s16(const int16_t *expected,unsigned count) {
    for(unsigned i=0;i<count;++i)if(encoder_sample_s16(i)!=expected[i])return false;
    return true;
}
#ifdef PLAYBACK_REAL_PHONE
static void phone_pipeline_encoded(const int16_t *pcm,unsigned frames);
#endif

static void pipeline_received(unsigned frames) {
    sink_frames+=frames;
    if(sink_frames>=2880)sink_started=true; // Receiver starts with the requested 60 ms reserve.
}

static int16_t signal(uint32_t frame) {
    uint32_t phase=frame%48000u;
    return (int16_t)(phase<24000u?(int32_t)phase-12000:36000-(int32_t)phase);
}
static void ingress(unsigned frames) {
    int16_t input[100];
    assert(frames<=50);
    if(waveform_fixture) {
        float samples[100];
        for(unsigned i=0;i<frames;++i) {
            double phase=(double)generated++*6.2831853071795864769/48000.0;
            samples[2*i]=waveform_sub_lsb?0.125f+0x1p-20f:
                (float)(0.3*sin(phase*waveform_hz[0]));
            samples[2*i+1]=waveform_antiphase?-samples[2*i]:
                (float)(0.3*sin(phase*waveform_hz[1]));
            input[2*i]=playback_pcm_quantize(samples[2*i]);
            input[2*i+1]=playback_pcm_quantize(samples[2*i+1]);
        }
        if(waveform_float)audio_slot_push_float(samples,(uint16_t)frames);
        else audio_slot_push_samples(input,(uint16_t)frames);
        return;
    }
    for(unsigned i=0;i<frames;++i) {
        input[2*i]=signal(generated++);
        input[2*i+1]=(int16_t)-input[2*i];
    }
    audio_slot_push_samples(input,(uint16_t)frames);
}
static void phone_ingress(unsigned frames) {
    int16_t input[256];
    while(frames) {
        unsigned count=frames>128?128:frames;
        for(unsigned i=0;i<count;++i) {
            input[2*i]=signal(generated++);input[2*i+1]=(int16_t)-input[2*i];
        }
        playback_pcm_push_measured(input,count);frames-=count;
    }
}
static void advance(unsigned ms) {
    while(ms--) {
        ++now;
        if(sink_started) {
            if(sink_frames<48) {++sink_underruns;sink_frames=0;}
            else sink_frames-=48;
        }
        // Unlike the old locked encode, the new job lets Bluetooth timers run.
        if(!bt_locked && media_tracker.streaming &&
           (int32_t)(now-media_tracker.audio_timer.timeout-timer_phase)>=0) {
            assert(!bt_locked);++unlocked_timer_callbacks;
            if(encode_in_progress)++encode_timer_callbacks;
            bt_locked=true;avdtp_audio_timeout_handler(&media_tracker.audio_timer);bt_locked=false;
        }
        if(phone_active) {
            if(now%15u==0)++phone_arrivals; // 720-frame LHDC packets.
            if(now%20u==0) {
                phone_queued+=phone_arrivals;phone_arrivals=0;
                assert(phone_queued<=8);
            }
            if(phone_decoding && now>=phone_decode_due) {
                --phone_queued;++phone_decoded;phone_decoding=false;phone_output_ready=true;
            }
            if(!phone_decoding && !phone_output_ready && phone_queued) {
                phone_decoding=true;phone_decode_due=now+(phone_decoded%10u?8u:14u);
            }
            continue;
        }
        ingress_fraction+=(uint64_t)48*(1000000+drift_ppm);
        unsigned frames=(unsigned)(ingress_fraction/1000000u);
        ingress_fraction%=1000000u;
        delayed_frames+=frames;
        if(usb_jitter && (now%17==0 || now%29==0))continue;
        while(delayed_frames) {
            unsigned chunk=delayed_frames>49?49:delayed_frames;
            ingress(chunk);delayed_frames-=chunk;
        }
    }
}
static unsigned waveform_fractional_ms(unsigned hundredths,unsigned *remainder) {
    *remainder+=hundredths;
    unsigned whole=*remainder/100;
    *remainder%=100;
    return whole;
}
static void pipeline_encode(AACENC_BufDesc *input,AACENC_InArgs *args) {
    int16_t checked[2048];
    assert(input->bufElSizes[0]==(encoder_job.codec==4?4:2));
    assert(args->numInSamples<=2048);
    if(waveform_fixture) {
        const int32_t *wide=input->bufs[0];
        unsigned frames=args->numInSamples/2;
        assert(input->bufElSizes[0]==4 && !bt_locked && !pcm_locked);
        if(encoder_job.tempo) {
            unsigned interval=encoder_tempo_job.interval;
            if(interval!=10 && interval==waveform_previous_tempo_interval &&
               tempo_generation==waveform_previous_tempo_generation)
                assert((uint32_t)(now-waveform_previous_tempo_ms)>=interval);
            waveform_previous_tempo_ms=now;
            waveform_previous_tempo_interval=interval;
            waveform_previous_tempo_generation=tempo_generation;
            ++waveform_tempo_jobs;
            advance(waveform_tempo_cost+waveform_fractional_ms(waveform_tempo_hundredths,&waveform_tempo_remainder));
        }
        assert(waveform_frames+frames<=sizeof(waveform_output)/sizeof(waveform_output[0]));
        for(unsigned i=0;i<frames;++i)for(unsigned channel=0;channel<2;++channel)
            waveform_output[waveform_frames+i][channel]=(float)((double)wide[2*i+channel]/2147483648.0);
        waveform_frames+=frames;encoded+=frames;
        advance((variable_cost?5u+now%4u:encode_cost)+
            waveform_fractional_ms(waveform_encoder_hundredths,&waveform_encoder_remainder));
        return;
    }
    if(precise_input_fixture) {
        const int32_t *wide=input->bufs[0];
        assert(input->bufElSizes[0]==4 && input->bufSizes[0]==3840 && args->numInSamples==960);
        for(unsigned i=0;i<480;++i) {
            int32_t expected=i<PLAYBACK_FILTER_DELAY?0:268435456+((i-PLAYBACK_FILTER_DELAY)%8+1)*2048;
            assert(wide[2*i]==expected && wide[2*i+1]==-expected);
        }
        encoded+=480;
        return;
    }
    const int16_t *pcm=input->bufs[0];
    if(input->bufElSizes[0]==4) {
        const int32_t *wide=input->bufs[0];
        for(unsigned i=0;i<args->numInSamples;++i)checked[i]=playback_pcm_quantize((float)wide[i]/2147483648.0f);
        pcm=checked;
    }
#ifdef PLAYBACK_REAL_PHONE
    phone_pipeline_encoded(pcm,args->numInSamples/2);
    encoded+=args->numInSamples/2;
#else
    for(unsigned i=0;i<args->numInSamples/2;++i) {
        assert(pcm[2*i+1]==-pcm[2*i]);
        if(phone_priming_left) {
            assert(pcm[2*i]==0);--phone_priming_left;++phone_priming_encoded;
        } else if(phone_active || call_active) {
            assert(pcm[2*i]==signal(encoded-phone_priming_encoded+playback_metrics.pcm_drops));
        } else if(check_sequence) {
            int expected=encoded<PLAYBACK_FILTER_DELAY?0:signal(encoded-PLAYBACK_FILTER_DELAY);
            assert(pcm[2*i]==expected);
        } else if(have_previous && encoded>64 && !encoder_job.tempo) {
            int delta=pcm[2*i]-(int)previous;
            if(delta< -2 || delta>2)fprintf(stderr,"discontinuity ms=%u encoded=%u sample=%u delta=%d phase=%u interval=%u used=%u debt=%u drops=%u zeros=%u recoveries=%u\n",
                now,encoded,i,delta,playback_pcm.fraction,playback_adjust_interval(&playback_adjust),playback_pcm.used,
                media_tracker.samples_ready,playback_metrics.pcm_drops,playback_metrics.pcm_underflows,playback_trace.stats[TRACE_RECOVERIES]);
            assert(delta>= -2 && delta<=2);
        }
        previous=(uint32_t)(int32_t)pcm[2*i];have_previous=true;++encoded;
    }
    advance(variable_cost?5u+now%4u:encode_cost);
#endif
}
static void pipeline_filter_cost(void) { advance(filter_cost); }
static void fixture(int ppm) {
    waveform_fixture=waveform_sub_lsb=false;
    waveform_tempo_cost=waveform_tempo_jobs=0;
    waveform_tempo_hundredths=waveform_encoder_hundredths=0;
    waveform_tempo_remainder=waveform_encoder_remainder=0;
    waveform_previous_tempo_ms=waveform_previous_tempo_interval=0;
    waveform_previous_tempo_generation=0;
    initialize(4,480,324,10,986);
    is_usb_streaming=true;phone_active=phone_buffer_started=false;
    playback_game.changed=true;playback_game.target=60;
    memset(&playback_pcm,0,sizeof(playback_pcm));
    memset(&playback_trace,0,sizeof(playback_trace));
    assert(playback_pcm_reset(&playback_pcm,7680,false));
    playback_pcm.primed_on_wire=true;
    playback_usb_clock_reset(&usb_clock);
    usb_recovery_drops=generated=encoded=previous=0;
    ingress_fraction=0;drift_ppm=ppm;encode_cost=5;blocked_until=0;
    filter_cost=loop_cost=0;
    timer_phase=0;
    unlocked_timer_callbacks=encode_timer_callbacks=0;
    request_wait=next_wait=0;
    check_sequence=ppm==0;have_previous=false;
    usb_jitter=variable_cost=false;delayed_frames=0;
    sink_frames=sink_underruns=0;sink_started=false;
    phone_priming_left=phone_priming_encoded=0;
    for(unsigned i=0;i<20;++i)ingress(48);
    media_tracker.audio_timer.context=&media_tracker;
    media_tracker.audio_timer.timeout=10;
}
static void step(void) {
    uint32_t started=now*1000u;
    dongle_playback_encode_complete();
    unsigned encoded_before=encode_calls;
    if(phone_active && phone_output_ready) {
        phone_ingress(720);phone_output_ready=false;
    }
    if((int32_t)(now-media_tracker.audio_timer.timeout-timer_phase)>=0)
        avdtp_audio_timeout_handler(&media_tracker.audio_timer);
    if(send_requested && (int32_t)(now-blocked_until)>=0) {
        send_requested=false;
        a2dp_demo_send_media_packet();
    }
    playback_audio_work(now);
    bt_locked=false;dongle_playback_encode_run();bt_locked=true;
    assert(encode_calls-encoded_before<=1); // Can-send callbacks cannot recursively encode.
    advance(loop_cost);
    advance(realtime_loop_idle_us(started,now*1000u,false)/1000u);
}
static void pipeline_audio_work(uint32_t time) {
    dongle_playback_encode_complete();
    playback_audio_work(time);
    complete_test_encode();
}
#define playback_audio_work pipeline_audio_work
static void startup_keeps_one_filter_phase(void) {
    for(unsigned floating=0;floating<2;++floating) {
        fixture(0);
        playback_pcm_clear(&playback_pcm,7680);
        playback_pcm.priming=PLAYBACK_PRIMING_FRAMES;
        playback_pcm.primed_on_wire=false;
        for(unsigned packet=0;packet<30;++packet) {
            int16_t integer[96];float real[96];
            for(unsigned i=0;i<48;++i) {
                int16_t sample=(int16_t)((packet*48u+i+1u)*8u);
                integer[2*i]=sample;integer[2*i+1]=(int16_t)-sample;
                real[2*i]=((float)sample+0.25f)/32768.0f;real[2*i+1]=-real[2*i];
            }
            if(floating)audio_slot_push_float(real,48);
            else audio_slot_push_samples(integer,48);
        }
        playback_usb_clock_t before=usb_clock;
        for(unsigned block=0;block<10;++block) {
            audio_pcm_read(480);
            for(unsigned i=0;i<960;++i)assert(encoder_sample_s16(i)==0);
        }
        assert(!playback_pcm.priming && playback_pcm.used==1440);
        // Normal 150 ms mode sends three AUs together: priming finishes inside
        // a packet, and two real AUs precede its on-wire priming receipt.
        for(unsigned block=0;block<3;++block) {
            audio_pcm_read(480);
            for(unsigned i=0;i<480;++i) {
                unsigned position=block*480u+i;
                int16_t expected=position<PLAYBACK_FILTER_DELAY?0:
                    (int16_t)((position-PLAYBACK_FILTER_DELAY+1u)*8u);
                assert(encoder_sample_s16(2*i)==expected && encoder_sample_s16(2*i+1)==-expected);
            }
            if(block<2)assert(memcmp(&before,&usb_clock,sizeof(before))==0);
            if(block==1)playback_pcm_sent(&playback_pcm);
        }
        assert(!playback_metrics.pcm_underflows);
    }
}
static void fractional_shortage_keeps_filter(void) {
    static playback_pcm_t reference;
    static playback_resample_t work;
    for(unsigned floating=0;floating<2;++floating) {
        fixture(0);
        playback_pcm_clear(&playback_pcm,7680);
        playback_pcm.primed_on_wire=true;
        // This phase is reached after twelve 480-frame +976 ppm reads.
        playback_pcm.fraction=409600;
        usb_clock.rate_q16=65600;
        for(unsigned i=0;i<PLAYBACK_FILTER_TAPS;++i) {
            float value=(float)(1000+8*((int)i-(int)PLAYBACK_FILTER_TAPS))/32768.0f;
            playback_pcm.history[2*i]=value;playback_pcm.history[2*i+1]=-value;
        }
        int16_t integer[1000],expected[960],last;
        float real[1000];
        for(unsigned i=0;i<480;++i) {
            integer[2*i]=(int16_t)(1000+8*i);integer[2*i+1]=-integer[2*i];
            real[2*i]=((float)integer[2*i]+0.375f)/32768.0f;real[2*i+1]=-real[2*i];
        }
        if(floating)audio_slot_push_float(real,480);
        else audio_slot_push_samples(integer,480);
        reference=playback_pcm;
        // Independent shortage oracle: insert one explicitly counted zero input
        // frame, then use the existing strict (enough-input) FIR path.
        const int16_t zero[2]={0};
        playback_pcm_push(&reference,zero,1);
        assert(playback_pcm_prepare_resample(&reference,&work,480,65600u*10u));
        playback_pcm_render(&work,expected);
        audio_pcm_read(480);
        assert(encoder_matches_s16(expected,960));
        assert(encoder_sample_s16(0)>=876 && encoder_sample_s16(0)<=878); // Raw fallback began at 1000.
        assert(playback_pcm.used==0 && playback_pcm.fraction==reference.fraction);
        assert(memcmp(playback_pcm.history,reference.history,sizeof(reference.history))==0);
        assert(playback_metrics.pcm_underflows==1 && playback_trace.stats[TRACE_ACTIVE_ZERO]==1);
        last=encoder_sample_s16(958);
        for(unsigned i=0;i<500;++i) {
            integer[2*i]=(int16_t)(1000+8*(480+i));integer[2*i+1]=-integer[2*i];
            real[2*i]=((float)integer[2*i]+0.375f)/32768.0f;real[2*i+1]=-real[2*i];
        }
        if(floating) {
            audio_slot_push_float(real,500);playback_pcm_push_float(&reference,real,500);
        } else {
            audio_slot_push_samples(integer,500);playback_pcm_push(&reference,integer,500);
        }
        assert(playback_pcm_prepare_resample(&reference,&work,480,usb_clock.rate_q16*10u));
        playback_pcm_render(&work,expected);
        audio_pcm_read(480);
        assert(encoder_matches_s16(expected,960));
        assert(encoder_sample_s16(0)-last>=4 && encoder_sample_s16(0)-last<=12); // Former raw→FIR jump was -115.
        assert(playback_metrics.pcm_underflows==1 && playback_trace.stats[TRACE_ACTIVE_ZERO]==1);
    }
}
static void measured_cpu_budget(void) {
    fixture(0);
    // Hardware reached 7.9 ms encode and 9.6 ms including PCM/other work.
    encode_cost=8;filter_cost=1;loop_cost=1;
    while(now<30000)step();
    assert(!playback_metrics.pcm_drops && !playback_metrics.pcm_underflows);
    assert(!playback_trace.stats[TRACE_RECOVERIES] && !sink_underruns);
    assert(encoded>=1439000u);
}
static void steady_and_stalls(void) {
    for(unsigned cost=5;cost<=8;cost+=3) {
        fixture(0);encode_cost=cost;
        for(unsigned stall=1;stall<=40;++stall) {
            unsigned end=now+200;
            while(now<end)step();
            blocked_until=now+stall;
            end=now+300;
            while(now<end)step();
            assert(!playback_metrics.pcm_underflows && !playback_metrics.pcm_drops);
            assert(!usb_recovery_drops && playback_pcm.used<=PLAYBACK_USB_ACTIVE_LIMIT);
            assert(!sink_underruns);
        }
    }
}
static void clock_drift(void) {
    const int rates[]={0,100,-100,500,-500};
    for(unsigned i=0;i<5;++i) {
        fixture(rates[i]);
        while(now<600000)step();
        assert(!playback_metrics.pcm_underflows && !playback_metrics.pcm_drops);
        assert(playback_pcm.used<PLAYBACK_USB_ACTIVE_LIMIT && playback_pcm.used>240);
        assert(!sink_underruns);
        assert(encoded>28000000u);
    }
}
static void delayed_timer_clock(void) {
    const int rates[]={500,-500};
    for(unsigned i=0;i<2;++i) {
        fixture(rates[i]);timer_phase=3;
        while(now<600000)step();
        assert(!playback_metrics.pcm_drops && !playback_metrics.pcm_underflows);
        assert(!sink_underruns && !playback_trace.stats[TRACE_RECOVERIES]);
    }
}
static void send_wait_after_encoding(void) {
    fixture(0);encode_cost=8;filter_cost=1;timer_phase=3;
    while(now<1000)step();
    request_wait=40;
    while(now<10000)step();
    assert(!playback_metrics.pcm_drops && !playback_metrics.pcm_underflows);
    assert(!sink_underruns && !playback_trace.stats[TRACE_RECOVERIES]);
    assert(playback_trace.stats[TRACE_SEND_WAIT_US]<=41000u);
    assert(encode_timer_callbacks>0); // The production callback ran during unlocked FDK work.
}
static void timer_interrupts_respect_bluetooth_lock(void) {
    fixture(0);bt_locked=true;advance(11);
    assert(!unlocked_timer_callbacks && media_tracker.audio_timer.timeout==10);
    bt_locked=false;advance(1);bt_locked=true;
    assert(unlocked_timer_callbacks==1 && media_tracker.audio_timer.timeout==20);
}
static void excessive_stall(void) {
    fixture(0);
    while(now<200)step();
    advance(100); // USB IRQ continues while Bluetooth execution is stalled.
    assert(usb_recovery_drops);
    allow_recovery=true;
    avdtp_audio_timeout_handler(&media_tracker.audio_timer);
    playback_audio_work(now);
    assert(!usb_recovery_drops && !media_tracker.samples_ready);
    assert(playback_pcm.used<=1440);
    assert(playback_trace.stats[TRACE_RECOVERIES]==1);
    assert(suspend_recovery_pending && recovery_reset_encoder && !media_tracker.streaming);
    unsigned encoded_before=encoded;
    unsigned end=now+1000;
    while(now<end)step();
    assert(playback_trace.stats[TRACE_RECOVERIES]==1 && !playback_metrics.pcm_underflows);
    assert(encoded==encoded_before); // Stale packets cannot resume before confirmed restart.
}
static void stale_receiver_window(void) {
    fixture(0);
    while(!emitted)step();
    blocked_until=now+1000; // Hold the callback; completion now submits immediately.
    unsigned deadline=now+100;
    while(!send_requested && now<deadline)step();
    assert(send_requested);
    now+=80; // Isolate a successful submission after the 60 ms receiver window.
    send_requested=false;a2dp_demo_send_media_packet();
    assert(playback_transport_stale);
    allow_recovery=true;playback_audio_work(now);
    assert(suspend_recovery_pending && !media_tracker.streaming && playback_metrics.recoveries==1);
}
static void jitter_and_live_changes(void) {
    fixture(100);usb_jitter=variable_cost=true;
    unsigned next=300;
    while(now<10000) {
        if(now>=next) {blocked_until=now+32;next=now+300;}
        step();
    }
    assert(!playback_metrics.pcm_drops && !playback_metrics.pcm_underflows);
    const uint16_t reports[]={1500,600,1500};
    const uint16_t targets[]={60,150,60};
    for(unsigned i=0;i<3;++i) {
        playback_adjust_request(&playback_adjust,reports[i],targets[i]);
        playback_game.target=targets[i];
        unsigned end=now+3000;
        while(now<end)step();
        if(playback_adjust_busy(&playback_adjust)||playback_adjust.failed)
            fprintf(stderr,"game stuck index=%u ms=%u pending=%u running=%u failed=%u started=%u interval=%u sent=%u goal=%u debt=%u queued=%u used=%u\n",
                i,now,playback_adjust.pending,playback_adjust.running,playback_adjust.failed,playback_adjust.started,
                playback_adjust.interval,playback_adjust.sent_frames,playback_adjust.goal_frames,
                media_tracker.samples_ready,media_tracker.codec_storage_count,playback_pcm.used);
        assert(!playback_adjust_busy(&playback_adjust) && !playback_adjust.failed);
        assert(!playback_metrics.pcm_underflows && !playback_metrics.pcm_drops);
    }
}
static void phone_send_wait_keeps_source_time(void) {
    fixture(0);
    playback_game.target=150; // Low-level overflow accounting; phone_catchup tests the live recovery poll.
    is_usb_streaming=false;phone_active=phone_opened=phone_buffer_started=true;
    assert(playback_pcm_reset(&playback_pcm,8192,false));playback_pcm.primed_on_wire=true;
    generated=encoded=0;phone_arrivals=phone_queued=phone_decode_due=phone_decoded=0;
    phone_decoding=phone_output_ready=false;
    phone_ingress(4800); // The 90 ms phone reserve plus one encoder unit.
    request_wait=100;next_wait=500;
    while(now<10000)step();
    assert(playback_metrics.pcm_drops && !playback_metrics.pcm_underflows);
    assert(playback_metrics.pcm_drops>=playback_trace.stats[TRACE_DEBT_DROP]);
    assert(playback_metrics.pcm_drops==playback_trace.stats[TRACE_ACTIVE_DROP]);
    assert(generated==encoded+playback_pcm.used+playback_metrics.pcm_drops);
    assert(playback_trace.stats[TRACE_RECOVERIES] && !playback_metrics.recoveries);
    assert(!suspend_recovery_pending && media_tracker.streaming); // This fixture omits the recovery poll.
    request_wait=0;blocked_until=0;
    unsigned end=now+1000;
    while(now<end)step();
    assert(playback_pcm.used<5760 && media_tracker.samples_ready<480);
}
static void phone_late_decode_keeps_unmatched_debt(void) {
    fixture(0);
    is_usb_streaming=false;phone_active=phone_opened=phone_buffer_started=true;
    assert(playback_pcm_reset(&playback_pcm,8192,false));generated=encoded=0;
    media_tracker.samples_ready=4800;
    media_tracker.codec_ready_to_send=1;playback_work_pending=false;
    playback_pcm.fraction=12345; // A timeline trim must not restart resampling phase.
    phone_ingress(720);
    playback_audio_work(now);
    assert(playback_pcm.used==720 && media_tracker.samples_ready==4800);
    assert(!playback_metrics.pcm_drops && !playback_trace.stats[TRACE_DEBT_DROP]);
    playback_audio_work(now); // A blocked send cannot erase retained PCM or future decode debt.
    assert(media_tracker.samples_ready==4800 && playback_pcm.used==720);
    phone_ingress(1440);playback_audio_work(now);
    assert(playback_pcm.used==2160 && media_tracker.samples_ready==4800);
    assert(!playback_metrics.pcm_drops && !playback_trace.stats[TRACE_DEBT_DROP]);
    assert(playback_pcm.fraction==12345 && !playback_trace.stats[TRACE_RECOVERIES]);
    assert(generated==playback_pcm.used+playback_metrics.pcm_drops);
    assert(!suspend_recovery_pending && media_tracker.streaming);
}
static void phone_overflow_reconciles_source_time(void) {
    fixture(0);is_usb_streaming=false;phone_active=phone_opened=phone_buffer_started=true;
    assert(playback_pcm_reset(&playback_pcm,8192,false));playback_pcm.primed_on_wire=true;
    generated=encoded=0;phone_ingress(8000);
    media_tracker.samples_ready=3500;
    media_tracker.codec_ready_to_send=1;playback_work_pending=false;
    phone_ingress(512);
    assert(playback_metrics.pcm_drops==320 && playback_pcm.dropped_pending==320);
    assert(playback_trace.stats[TRACE_ACTIVE_DROP]==320);
    playback_audio_work(now);
    assert(media_tracker.samples_ready==3180 && !playback_pcm.dropped_pending);
    assert(playback_metrics.pcm_drops==320 && playback_pcm.used==8192);
    assert(playback_trace.stats[TRACE_DEBT_DROP]==320 && playback_trace.stats[TRACE_ACTIVE_DROP]==320);
    playback_audio_work(now); // A physical loss retires its matching deadline exactly once.
    assert(playback_metrics.pcm_drops==320 && media_tracker.samples_ready==3180);
    audio_pcm_read(480);
    for(unsigned i=0;i<480;++i) {
        assert(encoder_sample_s16(2*i)==signal(320+i));
        assert(encoder_sample_s16(2*i+1)==-signal(320+i));
    }

    playback_pcm_clear(&playback_pcm,8192);generated=0;
    phone_ingress(8192);media_tracker.samples_ready=200;
    phone_ingress(720);
    playback_audio_work(now);
    assert(media_tracker.samples_ready==200 && !playback_pcm.dropped_pending);
    media_tracker.samples_ready=480;playback_audio_work(now);
    assert(media_tracker.samples_ready==480 && !playback_pcm.dropped_pending);
    assert(playback_pcm.used==8192); // Lost source samples cannot cancel current/future output deadlines.

    playback_pcm.dropped_pending=120;playback_pcm.priming=480;
    media_tracker.samples_ready=480;playback_audio_work(now);
    assert(!playback_pcm.dropped_pending && media_tracker.samples_ready==480);
    playback_pcm.priming=0;playback_pcm.primed_on_wire=false;
    playback_pcm.dropped_pending=120;playback_audio_work(now);
    assert(!playback_pcm.dropped_pending && media_tracker.samples_ready==480);
    playback_pcm.primed_on_wire=true;phone_buffer_started=false;
    playback_pcm.dropped_pending=120;playback_audio_work(now);
    assert(!playback_pcm.dropped_pending && media_tracker.samples_ready==480);
    playback_pcm_clear(&playback_pcm,7680); // The normal source-switch reset drops old credit.
    assert(!playback_pcm.dropped_pending);
    phone_active=phone_opened=false;is_usb_streaming=true;
    playback_audio_work(now);
    assert(media_tracker.samples_ready==480 && !playback_pcm.dropped_pending);
}
static void phone_overflow_does_not_starve_output(void) {
    // Actual timer, PCM, sample reads and packet preparation. One early packet
    // must not make a full FIFO cancel every future read at a healthy 48 kHz.
    const unsigned packet_frames[]={720,960};
    for(unsigned kind=0;kind<2;++kind) {
        unsigned frames=packet_frames[kind],period=frames/48;
        for(unsigned phase=0;phase<period;++phase) {
            fixture(0);is_usb_streaming=false;phone_active=phone_opened=phone_buffer_started=true;
            playback_game.target=150;encode_cost=0;generated=encoded=0;
            assert(playback_pcm_reset(&playback_pcm,8192,false));playback_pcm.primed_on_wire=true;
            phone_ingress(8096);phone_ingress(frames);
            assert(playback_metrics.pcm_drops==frames-96);
            playback_audio_work(now);
            for(now=1;now<=1000;++now) {
                if((now+phase)%period==0)phone_ingress(frames);
                if((int32_t)(now-media_tracker.audio_timer.timeout)>=0)
                    avdtp_audio_timeout_handler(&media_tracker.audio_timer);
                if(send_requested) {send_requested=false;a2dp_demo_send_media_packet();}
                playback_audio_work(now);
            }
            assert(encoded==48000 && !playback_metrics.pcm_underflows);
            assert(!playback_trace.stats[TRACE_DEBT_DROP] && !playback_pcm.dropped_pending);
            assert(generated==encoded+playback_pcm.used+playback_metrics.pcm_drops);
            assert(playback_pcm.used<8192 && playback_metrics.pcm_drops<3*frames);
        }
    }
}
static void phone_stale_receiver_window(void) {
    fixture(0);
    is_usb_streaming=false;phone_active=phone_opened=phone_buffer_started=true;
    assert(playback_pcm_reset(&playback_pcm,8192,false));playback_pcm.primed_on_wire=true;
    generated=encoded=0;phone_arrivals=phone_queued=phone_decode_due=phone_decoded=0;
    phone_decoding=phone_output_ready=false;phone_ingress(4320);
    while(!emitted)step();
    blocked_until=now+1000;
    unsigned deadline=now+100;
    while(!send_requested && now<deadline)step();
    assert(send_requested);
    advance(80); // The real 60 ms phone receiver window is exceeded.
    send_requested=false;a2dp_demo_send_media_packet();
    assert(playback_transport_stale && !suspend_recovery_pending);
}
static void phone_reprime_then_fresh_pcm(void) {
    fixture(0);
    is_usb_streaming=false;phone_active=phone_opened=true;phone_buffer_started=false;
    assert(playback_pcm_reset(&playback_pcm,8192,true));generated=encoded=0;
    for(unsigned unit=0;unit<10;++unit) {
        audio_pcm_read(480);
        for(unsigned i=0;i<960;++i)assert(encoder_sample_s16(i)==0);
        playback_media_sent();
    }
    assert(!playback_pcm.priming && playback_pcm.primed_on_wire && !phone_buffer_started);
    phone_ingress(4320);
    audio_pcm_read(480);
    assert(phone_buffer_started && playback_pcm.used==3840);
    for(unsigned i=0;i<480;++i) {
        assert(encoder_sample_s16(2*i)==signal(i));
        assert(encoder_sample_s16(2*i+1)==-signal(i));
    }
    assert(!playback_metrics.pcm_underflows && !playback_metrics.pcm_drops);
}
static void phone_packet_priming(void) {
    for(unsigned recovery=0;recovery<2;++recovery) {
        fixture(0);is_usb_streaming=false;phone_opened=true;phone_active=recovery!=0;
        phone_buffer_started=false;encode_cost=0;generated=encoded=0;
        assert(playback_pcm_reset(&playback_pcm,8192,true));phone_priming_left=4800;
        for(unsigned unit=0;unit<10;++unit) {
            now+=10;avdtp_audio_timeout_handler(&media_tracker.audio_timer);
            unsigned before=encode_calls;playback_audio_work(now);
            assert(encode_calls==before+1 && !playback_pcm.primed_on_wire);
            if(send_requested) {send_requested=false;a2dp_demo_send_media_packet();}
            assert(playback_pcm.primed_on_wire==(unit==9));
        }
        assert(!phone_priming_left && phone_priming_encoded==4800);
        assert(eld_packets==5 && eld_units==10 && emitted==4800 && !playback_pcm.used);
        phone_active=true;phone_ingress(4320);
        for(unsigned unit=0;unit<2;++unit) {
            now+=10;avdtp_audio_timeout_handler(&media_tracker.audio_timer);
            unsigned before=encode_calls;playback_audio_work(now);
            assert(encode_calls==before+1);
            if(send_requested) {send_requested=false;a2dp_demo_send_media_packet();}
        }
        assert(phone_buffer_started && playback_pcm.used==3360 && encoded==5760);
        assert(eld_packets==6 && eld_units==12 && emitted==5760);
        assert(!playback_metrics.pcm_underflows && !playback_metrics.pcm_drops);
    }
}
static void call_prefill_uses_call_clock(void) {
    fixture(0);is_usb_streaming=false;phone_active=false;call_active=call_opened=true;
    phone_buffer_started=false;generated=encoded=0;encode_cost=0;
    assert(playback_pcm_reset(&playback_pcm,8192,false));playback_pcm.primed_on_wire=true;
    usb_clock.rate_q16=65000;
    phone_ingress(1440);
    now+=10;avdtp_audio_timeout_handler(&media_tracker.audio_timer);playback_audio_work(now);
    assert(phone_buffer_started && playback_pcm.used==960 && encoded==480);
    assert(usb_clock.rate_q16==65000 && !playback_metrics.pcm_underflows);
    call_active=call_opened=false;
}
static void eld_low_bits_reach_encoder(void) {
    fixture(0);encode_cost=0;
    playback_pcm_clear(&playback_pcm,7680);
    float input[1024*2];
    for(unsigned i=0;i<1024;++i) {
        input[2*i]=0.125f+(i%8+1)*0x1p-20f;input[2*i+1]=-input[2*i];
    }
    audio_slot_push_float(input,1024);
    media_tracker.samples_ready=480;
    precise_input_fixture=true;
    assert(fill_aac_audio_buffer(&media_tracker)==480);
    complete_test_encode();
    precise_input_fixture=false;
    assert(encoded==480 && encode_calls==1);
}

static void waveform_start(double left,double right,bool antiphase,bool floating) {
    fixture(0);
    waveform_fixture=true;waveform_float=floating;waveform_antiphase=antiphase;
    waveform_hz[0]=left;waveform_hz[1]=right;waveform_frames=0;
    playback_pcm_clear(&playback_pcm,7680);playback_pcm.primed_on_wire=true;
    generated=0;
    for(unsigned i=0;i<20;++i)ingress(48);
    while(now<600)step();
}

static double waveform_power(unsigned first,unsigned count,unsigned channel,double hz) {
    double real=0,imaginary=0;
    for(unsigned i=0;i<count;++i) {
        double angle=6.2831853071795864769*i/count;
        double sample=waveform_output[first+i][channel]*(0.5-0.5*cos(angle));
        double phase=6.2831853071795864769*hz*i/48000.0;
        real+=sample*cos(phase);imaginary+=sample*sin(phase);
    }
    return real*real+imaginary*imaginary;
}

static void waveform_check_pitch(unsigned first,unsigned end) {
    assert(end>first+12000);
    first+=960;end-=960;
    for(unsigned channel=0;channel<2;++channel) {
        double hz=waveform_hz[channel];
        double desired=waveform_power(first,end-first,channel,hz);
        double lower=waveform_power(first,end-first,channel,hz*0.9);
        double upper=waveform_power(first,end-first,channel,hz*1.1);
        if(desired<=9*lower || desired<=9*upper)
            fprintf(stderr,"pitch channel=%u hz=%.1f frames=%u wanted=%.3f lower=%.3f upper=%.3f\n",
                channel,hz,end-first,desired,lower,upper);
        assert(desired>9*lower && desired>9*upper);
        double squares=0,first_cross=0,last_cross=0;
        unsigned crossings=0;
        for(unsigned i=first;i<end;++i) {
            double sample=waveform_output[i][channel];squares+=sample*sample;
            if(i>first && waveform_output[i-1][channel]<=0 && sample>0) {
                double previous_sample=waveform_output[i-1][channel];
                double crossing=i-1-previous_sample/(sample-previous_sample);
                if(!crossings)first_cross=crossing;
                last_cross=crossing;++crossings;
            }
        }
        assert(crossings>=4);
        double measured=(crossings-1)*48000.0/(last_cross-first_cross);
        if(fabs(measured/hz-1)>0.01) {
            double peak_hz=0,peak_power=0;
            for(double test=hz*0.88;test<hz*1.12;test+=hz*0.001) {
                double power=waveform_power(first,end-first,channel,test);
                if(power>peak_power){peak_power=power;peak_hz=test;}
            }
            if(fabs(peak_hz/hz-1)>0.01)
                fprintf(stderr,"pitch channel=%u wanted=%.1f crossing=%.4f peak=%.4f first=%u end=%u\n",channel,hz,measured,peak_hz,first,end);
            measured=peak_hz;
        }
        assert(fabs(measured/hz-1)<=0.01);
        assert(squares/(end-first)>0.025 && squares/(end-first)<0.065);
    }
}

static void waveform_check_joins(unsigned first,unsigned end) {
    for(unsigned channel=0;channel<2;++channel) {
        double limit=0.6*sin(3.14159265358979323846*waveform_hz[channel]/48000.0)+0.012;
        for(unsigned i=first+1;i<end;++i) {
            assert(isfinite(waveform_output[i][channel]));
            assert(fabs(waveform_output[i][channel])<=0.301);
            if(fabs(waveform_output[i][channel]-waveform_output[i-1][channel])>limit)
                fprintf(stderr,"join channel=%u hz=%.1f frame=%u jump=%.6f limit=%.6f\n",channel,
                    waveform_hz[channel],i,(double)waveform_output[i][channel]-waveform_output[i-1][channel],limit);
            assert(fabs(waveform_output[i][channel]-waveform_output[i-1][channel])<=limit);
        }
    }
    if(waveform_antiphase)for(unsigned i=first;i<end;++i)
        assert(fabs(waveform_output[i][0]+waveform_output[i][1])<=0x1p-23);
    assert(!playback_metrics.pcm_drops && !playback_metrics.pcm_underflows);
    assert(!playback_trace.stats[TRACE_RECOVERIES] && !sink_underruns);
}

static void game_transition_waveforms(void) {
    const double tones[][2]={
        {20,200},{40,997},{80,120},{997,1403},{100,100},
        {20,997},{30,401},{40,1403},{55,809},{80,997},{97,1103},
        {150,997},{220,1403},{997,2003},{3001,5003},{12001,14003},{16001,19001}
    };
    for(unsigned kind=0;kind<sizeof(tones)/sizeof(tones[0]);++kind) {
        waveform_start(tones[kind][0],tones[kind][1],kind==4,kind%2==0);
        for(unsigned restore=0;restore<2;++restore) {
            unsigned first=waveform_frames;
            sink_frames=(restore?60u:150u)*48u;sink_started=true;
            playback_adjust_request(&playback_adjust,restore?600:1500,restore?150:60);
            playback_game.target=restore?150:60;
            unsigned deadline=now+2500;
            do step();while(playback_adjust_busy(&playback_adjust) && now<deadline);
            assert(!playback_adjust_busy(&playback_adjust) && !playback_adjust.failed);
            unsigned end=waveform_frames;
            waveform_check_pitch(first,end);
            deadline=now+100;
            while(now<deadline)step();
            waveform_check_joins(first,waveform_frames);
        }
    }
    waveform_fixture=false;
}

static void game_transition_cancel_and_reverse(void) {
    waveform_start(40,997,false,true);
    sink_frames=150*48;sink_started=true;
    unsigned first=waveform_frames;
    for(unsigned change=0;change<8;++change) {
        uint8_t old=playback_adjust_interval(&playback_adjust);
        playback_adjust_cancel_stream(&media_tracker,now,old);
        playback_adjust_request(&playback_adjust,change%2?600:1500,change%2?150:60);
        playback_game.target=change%2?150:60;
        unsigned until=now+80;
        while(now<until)step();
    }
    playback_adjust_cancel_stream(&media_tracker,now,playback_adjust_interval(&playback_adjust));
    unsigned until=now+200;
    while(now<until)step();
    assert(!playback_adjust_busy(&playback_adjust) && !playback_adjust.failed);
    waveform_check_joins(first,waveform_frames);
    waveform_fixture=false;
}

static void game_transition_retains_float_precision(void) {
    waveform_start(100,100,true,true);waveform_sub_lsb=true;
    unsigned until=now+100;
    while(now<until)step();
    for(unsigned restore=0;restore<2;++restore) {
        unsigned first=waveform_frames;
        playback_adjust_request(&playback_adjust,restore?600:1500,restore?150:60);
        playback_game.target=restore?150:60;
        unsigned deadline=now+2500;
        do step();while(playback_adjust_busy(&playback_adjust) && now<deadline);
        assert(!playback_adjust_busy(&playback_adjust) && !playback_adjust.failed);
        for(unsigned i=first;i<waveform_frames;++i) {
            assert(fabs(waveform_output[i][0]-(0.125f+0x1p-20f))<=0x1p-23f);
            assert(waveform_output[i][0]==-waveform_output[i][1]);
        }
    }
    waveform_fixture=false;
}

static void game_transition_cpu_budget(void) {
    for(unsigned encoder_ms=5;encoder_ms<=6;++encoder_ms)for(unsigned tempo_ms=0;tempo_ms<=2;++tempo_ms) {
        waveform_start(100,100,true,true);
        encode_cost=encoder_ms;waveform_tempo_cost=tempo_ms;
        unsigned first=waveform_frames;
        for(unsigned restore=0;restore<2;++restore) {
            sink_frames=(restore?60u:150u)*48u;sink_started=true;
            playback_adjust_request(&playback_adjust,restore?600:1500,restore?150:60);
            playback_game.target=restore?150:60;
            unsigned deadline=now+2500;
            do step();while(playback_adjust_busy(&playback_adjust) && now<deadline);
            if(playback_adjust_busy(&playback_adjust) || playback_adjust.failed)
                fprintf(stderr,"tempo budget encoder_ms=%u tempo_ms=%u restore=%u failed=%u\n",
                    encoder_ms,tempo_ms,restore,playback_adjust.failed);
            assert(!playback_adjust_busy(&playback_adjust) && !playback_adjust.failed);
            deadline=now+100;
            while(now<deadline)step();
        }
        assert(waveform_tempo_jobs>100);
        waveform_check_joins(first,waveform_frames);
    }
    waveform_fixture=false;
}

static void invalidate_tempo_snapshot(void) {
    assert(!bt_locked && !pcm_locked);
    pcm_lock_hook=NULL;++stale_tempo_hooks;
    if(stale_tempo_kind==0)playback_pcm_discontinuity(&playback_pcm);
    else if(stale_tempo_kind==1)assert(playback_pcm_consume(&playback_pcm,48));
    else ++usb_playback_revision;
    stale_tempo_read=playback_pcm.read;stale_tempo_used=playback_pcm.used;
}

static void game_transition_stale_snapshot(void) {
    for(stale_tempo_kind=0;stale_tempo_kind<3;++stale_tempo_kind) {
        waveform_start(100,100,true,true);
        dongle_playback_encode_complete();
        if(send_requested) {send_requested=false;a2dp_demo_send_media_packet();}
        playback_adjust_request(&playback_adjust,1500,60);
        playback_adjust_start(&playback_adjust,now);
        media_tracker.samples_ready=480;
        assert(fill_aac_audio_buffer(&media_tracker)==480 && encoder_job.tempo);
        assert(atomic_load(&encoder_job.state)==ENCODE_PREPARED);
        playback_tempo_state_t previous_state=encoder_tempo_state;
        unsigned previous_calls=encode_calls,previous_packets=eld_packets;
        stale_tempo_hooks=0;pcm_lock_hook=invalidate_tempo_snapshot;
        bt_locked=false;dongle_playback_encode_run();bt_locked=true;
        assert(stale_tempo_hooks==1 && !pcm_lock_hook);
        assert(playback_pcm.read==stale_tempo_read && playback_pcm.used==stale_tempo_used);
        assert(encode_calls==previous_calls && eld_packets==previous_packets);
        assert(!encoder_tempo_job.rendered && !encoder_job.bytes && !encoder_job.consumed);
        dongle_playback_encode_complete();
        assert(atomic_load(&encoder_job.state)==ENCODE_IDLE);
        assert(!media_tracker.codec_storage_count && !media_tracker.codec_num_frames && !media_tracker.codec_ready_to_send);
        assert(encoder_tempo_state.active==previous_state.active &&
            encoder_tempo_state.offset==previous_state.offset && encoder_tempo_state.fraction==previous_state.fraction);
        assert(memcmp(encoder_tempo_state.history,previous_state.history,sizeof previous_state.history)==0);
    }
    waveform_fixture=false;
}

static void game_transition_measured_pressure(void) {
    for(unsigned encoder_fraction=0;encoder_fraction<=30;encoder_fraction+=30) {
        waveform_start(40,997,false,true);
        encode_cost=6;waveform_encoder_hundredths=encoder_fraction;
        waveform_tempo_cost=3;waveform_tempo_hundredths=37;
        playback_pcm.fraction=634880;
        for(unsigned restore=0;restore<2;++restore) {
            waveform_frames=0;
            sink_frames=(restore?60u:150u)*48u;sink_started=true;
            playback_adjust_request(&playback_adjust,restore?600:1500,restore?150:60);
            playback_game.target=restore?150:60;
            unsigned started=now,deadline=now+8000;
            bool stall_pending=encoder_fraction==30 && !restore;
            do {
                if(stall_pending && playback_adjust.running && now-started>=100) {
                    blocked_until=now+40;stall_pending=false;
                }
                step();
            } while(playback_adjust_busy(&playback_adjust) && now<deadline);
            fprintf(stderr,"measured tempo budget encoder=6.%02u tempo=3.37 restore=%u elapsed=%u failed=%u drops=%u missing=%u recoveries=%u sent=%u goal=%u used=%u C=%u ready=%u\n",
                encoder_fraction,restore,now-started,playback_adjust.failed,playback_metrics.pcm_drops,
                playback_metrics.pcm_underflows,playback_trace.stats[TRACE_RECOVERIES],playback_adjust.sent_frames,
                playback_adjust.goal_frames,playback_pcm.used,encoder_tempo_job.consumed,media_tracker.samples_ready);
            assert(!playback_adjust_busy(&playback_adjust) && !playback_adjust.failed);
            assert(!playback_metrics.pcm_drops && !playback_metrics.pcm_underflows);
            assert(!playback_trace.stats[TRACE_RECOVERIES]);
            waveform_check_pitch(0,waveform_frames);
            deadline=now+100;
            while(now<deadline)step();
            waveform_check_joins(0,waveform_frames);
            assert(playback_pcm.used<=1920 && media_tracker.samples_ready<=960);
        }
    }
    waveform_fixture=false;
}

static void game_transition_ingress_debt(void) {
    static const struct { int32_t arrived; uint16_t consumed; } cases[]={
        {449,448},{1057,528},{1,384},{-17,384},{450,450}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        waveform_start(100,100,true,true);
        playback_adjust_request(&playback_adjust,1500,60);playback_game.target=60;
        unsigned deadline=now+100;
        while(!encoder_tempo_state.active && now<deadline)step();
        assert(encoder_tempo_state.active);
        dongle_playback_encode_complete();
        if(send_requested) {send_requested=false;a2dp_demo_send_media_packet();}
        advance(11);
        assert(atomic_load(&encoder_job.state)==ENCODE_IDLE);
        uint32_t origin=usb_ingress_frames-(uint32_t)cases[i].arrived;
        encoder_tempo_ingress=origin;
        media_tracker.samples_ready=960;
        uint32_t read_before=playback_pcm.read,used_before=playback_pcm.used;
        assert(fill_aac_audio_buffer(&media_tracker)==480 && encoder_job.tempo);
        assert(encoder_tempo_job.consumed==cases[i].consumed);
        assert(encoder_tempo_ingress==origin && tempo_ingress_after==origin+cases[i].consumed);
        assert(playback_pcm.read==read_before && playback_pcm.used==used_before);
        bt_locked=false;dongle_playback_encode_run();bt_locked=true;
        dongle_playback_encode_complete();
        assert(encoder_tempo_ingress==origin+cases[i].consumed);
        assert((int32_t)(usb_ingress_frames-encoder_tempo_ingress)==
            cases[i].arrived-(int32_t)cases[i].consumed+48*(int32_t)encode_cost);
        assert(!playback_metrics.pcm_drops && !playback_metrics.pcm_underflows);
    }
    waveform_fixture=false;
}

static void game_transition_insufficient_throughput(void) {
    waveform_start(40,997,false,true);
    encode_cost=7;waveform_tempo_cost=3;
    playback_pcm.fraction=634880;
    sink_frames=60*48;sink_started=true;
    playback_adjust_request(&playback_adjust,600,150);playback_game.target=150;
    unsigned started=now,deadline=now+11000;
    do {waveform_frames=0;step();}while(playback_adjust_busy(&playback_adjust) && now<deadline);
    fprintf(stderr,"insufficient tempo throughput elapsed=%u failed=%u drops=%u missing=%u recoveries=%u\n",
        now-started,playback_game.failed,playback_metrics.pcm_drops,
        playback_metrics.pcm_underflows,playback_trace.stats[TRACE_RECOVERIES]);
    assert(!playback_adjust_busy(&playback_adjust) && playback_game.failed);
    assert(now-started>=10000 && now<deadline);
    assert(!playback_metrics.pcm_drops && !playback_metrics.pcm_underflows);
    assert(!playback_trace.stats[TRACE_RECOVERIES]);
    waveform_frames=0;deadline=now+200;
    while(now<deadline)step();
    waveform_check_joins(0,waveform_frames);
    waveform_fixture=false;
}

int main(void) {
    game_transition_waveforms();
    game_transition_cancel_and_reverse();
    game_transition_retains_float_precision();
    game_transition_cpu_budget();
    game_transition_stale_snapshot();
    game_transition_ingress_debt();
    game_transition_measured_pressure();
    game_transition_insufficient_throughput();
    eld_low_bits_reach_encoder();
    timer_interrupts_respect_bluetooth_lock();
    call_prefill_uses_call_clock();
    startup_keeps_one_filter_phase();
    fractional_shortage_keeps_filter();
    steady_and_stalls();clock_drift();delayed_timer_clock();send_wait_after_encoding();excessive_stall();stale_receiver_window();jitter_and_live_changes();measured_cpu_budget();
    phone_send_wait_keeps_source_time();
    phone_late_decode_keeps_unmatched_debt();
    phone_overflow_reconciles_source_time();
    phone_overflow_does_not_starve_output();
    phone_stale_receiver_window();
    phone_reprime_then_fresh_pcm();
    phone_packet_priming();
    puts("production USB pipeline: pitch, stereo, precision, cancellation, 1..40ms stalls, 10-minute clocks and bounded recovery passed");
    return 0;
}
