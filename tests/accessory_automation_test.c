// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/accessory_automation.h"

static void ears(accessory_automation_t *s,uint8_t left,uint8_t right,uint32_t now) {
    const uint8_t p[]={4,0,4,0,6,0,left,right};
    accessory_automation_packet(s,p,sizeof(p),now);
}
static void speech(accessory_automation_t *s,uint8_t stage,uint32_t now) {
    const uint8_t p[]={4,0,4,0,0x4b,0,0,0,0,stage};
    accessory_automation_packet(s,p,sizeof(p),now);
}
static void fixture(accessory_automation_t *s) {
    accessory_automation_reset(s);
    assert(accessory_automation_poll(s,0,false,false,0)==ACCESSORY_AUTOMATION_CANCEL);
    assert(!accessory_automation_poll(s,0,false,false,0));
    assert(s->ear_enabled && s->ca_enabled && !s->have_presence);
}
static void pause_sent(accessory_automation_t *s,uint32_t now,uint32_t receipt) {
    fixture(s);ears(s,0,0,now);
    assert(!accessory_automation_poll(s,now,true,true,receipt));
    ears(s,0,1,now);
    assert(accessory_automation_poll(s,now,true,true,receipt)==ACCESSORY_AUTOMATION_PAUSE);
    accessory_automation_sent(s,ACCESSORY_AUTOMATION_PAUSE,true,receipt,now);
    assert(s->phase==ACCESSORY_AUTO_PAUSE_SENT);
}
static void paused(accessory_automation_t *s,uint32_t now,uint32_t receipt) {
    pause_sent(s,now,receipt);
    assert(!accessory_automation_poll(s,now+200u,false,false,receipt+1u));
    assert(s->phase==ACCESSORY_AUTO_PAUSED);
}

static void test_initial_presence_duplicates_and_case_roles(void) {
    accessory_automation_t s;
    for(uint8_t left=0;left<=3;++left)for(uint8_t right=0;right<=3;++right) {
        fixture(&s);ears(&s,left,right,10);
        unsigned in=(left==0)+(right==0);
        uint8_t expected=in==2 || (in==1 && (left==2 || right==2)) ? ACCESSORY_EARS_FULL :
            in==1 ? ACCESSORY_EARS_PARTIAL : ACCESSORY_EARS_NONE;
        assert(s.have_presence && s.presence==expected);
        assert(!accessory_automation_poll(&s,10,true,true,0));
        ears(&s,left,right,11);assert(!accessory_automation_poll(&s,11,true,true,0));
    }
    fixture(&s);ears(&s,0,0,0);ears(&s,0,2,1);
    assert(s.presence==ACCESSORY_EARS_FULL && !accessory_automation_poll(&s,1,true,true,0));
    ears(&s,2,0,2);assert(!accessory_automation_poll(&s,2,true,true,0));
    ears(&s,2,2,3);assert(accessory_automation_poll(&s,3,true,true,0)==ACCESSORY_AUTOMATION_PAUSE);
    fixture(&s);ears(&s,1,0,0);ears(&s,1,1,1);
    assert(accessory_automation_poll(&s,1,true,true,0)==ACCESSORY_AUTOMATION_PAUSE);
    fixture(&s);ears(&s,0,0,0);ears(&s,0,3,1);
    assert(accessory_automation_poll(&s,1,true,true,0)==ACCESSORY_AUTOMATION_PAUSE);

    fixture(&s);ears(&s,0,0,0);ears(&s,0,1,1);
    assert(!accessory_automation_poll(&s,1,false,true,0)); // Open/zero source cannot authorize Pause.
    ears(&s,0,0,2);assert(!accessory_automation_poll(&s,2,false,false,0)); // No initial autoplay.
}

