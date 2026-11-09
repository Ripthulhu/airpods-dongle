// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "../src/btstack/accessory_automation.h"
#include "../src/btstack/playback_duck.h"
#include "../src/tinyusb/media_control.h"

static uint32_t now, session, host, pause_receipt;
static bool is_usb_streaming, phone_active, a2dp_is_connected_flag, ready, owned, model_known;
static bool call_active, call_opened, mic, mic_prepare_open, mic_recovery_pending;
static bool profile_pending, playback_released, queue_ok;
static struct {uint16_t avdtp_cid;} media_tracker;
static unsigned pauses, plays, cancellations;
static uint8_t cached_wear[40];
static uint16_t cached_length;
static uint32_t cached_session;
static uint32_t aacp_control_session(void) {return session;}
uint32_t usb_media_generation(void) {return host;}
static bool usb_host_active(void) {return (host&1u)!=0;}
static bool aacp_control_ready(void) {return ready;}
static bool aacp_settings_model_known(void) {return model_known;}
static bool aacp_audio_owned(void) {return owned;}
static bool usb_mic_requested(void) {return mic;}
static bool aacp_mic_requested(void) {return mic;}
static bool aacp_mic_active(void) {return mic;}
static uint32_t btstack_run_loop_get_time_ms(void) {return now;}
static uint16_t aacp_research_cached(uint8_t slot,uint8_t *out,uint16_t capacity) {
    assert(slot==5);
    if(!ready || cached_session!=session || capacity<cached_length)return 0;
    memcpy(out,cached_wear,cached_length);return cached_length;
}
uint32_t usb_media_pause_sent(void) {return pause_receipt;}
void usb_media_cancel_automatic(void) {++cancellations;}
bool usb_media_enqueue(enum usb_media_command command) {
    if(!queue_ok)return false;
    if(command==USB_MEDIA_AUTO_PAUSE)++pauses;
    else {assert(command==USB_MEDIA_AUTO_PLAY);++plays;}
    return true;
}
#include "playback_accessory.inc"

