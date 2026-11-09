// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/tinyusb/dongle_features.c"

static unsigned setting_writes, profile_writes, game_writes, starts, stops, polls;
static unsigned legacy_snapshots,extended_snapshots;
static uint8_t result=DONGLE_ACCEPTED;
static uint8_t expected_setting_id=0x28;
static bool diagnostic_scan_paused;
static bool inspection_active;
static unsigned volume_reads,volume_writes,volume_disconnects;
static unsigned flush_comparison_calls;
static bool flush_comparison_active,flush_comparison_ready=true;
uint32_t btstack_run_loop_get_time_ms(void) {return 12345;}
static unsigned radio_reads,adaptive_reads;
static unsigned routing_reads,routing_writes;
static unsigned policy_queries,policy_changes;
static bool automatic_reconnect;
uint8_t dongle_reconnect_policy(bool set,bool automatic,bool *current) {
    if(set)++policy_changes;else ++policy_queries;
    if(result!=DONGLE_ACCEPTED)return result;
    if(set)automatic_reconnect=automatic;
    *current=automatic_reconnect;return DONGLE_ACCEPTED;
}
uint16_t dongle_routing_snapshot(uint8_t *out,uint16_t capacity) {
    ++routing_reads;if(capacity<32)return 0;
    memset(out,0,32);out[0]=1;out[1]=32;return 32;
}
uint8_t dongle_routing_request(uint8_t action,uint32_t expected,uint32_t nonce) {
    assert(action==1 && expected==17 && nonce==999);++routing_writes;return result;
}
bool bt_hci_radio_diagnostics(bt_hci_radio_diagnostics_t *out) {
    ++radio_reads;memset(out,0,sizeof(*out));
    out->epoch=17;out->handle=0x123;out->active=true;out->pending=true;
    out->valid=31;out->failed=2;out->unavailable=8;out->rssi=-12;out->quality=200;
    out->tx_power=-4;out->max_tx_power=9;out->afh_mode=1;out->afh_map[0]=0x55;
    for(unsigned i=0;i<5;++i)out->age_ms[i]=i?i*100:UINT32_MAX;
    out->flush_events=5;out->completed_packets=987;return true;
}
uint16_t dongle_playback_adaptive_snapshot(uint8_t *out,uint16_t capacity) {
    ++adaptive_reads;assert(capacity==64);
    memset(out,0,64);out[0]=1;out[1]=3;dongle_put32(out+4,192000);return 64;
}
bool bt_hci_airpods_flush_comparison(bool enabled) {
    ++flush_comparison_calls;
    if(enabled && !flush_comparison_ready)return false;
    flush_comparison_active=enabled;return true;
}
uint16_t dongle_volume_snapshot(uint8_t client,uint8_t *out,uint16_t capacity) {
    assert(client<2);++volume_reads;
    if(capacity<24)return 0;
    memset(out,0,24);out[0]=1;out[2]=47;return 24;
}
uint8_t dongle_volume_request(uint8_t client,const uint8_t *p,uint16_t size,
                             uint8_t *out,uint16_t capacity,uint16_t *length) {
    assert(client==0 && size==7 && p[0]==17 && capacity>=24);
    uint8_t action=p[1],level=p[6];uint32_t token=dongle_u32(p+2);
    ++volume_writes;
    if(result!=DONGLE_ACCEPTED)return result;
    memset(out,0,24);out[0]=1;out[2]=level;
    out[3]=action;dongle_put32(out+8,token);*length=24;
    return result;
}
void dongle_volume_client_disconnect(uint8_t client) {assert(client<2);++volume_disconnects;}
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
    ++legacy_snapshots;
    if(cap<2)return 0;
    out[0]=1;out[1]=0;return 2;
}
uint16_t aacp_settings_snapshot_extended(uint8_t *out,uint16_t cap) {
    ++extended_snapshots;
    if(cap<15)return 0;
    memset(out,0,15);out[0]=1;out[1]=1;out[2]=0x37;return 15;
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
void dongle_finder_disconnect(uint8_t client) {assert(client<2);stops++;}

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
    assert(legacy_snapshots==1 && !extended_snapshots);
    const uint8_t extended[]={2,1};
    assert(dongle_features_request(1,extended,2,out,64,&n)==0 && n==15 && out[1]==1 && out[2]==0x37);
    assert(legacy_snapshots==1 && extended_snapshots==1);
    assert(dongle_features_request(1,extended,2,out,14,&n)==7 && !n);
    assert(dongle_features_request(1,p,1,out,1,&n)==7 && !n);
    unsigned old_legacy=legacy_snapshots,old_extended=extended_snapshots;
    const uint8_t bad_selectors[][3]={{2,0,0},{2,2,0},{2,255,0},{2,1,0}};
    for(unsigned i=0;i<sizeof(bad_selectors)/sizeof(*bad_selectors);++i)
        assert(dongle_features_request(1,bad_selectors[i],i==3 ? 3 : 2,out,64,&n)==1 && !n);
    assert(legacy_snapshots==old_legacy && extended_snapshots==old_extended);
    memcpy(p,extended,sizeof(extended));
    assert(dongle_features_request(1,p,2,p,64,&n)==0 && n==15 && p[0]==1 && p[1]==1 && p[2]==0x37);
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
    p[0]=16;
    assert(dongle_features_request(1,p,2,out,64,&n)==1 && !volume_reads);
    assert(dongle_features_request(1,p,1,out,23,&n)==1 && !volume_reads);
    assert(dongle_features_request(1,p,1,out,64,&n)==0 && n==24 && out[2]==47 && volume_reads==1);
    uint8_t acquire[]={17,0,0,0,0,0,0};
    for(unsigned size=1;size<7;++size)
        assert(dongle_features_request(0,acquire,size,out,64,&n)==1 && !volume_writes);
    assert(dongle_features_request(0,acquire,7,out,23,&n)==1 && !volume_writes);
    assert(dongle_features_request(1,acquire,7,out,64,&n)==7 && !volume_writes);
    assert(dongle_features_request(0,acquire,7,out,64,&n)==0 && n==24 && volume_writes==1);
    const uint8_t bad[][7]={{17,4,1,0,0,0,0},{17,0,1,0,0,0,0},{17,0,0,0,0,0,1},
        {17,1,0,0,0,0,0},{17,2,1,0,0,0,1},{17,3,1,0,0,0,128}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i)
        assert(dongle_features_request(0,bad[i],7,out,64,&n)==1 && volume_writes==1);
    uint8_t absolute[]={17,3,0x78,0x56,0x34,0x12,127};
    for(unsigned i=0;i<sizeof(results);++i){
        result=results[i];
        assert(dongle_features_request(0,absolute,7,out,64,&n)==errors[i] && n==0);
    }
    result=DONGLE_ACCEPTED;
    memcpy(p,absolute,sizeof(absolute));
    assert(dongle_features_request(0,p,7,p,64,&n)==0 && n==24 && p[2]==127 &&
        p[3]==3 && dongle_u32(p+8)==UINT32_C(0x12345678));
    uint8_t comparison[]={18,1};
    assert(dongle_features_request(0,comparison,1,out,64,&n)==1 && !flush_comparison_calls);
    assert(dongle_features_request(0,comparison,3,out,64,&n)==1 && !flush_comparison_calls);
    comparison[1]=2;
    assert(dongle_features_request(0,comparison,2,out,64,&n)==1 && !flush_comparison_calls);
    comparison[1]=1;
    assert(dongle_features_request(1,comparison,2,out,64,&n)==7 && !flush_comparison_calls);
    flush_comparison_ready=false;
    assert(dongle_features_request(0,comparison,2,out,64,&n)==2 && !n && !flush_comparison_active);
    flush_comparison_ready=true;
    assert(dongle_features_request(0,comparison,2,comparison,2,&n)==0 && !n && flush_comparison_active);
    comparison[1]=0;
    assert(dongle_features_request(0,comparison,2,out,64,&n)==0 && !n && !flush_comparison_active);
    p[0]=0;assert(dongle_features_request(1,p,1,out,64,&n)==7 && n==0);
    p[0]=19;
    assert(dongle_features_request(0,p,2,trace,128,&n)==1 && !radio_reads);
    assert(dongle_features_request(0,p,1,trace,127,&n)==7 && !radio_reads);
    for(uint8_t client=0;client<2;++client) {
        trace[0]=19;
        assert(dongle_features_request(client,trace,1,trace,128,&n)==0 && n==128);
        assert(trace[0]==1 && trace[1]==128 && trace[2]==7 && trace[3]==31);
        assert(dongle_u32(trace+4)==12345 && dongle_u32(trace+8)==17);
        assert(trace[12]==0x23 && trace[13]==1 && trace[14]==2 && trace[15]==8);
        assert((int8_t)trace[16]==-12 && trace[17]==200 && (int8_t)trace[18]==-4);
        assert(trace[19]==9 && trace[20]==1 && trace[21]==0x55 && trace[31]==0);
        assert(dongle_u32(trace+32)==UINT32_MAX && dongle_u32(trace+48)==400);
        assert(dongle_u32(trace+52)==5 && dongle_u32(trace+56)==987 && !dongle_u32(trace+60));
        assert(trace[64]==1 && trace[65]==3 && dongle_u32(trace+68)==192000);
    }
    assert(radio_reads==2 && adaptive_reads==2);
    dongle_features_poll();dongle_features_disconnect(1);
    assert(polls==2 && stops==2 && volume_disconnects==1);
    comparison[1]=1;
    assert(dongle_features_request(0,comparison,2,out,64,&n)==0 && flush_comparison_active);
    dongle_features_disconnect(0);
    assert(!flush_comparison_active && stops==3 && volume_disconnects==2);
    p[0]=20;
    assert(dongle_features_request(0,p,2,out,64,&n)==1);
    assert(dongle_features_request(0,p,1,out,31,&n)==7);
    assert(dongle_features_request(0,p,1,out,32,&n)==0 && n==32 && out[1]==32);
    uint8_t route[32]={21,1,17,0,0,0,0xe7,3,0,0};
    for(unsigned size=1;size<10;++size)assert(dongle_features_request(0,route,size,out,64,&n)==1);
    assert(!routing_writes);
    assert(dongle_features_request(0,route,10,out,31,&n)==1 && !routing_writes);
    assert(dongle_features_request(0,route,10,route,32,&n)==0 && n==32 && routing_writes==1);
    uint8_t route_busy[]={21,1,17,0,0,0,0xe7,3,0,0};result=DONGLE_BUSY;
    assert(dongle_features_request(1,route_busy,10,out,64,&n)==2 && !n && routing_writes==2);
    assert(routing_reads==3);
    uint8_t reserved[]={22};
    assert(dongle_features_request(0,reserved,1,out,64,&n)==7 && !policy_queries);
    uint8_t policy[3]={23,1,0};result=DONGLE_ACCEPTED;
    assert(dongle_features_request(0,policy,3,out,64,&n)==1 && !policy_changes);
    assert(dongle_features_request(0,policy,2,out,1,&n)==1 && !policy_changes);
    policy[1]=2;assert(dongle_features_request(0,policy,2,out,64,&n)==1 && !policy_changes);
    policy[1]=1;
    assert(dongle_features_request(0,policy,1,out,64,&n)==0 && n==2 && out[0]==1 && !out[1]);
    assert(dongle_features_request(1,policy,2,policy,2,&n)==0 && n==2 && policy[0]==1 && policy[1]==1);
    policy[0]=23;policy[1]=0;
    assert(dongle_features_request(0,policy,2,out,64,&n)==0 && n==2 && !out[1]);
    assert(policy_changes==2 && policy_queries==1);
    const uint8_t policy_failures[]={DONGLE_BUSY,DONGLE_UNSUPPORTED,DONGLE_TRANSPORT};
    const uint8_t policy_errors[]={2,7,6};
    for(unsigned i=0;i<sizeof(policy_failures);++i) {
        result=policy_failures[i];assert(dongle_features_request(0,policy,2,out,64,&n)==policy_errors[i] && !n);
    }
    puts("typed features: validation, exact integer values, aliased buffers, bounded responses and result mapping passed");
}