static void test_packet_and_choice_validation(void) {
    accessory_automation_t s,before;fixture(&s);before=s;
    const uint8_t valid[]={4,0,4,0,6,0,0,0};
    for(uint16_t length=0;length<8;++length)accessory_automation_packet(&s,valid,length,1);
    accessory_automation_packet(&s,NULL,8,1);
    uint8_t p[11]={4,0,4,0,6,0,0,0};
    p[0]=0;accessory_automation_packet(&s,p,8,1);p[0]=4;
    p[5]=1;accessory_automation_packet(&s,p,8,1);p[5]=0;
    p[6]=4;accessory_automation_packet(&s,p,8,1);p[6]=0;
    p[7]=255;accessory_automation_packet(&s,p,8,1);
    assert(!memcmp(&s,&before,sizeof(s)));
    accessory_automation_choice(&s,0x0a,0);accessory_automation_choice(&s,0x28,3);
    accessory_automation_choice(&s,0x26,2);assert(s.ear_enabled && s.ca_enabled);
    p[4]=9;p[6]=0x0a;p[7]=1;p[8]=1;
    accessory_automation_packet(&s,p,11,1);assert(s.ear_enabled);
    p[8]=0;accessory_automation_packet(&s,p,10,1);assert(s.ear_enabled);
    accessory_automation_packet(&s,p,11,1);assert(!s.ear_enabled);
    assert(accessory_automation_poll(&s,1,false,false,0)==ACCESSORY_AUTOMATION_CANCEL);
    ears(&s,0,0,2);ears(&s,0,1,3);assert(!accessory_automation_poll(&s,3,true,true,0));
    accessory_automation_choice(&s,0x0a,2);assert(s.ear_enabled);
    assert(!accessory_automation_poll(&s,4,false,false,0));
    speech(&s,1,4);assert(accessory_automation_duck(&s,4));
    accessory_automation_choice(&s,0x28,2);assert(!s.ca_enabled && !accessory_automation_duck(&s,4));
    speech(&s,1,5);assert(!accessory_automation_duck(&s,5));
    accessory_automation_choice(&s,0x28,1);assert(s.ca_enabled && !accessory_automation_duck(&s,5));
}

static void test_pause_requires_delivery_then_quiescence(void) {
    accessory_automation_t s;pause_sent(&s,100,7);
    assert(!accessory_automation_poll(&s,200,false,false,7));
    assert(s.phase==ACCESSORY_AUTO_PAUSE_SENT); // Enqueue and silence are not delivery.
    ears(&s,0,0,201);
    assert(accessory_automation_poll(&s,201,false,false,7)==ACCESSORY_AUTOMATION_CANCEL);
    assert(!accessory_automation_poll(&s,300,false,false,8));

    pause_sent(&s,100,7);
    assert(!accessory_automation_poll(&s,200,true,true,8));
    assert(s.phase==ACCESSORY_AUTO_PAUSE_DELIVERED);
    ears(&s,0,0,201);
    assert(!accessory_automation_poll(&s,201,true,true,8));
    assert(s.resume_pending && s.phase==ACCESSORY_AUTO_PAUSE_DELIVERED);
    assert(!accessory_automation_poll(&s,1299,true,true,8));
    assert(accessory_automation_poll(&s,1300,false,false,8)==ACCESSORY_AUTOMATION_PLAY);
    assert(!accessory_automation_poll(&s,1301,false,false,8));
    accessory_automation_sent(&s,ACCESSORY_AUTOMATION_PLAY,true,8,1301);
    assert(accessory_automation_poll(&s,1500,true,true,8)==ACCESSORY_AUTOMATION_CANCEL);
    assert(!accessory_automation_poll(&s,1501,true,true,8));

    pause_sent(&s,100,7);ears(&s,0,0,101);
    assert(accessory_automation_poll(&s,101,true,true,7)==ACCESSORY_AUTOMATION_CANCEL);
    assert(!accessory_automation_poll(&s,1200,false,false,8));
    pause_sent(&s,100,7);
    assert(!accessory_automation_poll(&s,1599,true,true,8));
    assert(accessory_automation_poll(&s,1600,true,true,8)==ACCESSORY_AUTOMATION_CANCEL);
    ears(&s,0,0,1601);assert(!accessory_automation_poll(&s,1601,false,false,8));

    paused(&s,100,7);ears(&s,0,0,301);
    assert(accessory_automation_poll(&s,301,true,true,8)==ACCESSORY_AUTOMATION_CANCEL);
    assert(!accessory_automation_poll(&s,302,false,false,8)); // Manual/external playback retires resume.

    pause_sent(&s,100,7);assert(!accessory_automation_poll(&s,200,true,true,8));
    ears(&s,0,0,201);assert(!accessory_automation_poll(&s,201,true,true,8));
    assert(s.resume_pending);
    ears(&s,0,1,202);assert(!accessory_automation_poll(&s,202,true,true,8));
    assert(!s.resume_pending);
    assert(!accessory_automation_poll(&s,400,false,false,8)); // A second removal supersedes the pending return.
    ears(&s,0,0,401);assert(accessory_automation_poll(&s,401,false,false,8)==ACCESSORY_AUTOMATION_PLAY);
}

