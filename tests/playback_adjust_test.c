// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/playback_adjust.h"
#include "../src/btstack/playback_pcm.h"
static playback_pcm_t pcm;

static void padded_window_boundaries(void) {
    playback_resample_t work;
    int16_t input[864],output[960];
    for(unsigned i=0;i<432;++i) {
        input[2*i]=(int16_t)(4*(i+1));input[2*i+1]=-input[2*i];
    }
    assert(playback_pcm_reset(&pcm,600,false));
    pcm.read=590;pcm.fraction=634880; // A USB-reachable phase, then a 0.9 ramp.
    playback_pcm_push(&pcm,input,432);
    assert(!playback_pcm_prepare_resample(&pcm,&work,480,9u*65536u));
    assert(pcm.used==432 && pcm.read==590 && pcm.fraction==634880);
    uint32_t missing=99;
    assert(playback_pcm_prepare_resample_fill(&pcm,&work,480,9u*65536u,&missing));
    assert(missing==1 && pcm.used==0 && pcm.read==422 && pcm.fraction==634880);
    assert(work.samples[2*(PLAYBACK_FILTER_TAPS+432)]==0); // Unconsumed lookahead.
    for(unsigned i=0;i<PLAYBACK_FILTER_TAPS;++i) {
        float expected=(float)input[2*(432-PLAYBACK_FILTER_TAPS+i)]/32768.0f;
        assert(pcm.history[2*i]==expected && pcm.history[2*i+1]==-expected);
    }
    playback_pcm_render(&work,output);
    for(unsigned i=0;i<480;++i)assert(output[2*i+1]==-output[2*i]);

    // Empty input still advances only fractional time, retaining the FIR delay.
    assert(playback_pcm_reset(&pcm,600,false));
    pcm.read=590;pcm.fraction=634880;
    assert(playback_pcm_prepare_resample_fill(&pcm,&work,480,655360u,&missing));
    assert(missing==480 && pcm.used==0 && pcm.read==590 && pcm.fraction==634880);
    playback_pcm_render(&work,output);
    for(unsigned i=0;i<960;++i)assert(output[i]==0);
    for(unsigned i=0;i<2*PLAYBACK_FILTER_TAPS;++i)assert(pcm.history[i]==0);

    assert(playback_pcm_reset(&pcm,7680,true));
    assert(!playback_pcm_prepare_resample_fill(&pcm,&work,480,655360u,&missing));
    assert(pcm.priming==PLAYBACK_PRIMING_FRAMES); // Never bypass encoded startup priming.
}

static void interpolation(void) {
    int16_t input[1400], output[1400];
    for (unsigned i=0;i<700;++i) { input[i*2]=(int16_t)(i*10); input[i*2+1]=(int16_t)-(int)(i*10); }
    for (unsigned interval=9;interval<=11;interval+=2) {
        assert(playback_pcm_reset(&pcm,768,false));
        // Exercise the wrap between both interpolation samples.
        playback_pcm_push(&pcm,input,700);
        assert(playback_pcm_read(&pcm,output,600)==600);
        playback_pcm_clear(&pcm,768); pcm.read=600;
        playback_pcm_push(&pcm,input,700);
        assert(playback_pcm_read_adjusted(&pcm,output,480,(uint8_t)interval));
        assert(pcm.used==700-48*interval);
        for (unsigned i=64;i<480;++i) {
            int expected=(int)(i*interval)-10*(int)PLAYBACK_FILTER_DELAY;
            assert(output[i*2]>=expected-1 && output[i*2]<=expected+1);
            assert(output[i*2+1]==-output[i*2]);
        }
        assert(!playback_pcm_read_adjusted(&pcm,output,480,(uint8_t)interval));
        assert(pcm.used==700-48*interval); // A failed read is non-destructive.
    }
}
static void live_transition(uint16_t report, uint16_t target) {
    playback_adjust_t s={0};
    int16_t input[96], output[960];
    for(unsigned i=0;i<48;++i) {input[i*2]=1200;input[i*2+1]=-1200;}
    assert(playback_pcm_reset(&pcm,7680,false));
    for(unsigned i=0;i<10;++i) playback_pcm_push(&pcm,input,48);
    playback_adjust_request(&s,report,target);
    assert(s.pending);
    playback_adjust_start(&s,0);
    unsigned due=0,remainder=0,packet_frames=0,ms=0;
    while(s.running && ms<3000) {
        ++ms; playback_pcm_push(&pcm,input,48);
        assert(playback_pcm_limit(&pcm,PLAYBACK_USB_MIN_FRAMES)==0);
        if(ms%s.interval) continue;
        due+=playback_adjust_samples(playback_adjust_interval(&s),s.interval,48000,&remainder);
        while(due>=480) {
            assert(playback_pcm_read_adjusted(&pcm,output,480,s.interval));
            for(unsigned i=64;i<480;++i) {assert(output[i*2]==1200);assert(output[i*2+1]==-1200);}
            due-=480; packet_frames+=480;
            if(packet_frames==1440) {playback_adjust_sent(&s,packet_frames,ms);packet_frames=0;}
        }
    }
    assert(!s.running && !s.failed && ms<2000);
    assert(playback_adjust_interval(&s)==10 && pcm.used<PLAYBACK_USB_MIN_FRAMES);
}
int main(void) {
    padded_window_boundaries();
    interpolation();
    live_transition(1590,60); live_transition(900,150);
    playback_adjust_t s={0};
    playback_adjust_request(&s,600,60); assert(!s.pending);
    playback_adjust_request(&s,0,60); assert(!s.pending);
    playback_adjust_request(&s,UINT16_MAX,60); assert(!s.pending);
    playback_adjust_request(&s,65534,60); assert(!s.pending && s.failed);
    playback_adjust_request(&s,5610,60); assert(!s.pending && s.failed);
    playback_adjust_request(&s,5600,60); assert(s.pending && !s.failed);
    playback_adjust_request(&s,1500,60); assert(s.goal_frames==80*48 && s.interval==11);
    playback_adjust_start(&s,UINT32_MAX-500);
    playback_adjust_poll(&s,9498); assert(s.running);
    playback_adjust_poll(&s,9499); assert(!s.running && s.failed);
    playback_adjust_cancel(&s); assert(!s.failed && playback_adjust_interval(&s)==10);
    // Fractional pacing debt survives both rate changes exactly.
    uint32_t remainder=0,total=0;
    for(unsigned i=0;i<99;++i) total+=playback_adjust_samples(11,10,48000,&remainder);
    assert(total==43200 && remainder==0);
    for(unsigned i=0;i<81;++i) total+=playback_adjust_samples(9,10,48000,&remainder);
    assert(total==86400 && remainder==0);
    puts("live playback: bounded drift, matched USB resampling, wrap, timeout and cancellation passed");
}
