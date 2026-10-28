// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/playback_options.h"

static void test_profiles(void) {
    assert(playback_options_valid((playback_options_t){0,0,0}));
    assert(playback_options_valid((playback_options_t){1,0,0}));
    assert(!playback_options_valid((playback_options_t){0,1,0}));
    assert(!playback_options_valid((playback_options_t){1,0,128000}));
    assert(!playback_options_valid((playback_options_t){2,1,128000}));
    assert(!playback_options_valid((playback_options_t){2,0,256000}));
    assert(!playback_options_valid((playback_options_t){4,0,128000}));
    const uint32_t rates[]={128000,174000,192000,220000,256000};
    uint8_t cap[]={0x4c,0,0,0,1,0x80,0,0x80,0,0x84,0,0x83,0xe8,0}, out[14];
    for (unsigned i=0;i<sizeof(rates)/sizeof(*rates);++i) {
        for (unsigned mode=0;mode<2;++mode) {
            playback_options_t p={3,(uint8_t)mode,rates[i]};
            assert(playback_options_valid(p) && playback_options_eld(cap,sizeof(cap),p,out));
            assert(playback_eld_bitrate(out)==(mode ? rates[i]/800*800 : rates[i]));
            assert(playback_options_eld_configuration(out,sizeof(out),p));
            assert(!playback_options_eld_configuration(out,sizeof(out)-1,p));
            playback_options_t wrong=p; wrong.rate_mode^=1;
            assert(!playback_options_eld_configuration(out,sizeof(out),wrong));
        }
    }
    playback_options_t requested={3,1,174000};
    cap[11]&=0x7f; assert(!playback_options_eld(cap,sizeof(cap),requested,out));
    requested.rate_mode=0; assert(playback_options_eld(cap,sizeof(cap),requested,out));
    cap[11]=2; cap[12]=0x58; cap[13]=0; // Peer ceiling 153600.
    assert(!playback_options_eld(cap,sizeof(cap),requested,out));
    requested.bitrate=0;
    assert(playback_options_eld(cap,sizeof(cap),requested,out) && playback_eld_bitrate(out)==153600);
    for (unsigned n=0;n<14;++n) assert(!playback_options_eld(cap,n,requested,out));
}
static void test_game_confirmation_and_restore(void) {
    playback_game_t s={0}; playback_game_request(&s,true);
    assert(!playback_game_step(&s,true,true,true,100,10,1500));
    assert(!playback_game_step(&s,true,true,true,599,10,1500));
    assert(playback_game_step(&s,true,true,true,600,10,1500)==60);
    playback_game_queued(&s,60,600,5); assert(s.phase==GAME_PENDING);
    playback_game_observe_send(&s,601,5,60,10); assert(s.phase==GAME_PENDING);
    playback_game_observe_send(&s,602,6,60,10); assert(s.phase==GAME_SENT);
    assert(!playback_game_step(&s,true,true,true,700,10,1500) && s.phase==GAME_SENT);
    assert(!playback_game_step(&s,true,true,true,701,11,0) && s.phase==GAME_SENT);
    assert(!playback_game_step(&s,true,true,true,702,12,600) && s.phase==GAME_CONFIRMED);
    // Microphone takeover restores while the user's preference stays on.
    assert(playback_game_step(&s,true,true,false,703,12,600)==150 && s.requested);
    playback_game_queued(&s,150,703,6);
    assert(!playback_game_step(&s,true,true,false,704,13,600));
    playback_game_observe_send(&s,705,7,150,13);
    assert(!playback_game_step(&s,true,true,false,706,14,1500) && !s.changed && s.phase==GAME_PENDING);
    assert(!playback_game_step(&s,true,true,true,800,14,1500));
    assert(playback_game_step(&s,true,true,true,1300,14,1500)==60);
    playback_game_queued(&s,60,1300,7); playback_game_observe_send(&s,1301,8,60,14);
    playback_game_request(&s,false);
    assert(playback_game_step(&s,true,true,true,1302,14,600)==150);
}
static void test_normal_boot_preparation(void) {
    playback_game_t s={.requested=false,.phase=GAME_OFF};
    assert(!playback_game_step(&s,false,true,true,0,0,600)); // No authority/readiness.
    assert(!playback_game_step(&s,true,false,true,0,0,600)); // Unsupported peer.
    assert(playback_game_step(&s,true,true,true,100,0,600)==150);
    assert(!s.changed && s.target==0 && s.phase==GAME_RESTORING); // No invented old60.
    playback_game_queued(&s,150,100,7);
    assert(!playback_game_step(&s,true,true,true,101,1,1500) && s.waiting_tx);
    playback_game_observe_send(&s,102,8,150,1);
    assert(!playback_game_step(&s,true,true,true,103,1,1500) && s.waiting_report);
    assert(!playback_game_step(&s,true,true,true,104,2,1500) && !s.changed && s.phase==GAME_OFF);
    for(unsigned i=0;i<100;++i)assert(!playback_game_step(&s,true,true,true,200+i,2,1500));
    playback_game_session_lost(&s);assert(s.target==0 && !s.requested);
    assert(playback_game_step(&s,true,true,true,400,2,1500)==150);
    // Busy before enqueue is bounded too, rather than retrying forever.
    s=(playback_game_t){0};
    assert(playback_game_step(&s,true,true,true,UINT32_MAX-999,0,600)==150);
    assert(playback_game_step(&s,true,true,true,999,0,600)==150);
    assert(!playback_game_step(&s,true,true,true,1000,0,600) && s.failed && s.restore_failed);
    for(unsigned i=0;i<100;++i)assert(!playback_game_step(&s,true,true,true,2000+i,1,1500));
    s=(playback_game_t){0};
    assert(playback_game_step(&s,true,true,true,0,0,600)==150);
    playback_game_queued(&s,150,0,0);playback_game_observe_send(&s,1,1,150,0);
    assert(!playback_game_step(&s,true,true,true,2001,0,1500) && s.restore_failed);
    playback_game_request(&s,true);
    assert(s.requested && playback_game_step(&s,true,true,true,2002,0,1500)==150);
    playback_game_queued(&s,150,2002,1);playback_game_observe_send(&s,2003,2,150,0);
    assert(playback_game_step(&s,true,true,true,2004,1,1500)==60 && !s.changed && s.requested);
}
static void test_catalog_preflight(void) {
    playback_options_t automatic={0,0,0}, aac={2,0,220000}, eld={3,1,174000};
    // Unknown/partial catalogs never turn the absence of a matching entry into
    // evidence against the requested codec. Complete catalogs can reject it.
    assert(playback_options_preflight(automatic,true,0));
    assert(playback_options_preflight(aac,false,0));
    assert(playback_options_preflight(eld,false,1u<<1));
    assert(!playback_options_preflight(aac,true,1u<<1));
    assert(!playback_options_preflight(eld,true,(1u<<1)|(1u<<2)));
    assert(playback_options_preflight(aac,true,1u<<2));
    assert(playback_options_preflight(eld,true,1u<<3));
    assert(!playback_options_preflight((playback_options_t){255,0,0},false,255));
    assert(playback_options_aac_supported(aac,0x20,8,4,256000));
    assert(playback_options_aac_supported(aac,0x40,8,4,0)); // Unspecified ceiling.
    assert(!playback_options_aac_supported(aac,0x20,8,4,192000));
    assert(!playback_options_aac_supported(aac,0x80,8,4,256000));
    assert(!playback_options_aac_supported(aac,0x20,16,4,256000));
    assert(!playback_options_aac_supported(aac,0x20,8,8,256000));
    aac.bitrate=0;
    assert(playback_options_aac_supported(aac,0x20,8,4,128000));
    assert(!playback_options_aac_supported(aac,0x20,8,4,95999));
    aac.bitrate=96000;
    assert(playback_options_aac_supported(aac,0x20,8,4,96000));
}
static void test_game_faults_wrap_and_session_loss(void) {
    playback_game_t s={0}; playback_game_request(&s,true);
    assert(!playback_game_step(&s,true,false,true,0,0,UINT16_MAX) && s.phase==GAME_UNSUPPORTED);
    playback_game_queued(&s,60,UINT32_MAX-999,UINT32_MAX);
    playback_game_observe_send(&s,UINT32_MAX-998,1,60,UINT32_MAX);
    assert(!playback_game_step(&s,true,true,true,0,1,600) && s.phase==GAME_CONFIRMED);
    playback_game_session_lost(&s);
    assert(!playback_game_step(&s,false,false,false,1,0,UINT16_MAX) && s.phase==GAME_RESTORING);
    assert(playback_game_step(&s,true,true,true,100,1,600)==150);
    playback_game_queued(&s,150,100,1); playback_game_observe_send(&s,101,2,150,1);
    assert(!playback_game_step(&s,true,true,true,2101,2,1500));
    assert(s.phase==GAME_FAILED && s.restore_failed && s.changed);
    // No endless retry after a missing restore confirmation.
    for (unsigned i=0;i<1000;++i) assert(!playback_game_step(&s,true,true,true,2200+i,3,1500));
    playback_game_request(&s,false);
    assert(playback_game_step(&s,true,true,false,4000,3,1500)==150);
    s=(playback_game_t){0}; playback_game_request(&s,true);
    playback_game_queued(&s,60,0,1); playback_game_observe_send(&s,1,2,60,0);
    assert(playback_game_step(&s,true,true,true,2001,1,600)==150 && s.failed);
    // A new report outside the bounded wait cannot confirm the expired request.
    assert(s.phase==GAME_RESTORING);
}
int main(void) {
    test_profiles(); test_catalog_preflight(); test_game_confirmation_and_restore(); test_game_faults_wrap_and_session_loss();
    test_normal_boot_preparation();
    puts("playback options: explicit codec/rate bounds, VBR negotiation, fresh Game Mode reports and restoration passed");
}