static void test_cancellation_queue_failure_and_resume_bounds(void) {
    accessory_automation_t s;paused(&s,100,7);
    accessory_automation_cancel_media(&s);ears(&s,0,0,301);
    assert(accessory_automation_poll(&s,301,false,false,8)==ACCESSORY_AUTOMATION_CANCEL);
    assert(!accessory_automation_poll(&s,302,false,false,8));
    paused(&s,100,7);accessory_automation_choice(&s,0x0a,1);ears(&s,0,0,301);
    assert(accessory_automation_poll(&s,301,false,false,8)==ACCESSORY_AUTOMATION_CANCEL);
    assert(!s.ear_enabled && !accessory_automation_poll(&s,302,false,false,8));
    fixture(&s);ears(&s,0,0,100);ears(&s,0,1,101);
    assert(accessory_automation_poll(&s,101,true,true,7)==ACCESSORY_AUTOMATION_PAUSE);
    accessory_automation_sent(&s,ACCESSORY_AUTOMATION_PAUSE,false,7,101);
    assert(accessory_automation_poll(&s,102,true,true,7)==ACCESSORY_AUTOMATION_CANCEL);
    ears(&s,0,1,103);assert(!accessory_automation_poll(&s,103,true,true,7));
    ears(&s,0,0,104);assert(!accessory_automation_poll(&s,104,false,false,7));
    paused(&s,100,7);ears(&s,0,0,301);
    assert(accessory_automation_poll(&s,301,false,false,8)==ACCESSORY_AUTOMATION_PLAY);
    accessory_automation_sent(&s,ACCESSORY_AUTOMATION_PLAY,false,8,301);
    assert(accessory_automation_poll(&s,302,false,false,8)==ACCESSORY_AUTOMATION_CANCEL);
    assert(!accessory_automation_poll(&s,303,false,false,8));

    paused(&s,100,7);assert(!accessory_automation_poll(&s,120099,false,false,8));
    assert(accessory_automation_poll(&s,120100,false,false,8)==ACCESSORY_AUTOMATION_CANCEL);
    ears(&s,0,0,120101);assert(!accessory_automation_poll(&s,120101,false,false,8));
    paused(&s,100,7);ears(&s,0,0,301);
    assert(accessory_automation_poll(&s,301,false,false,8)==ACCESSORY_AUTOMATION_PLAY);
    accessory_automation_sent(&s,ACCESSORY_AUTOMATION_PLAY,true,8,301);
    assert(!accessory_automation_poll(&s,1800,false,false,8));
    assert(accessory_automation_poll(&s,1801,false,false,8)==ACCESSORY_AUTOMATION_CANCEL);

    paused(&s,UINT32_MAX-100u,UINT32_MAX);
    ears(&s,0,0,200);assert(accessory_automation_poll(&s,200,false,false,0)==ACCESSORY_AUTOMATION_PLAY);
    pause_sent(&s,UINT32_MAX-100u,7);
    assert(accessory_automation_poll(&s,1399,true,true,7)==ACCESSORY_AUTOMATION_CANCEL);
    paused(&s,100,7);speech(&s,1,301);accessory_automation_reset(&s);
    assert(!s.have_presence && !s.duck && s.ear_enabled && s.ca_enabled);
    assert(accessory_automation_poll(&s,302,false,false,8)==ACCESSORY_AUTOMATION_CANCEL);
    ears(&s,0,0,303);assert(!accessory_automation_poll(&s,303,false,false,8));
}

static void test_bounded_conversation_awareness(void) {
    accessory_automation_t s;fixture(&s);
    speech(&s,2,1);speech(&s,3,2);assert(!accessory_automation_duck(&s,2));
    speech(&s,1,100);assert(accessory_automation_duck(&s,100));
    speech(&s,2,30099);assert(accessory_automation_duck(&s,60098));
    assert(!accessory_automation_duck(&s,60099));
    speech(&s,3,60100);assert(!accessory_automation_duck(&s,60100));
    speech(&s,1,100);speech(&s,2,30100);assert(!accessory_automation_duck(&s,30100));
    const uint8_t stop[]={4,6,7,8,9};
    for(unsigned i=0;i<sizeof(stop);++i) {
        speech(&s,1,100);speech(&s,stop[i],101);assert(!accessory_automation_duck(&s,101));
        speech(&s,2,102);assert(!accessory_automation_duck(&s,102));
    }
    speech(&s,1,100);speech(&s,0,101);speech(&s,5,102);speech(&s,255,103);
    assert(accessory_automation_duck(&s,30099));assert(!accessory_automation_duck(&s,30100));
    speech(&s,1,UINT32_MAX-100u);assert(accessory_automation_duck(&s,29898));
    assert(!accessory_automation_duck(&s,29899));
    const uint8_t short_packet[]={4,0,4,0,0x4b,0,0,0,0};
    accessory_automation_packet(&s,short_packet,sizeof(short_packet),30000);
    assert(!accessory_automation_duck(&s,30000));
}

int main(void) {
    test_initial_presence_duplicates_and_case_roles();
    test_packet_and_choice_validation();
    test_pause_requires_delivery_then_quiescence();
    test_cancellation_queue_failure_and_resume_bounds();
    test_bounded_conversation_awareness();
    puts("accessory automation: validated wear/speech, delivered-pause idle proof, one-shot actions, manual/queue/owner cancellation and bounded wrap-safe leases passed");
}
