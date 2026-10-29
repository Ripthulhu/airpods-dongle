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
static void pipeline_encode(AACENC_BufDesc *input,AACENC_InArgs *args) {
    const int16_t *pcm=input->bufs[0];
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
        } else if(have_previous && encoded>64) {
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
    assert(encode_calls-encoded_before<=1); // Can-send callbacks cannot recursively encode.
    advance(loop_cost);
    advance(realtime_loop_idle_us(started,now*1000u,false)/1000u);
}
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
            for(unsigned i=0;i<960;++i)assert(encoder_pcm[i]==0);
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
                assert(encoder_pcm[2*i]==expected && encoder_pcm[2*i+1]==-expected);
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
        assert(memcmp(encoder_pcm,expected,sizeof(expected))==0);
        assert(encoder_pcm[0]>=876 && encoder_pcm[0]<=878); // Raw fallback began at 1000.
        assert(playback_pcm.used==0 && playback_pcm.fraction==reference.fraction);
        assert(memcmp(playback_pcm.history,reference.history,sizeof(reference.history))==0);
        assert(playback_metrics.pcm_underflows==1 && playback_trace.stats[TRACE_ACTIVE_ZERO]==1);
        last=encoder_pcm[958];
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
        assert(memcmp(encoder_pcm,expected,sizeof(expected))==0);
        assert(encoder_pcm[0]-last>=4 && encoder_pcm[0]-last<=12); // Former raw→FIR jump was -115.
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
    while(!send_requested)step();
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
    playback_game.target=150; // Isolate accounting within a legitimate receiver window.
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
    assert(!suspend_recovery_pending && media_tracker.streaming); // Phone readiness never changes.
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
    assert(!playback_pcm.used && media_tracker.samples_ready==4080);
    assert(playback_metrics.pcm_drops==720 && playback_trace.stats[TRACE_DEBT_DROP]==720);
    playback_audio_work(now); // No decoded PCM exists to match another skip yet.
    assert(media_tracker.samples_ready==4080 && playback_metrics.pcm_drops==720);
    phone_ingress(1440);playback_audio_work(now);
    assert(playback_pcm.used==240 && media_tracker.samples_ready==2880);
    assert(playback_metrics.pcm_drops==1920 && playback_trace.stats[TRACE_DEBT_DROP]==1920);
    assert(playback_pcm.fraction==12345 && playback_trace.stats[TRACE_RECOVERIES]==1);
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
    assert(media_tracker.samples_ready==2880 && !playback_pcm.dropped_pending);
    assert(playback_metrics.pcm_drops==620 && playback_pcm.used==7892);
    assert(playback_trace.stats[TRACE_DEBT_DROP]==620 && playback_trace.stats[TRACE_ACTIVE_DROP]==620);
    playback_audio_work(now); // The explicit debt clamp must not create another drop credit.
    assert(playback_metrics.pcm_drops==620 && media_tracker.samples_ready==2880);
    audio_pcm_read(480);
    for(unsigned i=0;i<480;++i) {
        assert(encoder_pcm[2*i]==signal(620+i));
        assert(encoder_pcm[2*i+1]==-signal(620+i));
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
    while(!send_requested)step();
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
        for(unsigned i=0;i<960;++i)assert(encoder_pcm[i]==0);
        playback_media_sent();
    }
    assert(!playback_pcm.priming && playback_pcm.primed_on_wire && !phone_buffer_started);
    phone_ingress(4320);
    audio_pcm_read(480);
    assert(phone_buffer_started && playback_pcm.used==3840);
    for(unsigned i=0;i<480;++i) {
        assert(encoder_pcm[2*i]==signal(i));
        assert(encoder_pcm[2*i+1]==-signal(i));
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

int main(void) {
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
    puts("production USB pipeline: real PCM, measured CPU budget, 1..40ms stalls, 10-minute clocks and bounded recovery passed");
    return 0;
}
