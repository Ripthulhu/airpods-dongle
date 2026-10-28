// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/tinyusb/dongle_features.c"

static unsigned setting_writes, profile_writes, game_writes, starts, stops, polls;
static uint8_t result=DONGLE_ACCEPTED;
static uint8_t expected_setting_id=0x28;
static bool diagnostic_scan_paused;
static bool inspection_active;
uint16_t phone_sink_diagnostics(uint8_t *out,uint16_t cap) {
    if(cap<64)return 0;
    memset(out,0,64);out[0]=2;out[3]=15;return 64;
}
void dongle_playback_inspect_enable(bool active) {inspection_active=active;}
uint16_t dongle_playback_inspect(uint8_t *out,uint16_t cap) {
    if(cap<400)return 0;
    memset(out,0,400);out[0]=1;return 400;
}
void ble_scan_diagnostic_pause(bool paused) {diagnostic_scan_paused=paused;}
static uint32_t expected_setting_value=UINT32_C(0x89abcdef);
uint8_t aacp_setting_write(uint8_t id,uint32_t value) {
    assert(id==expected_setting_id && value==expected_setting_value);setting_writes++;return result;
}
uint16_t aacp_settings_snapshot(uint8_t *out,uint16_t cap) {
    if(cap<2)return 0;
    out[0]=1;out[1]=0;return 2;
}
uint8_t dongle_playback_configure(uint8_t codec,uint8_t mode,uint32_t bitrate) {
    assert(codec==3 && mode==1 && bitrate==174000);profile_writes++;return result;
}
uint8_t dongle_playback_game_mode(bool enabled) {assert(enabled);game_writes++;return result;}
uint16_t dongle_playback_status(uint8_t *out,uint16_t cap) {
    if(cap<20)return 0;
    memset(out,0,20);out[0]=1;return 20;
}
uint16_t dongle_playback_diagnostics(uint8_t *out,uint16_t cap) {
    if(cap<64)return 0;
    memset(out,0,64);out[0]=1;return 64;
}
uint16_t dongle_playback_trace(uint8_t *out,uint16_t cap) {
    if(cap<512)return 0;
    memset(out,0,512);out[0]=1;return 512;
}
uint8_t dongle_finder_start(uint8_t client) {assert(client==1);starts++;return result;}
uint8_t dongle_finder_stop(uint8_t client) {assert(client==1);stops++;return result;}
uint16_t dongle_finder_snapshot(uint8_t client,uint8_t *out,uint16_t cap) {
    assert(client==1);if(cap<24)return 0;memset(out,0,24);out[0]=1;return 24;
}
void dongle_playback_poll(void) {polls++;}
void dongle_finder_poll(void) {polls++;}
void dongle_finder_disconnect(uint8_t client) {assert(client==1);stops++;}

int main(void) {
    uint8_t out[64],p[64]={1,0x28,0xef,0xcd,0xab,0x89};uint16_t n=99;
    assert(dongle_features_request(1,NULL,1,out,64,&n)==1);
    assert(dongle_features_request(1,p,0,out,64,&n)==1);
    assert(dongle_features_request(1,p,6,out,64,NULL)==1);
    for(unsigned size=1;size<6;size++)assert(dongle_features_request(1,p,size,out,64,&n)==1);
    assert(!setting_writes);
    assert(dongle_features_request(1,p,6,p,64,&n)==0 && n==0 && setting_writes==1);
    const uint8_t results[]={DONGLE_BUSY,DONGLE_STALE,DONGLE_DISCONNECTED,DONGLE_UNSUPPORTED,DONGLE_MALFORMED};
    const uint8_t errors[]={2,3,4,7,1};
    for(unsigned i=0;i<sizeof(results);i++){
        result=results[i];assert(dongle_features_request(1,p,6,out,64,&n)==errors[i] && n==0);
    }
    result=DONGLE_ACCEPTED;
    const uint8_t hold_cycle[]={1,0x1a,11,0,0,0};
    expected_setting_id=0x1a;expected_setting_value=11;
    for(uint8_t client=0;client<2;++client)
        assert(dongle_features_request(client,hold_cycle,sizeof(hold_cycle),out,64,&n)==0 && !n);
    assert(setting_writes==8);
    uint8_t profile[]={3,3,1,0xb0,0xa7,2,0};
    assert(dongle_features_request(1,profile,7,out,19,&n)==1 && !profile_writes);
    assert(dongle_features_request(1,profile,7,out,64,&n)==0 && n==20 && profile_writes==1);
    uint8_t game[]={4,1};
    assert(dongle_features_request(1,game,2,out,19,&n)==1 && !game_writes);
    game[1]=2;assert(dongle_features_request(1,game,2,out,64,&n)==1 && !game_writes);
    game[1]=1;assert(dongle_features_request(1,game,2,out,64,&n)==0 && n==20 && game_writes==1);
    p[0]=6;assert(dongle_features_request(1,p,1,out,23,&n)==1 && !starts);
    assert(dongle_features_request(1,p,1,out,64,&n)==0 && n==24 && starts==1);
    p[0]=7;assert(dongle_features_request(1,p,1,out,64,&n)==0 && n==24 && starts==1);
    p[0]=8;assert(dongle_features_request(1,p,1,out,64,&n)==0 && n==0 && stops==1);
    p[0]=2;assert(dongle_features_request(1,p,1,out,64,&n)==0 && n==2 && out[0]==1);
    p[0]=5;assert(dongle_features_request(1,p,1,out,64,&n)==0 && n==64);
    p[0]=9;assert(dongle_features_request(1,p,1,out,64,&n)==0 && n==20);
    uint8_t trace[512];p[0]=11;
    assert(dongle_features_request(1,p,1,trace,511,&n)==7 && !n);
    assert(dongle_features_request(1,p,1,trace,512,&n)==0 && n==512 && trace[0]==1);
    const uint8_t scan_off[]={12,0},scan_on[]={12,1},bad_scan[]={12,2};
    assert(dongle_features_request(1,scan_off,2,out,64,&n)==0 && !n && diagnostic_scan_paused);
    assert(dongle_features_request(1,scan_on,2,out,64,&n)==0 && !diagnostic_scan_paused);
    assert(dongle_features_request(1,bad_scan,2,out,64,&n)==1);
    p[0]=13;assert(dongle_features_request(1,p,1,trace,399,&n)==7 && !n);
    assert(dongle_features_request(1,p,1,trace,400,&n)==0 && n==400 && trace[0]==1);
    const uint8_t enable[]={14,1},disable[]={14,0},bad_enable[]={14,2};
    assert(dongle_features_request(1,enable,2,out,64,&n)==0 && inspection_active && !n);
    assert(dongle_features_request(1,disable,2,out,64,&n)==0 && !inspection_active);
    assert(dongle_features_request(1,bad_enable,2,out,64,&n)==1);
    p[0]=15;
    assert(dongle_features_request(1,p,1,out,63,&n)==7 && n==0);
    assert(dongle_features_request(1,p,2,out,64,&n)==1 && n==0);
    assert(dongle_features_request(1,p,1,out,64,&n)==0 && n==64 && out[0]==2 && out[3]==15);
    p[0]=0;assert(dongle_features_request(1,p,1,out,64,&n)==7 && n==0);
    dongle_features_poll();dongle_features_disconnect(1);assert(polls==2 && stops==2);
    puts("typed features: validation, exact integer values, aliased buffers, bounded responses and result mapping passed");
}