static void ear(uint8_t a,uint8_t b) {
    const uint8_t packet[]={4,0,4,0,6,0,a,b};
    memset(cached_wear,0,sizeof(cached_wear));
    cached_wear[0]=0x60;cached_wear[1]=0xea;cached_wear[4]=1; // A 60-second-old unchanged report is still current.
    memcpy(cached_wear+8,packet,sizeof(packet));
    cached_length=8+sizeof(packet);cached_session=session;
    dongle_playback_accessory_event(packet,sizeof(packet));
}
static void stage(uint8_t n) {
    const uint8_t packet[]={4,0,4,0,0x4b,0,0,0,0,n};
    dongle_playback_accessory_event(packet,sizeof(packet));
}
static void choice(uint8_t id,uint8_t value) {
    const uint8_t packet[]={4,0,4,0,9,0,id,value,0,0,0};
    dongle_playback_accessory_event(packet,sizeof(packet));
}
static void signal(void) {
    int16_t sample[]={123,-123};playback_accessory_signal(sample,1,false);
}
static void fixture(void) {
    memset(&playback_accessory,0,sizeof(playback_accessory));
    session=1;host=3;now=100;pause_receipt=0;
    is_usb_streaming=a2dp_is_connected_flag=ready=owned=queue_ok=model_known=true;
    phone_active=call_active=call_opened=mic=mic_prepare_open=mic_recovery_pending=false;
    profile_pending=playback_released=false;media_tracker.avdtp_cid=7;
    cached_length=0;cached_session=0;
    pauses=plays=cancellations=0;playback_accessory_poll();
}
static void paused(void) {
    ear(0,0);signal();ear(0,1);assert(pauses==1 && !plays);
    ++pause_receipt;now+=200;playback_accessory_poll();
    assert(playback_accessory.state.phase==ACCESSORY_AUTO_PAUSED);
}
static void test_wear_and_signal(void) {
    fixture();ear(0,0);assert(!pauses && !plays);
    signal();ear(0,2);assert(!pauses); // The other bud is safely docked.
    ear(0,0);ear(0,1);assert(pauses==1 && !plays);
    ++pause_receipt;now+=200;is_usb_streaming=false;playback_accessory_poll();
    ear(0,0);assert(plays==1);ear(0,0);assert(plays==1);
    fixture();ear(0,0);ear(1,0);assert(!pauses); // An idle player is never resumed.
    fixture();ear(0,0);float bad[]={NAN,INFINITY};
    playback_accessory_signal(bad,1,true);ear(0,1);assert(!pauses);
    fixture();ear(0,0);float quiet[]={0x1p-25f,-0x1p-25f};
    playback_accessory_signal(quiet,1,true);ear(0,1);assert(pauses==1);
}
static void test_cached_startup_presence(void) {
    fixture();ready=model_known=false;playback_accessory_poll();
    ear(0,0);assert(!playback_accessory.state.have_presence && !pauses && !plays);
    ready=true;playback_accessory_poll();
    assert(!playback_accessory.state.have_presence && !pauses && !plays);
    model_known=true;playback_accessory_poll();
    assert(playback_accessory.state.have_presence && playback_accessory.state.presence==ACCESSORY_EARS_FULL);
    assert(!pauses && !plays && !playback_accessory_duck());
    signal();ear(0,1);assert(pauses==1 && !plays);
    ++pause_receipt;now+=200;playback_accessory_poll();
    assert(playback_accessory.state.phase==ACCESSORY_AUTO_PAUSED);
    ++session;playback_accessory_poll();
    assert(!playback_accessory.state.have_presence && !plays);
    ear(0,0);assert(playback_accessory.state.have_presence && pauses==1 && !plays);

    for(unsigned invalid=0;invalid<5;++invalid) {
        fixture();model_known=false;playback_accessory_poll();ear(0,0);
        if(invalid==0)cached_length=15;
        if(invalid==1)cached_wear[8]=0;
        if(invalid==2)cached_wear[14]=4;
        if(invalid==3)cached_wear[13]=1;
        if(invalid==4) {
            cached_length=18;cached_wear[12]=0x4b;cached_wear[17]=1;
        }
        model_known=true;playback_accessory_poll();signal();playback_accessory_poll();
        assert(!playback_accessory.state.have_presence && !pauses && !plays && !playback_accessory_duck());
    }
}
static void test_owner_and_manual_fences(void) {
    for(unsigned reason=0;reason<10;++reason) {
        fixture();paused();unsigned old=cancellations;
        if(reason==0)++session;
        if(reason==1)host+=2;
        if(reason==2)++media_tracker.avdtp_cid;
        if(reason==3)owned=false;
        if(reason==4)call_active=true;
        if(reason==5)mic=true;
        if(reason==6)playback_accessory_manual();
        if(reason==7)host&=~1u;
        if(reason==8)choice(0x0a,1);
        if(reason==9)model_known=false;
        playback_accessory_poll();ear(0,0);
        assert(!plays && cancellations>old);
    }
    fixture();paused();signal();playback_accessory_poll();ear(0,0);assert(!plays);
    fixture();ear(0,0);signal();queue_ok=false;ear(0,1);queue_ok=true;
    now+=200;ear(0,0);assert(!plays && !pauses);
    fixture();choice(0x0a,1);mic=true;playback_accessory_poll();mic=false;
    playback_accessory_poll();ear(0,0);signal();ear(0,1);assert(!pauses);
    fixture();model_known=false;playback_accessory_poll();choice(0x0a,1);
    model_known=true;playback_accessory_poll();ear(0,0);signal();ear(0,1);assert(!pauses);
    fixture();ear(0,0);signal();ear(0,1);++pause_receipt;now+=20;
    ear(0,0);assert(playback_accessory.state.resume_pending && !plays);
    now+=200;choice(0x0a,1);assert(!plays); // The newest Off must beat a pending resume.
}
static void test_conversation_scope(void) {
    fixture();stage(2);assert(!playback_accessory_duck());
    stage(1);assert(playback_accessory_duck());
    choice(0x28,2);assert(!playback_accessory_duck());
    stage(1);assert(!playback_accessory_duck());
    choice(0x28,1);stage(1);now+=30000;assert(!playback_accessory_duck());
    stage(1);call_opened=true;playback_accessory_poll();assert(!playback_accessory_duck());
    fixture();is_usb_streaming=false;phone_active=true;playback_accessory_poll();
    stage(1);assert(playback_accessory_duck());ear(0,0);signal();ear(0,1);
    assert(!pauses && !plays);stage(4);assert(!playback_accessory_duck());
    fixture();stage(1);host+=2;playback_accessory_poll();assert(!playback_accessory_duck());
    dongle_playback_accessory_event(NULL,11);
}
int main(void) {
    test_wear_and_signal();test_cached_startup_presence();test_owner_and_manual_fences();test_conversation_scope();
    puts("driverless accessory bridge: actual event/poll wiring, scope, signal, manual controls and relay duck passed");
}
