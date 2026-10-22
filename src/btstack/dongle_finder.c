// SPDX-License-Identifier: GPL-3.0-only
#include "btstack.h"
#include "dongle_finder.h"
#include "btstack_ble_scan.h"
#include "btstack_aacp.h"
#include "btstack_hci.h"
#include "../tinyusb/dongle_protocol.h"
#include "../tinyusb/dongle_client.h"
#include <limits.h>

#define LEASE_MS 30000u
#define RECENT_MS 90000u
static bool active,heard;
static uint8_t owner,pair[6],irk[16];
static uint32_t session,lease,heard_at,matches;
static int16_t readings[7];
static int32_t smoothed,history[5];
static unsigned reading_count,reading_next,history_count,history_next;
typedef struct { uint8_t address[6];uint32_t count,at;bool valid; } seen_t;
static seen_t seen[BLE_SCAN_CACHE_SIZE];

static void clear_readings(void) {
    reading_count=reading_next=history_count=history_next=0;
    smoothed=INT32_MIN;
}
static void clear(void) {
    active=heard=false;matches=0;clear_readings();memset(seen,0,sizeof(seen));
    volatile uint8_t *key=irk;for(unsigned i=0;i<sizeof(irk);i++)key[i]=0;
    memset(pair,0,sizeof(pair));
}
static bool selected_key(uint8_t address[6],uint8_t key[16]) {
    const uint8_t *selected=(const uint8_t *)get_device_addr();
    return selected && aacp_pair_irk(address,key) && !memcmp(selected,address,6);
}
bool dongle_finder_active(void) { return active; }
uint8_t dongle_finder_start(uint8_t client) {
    if(client>=DONGLE_CLIENT_COUNT)return DONGLE_MALFORMED;
    if(active && owner!=client)return DONGLE_BUSY;
    uint8_t address[6],key[16];
    if(!selected_key(address,key))return DONGLE_UNSUPPORTED;
    clear();memcpy(pair,address,6);memcpy(irk,key,16);memset(key,0,sizeof(key));
    owner=client;active=true;lease=btstack_run_loop_get_time_ms();
    if(!++session)++session;
    return DONGLE_ACCEPTED;
}
uint8_t dongle_finder_stop(uint8_t client) {
    if(client>=DONGLE_CLIENT_COUNT)return DONGLE_MALFORMED;
    if(active && owner!=client)return DONGLE_BUSY;
    clear();return DONGLE_ACCEPTED;
}
void dongle_finder_disconnect(uint8_t client) { if(active && owner==client)clear(); }
static bool proximity(const ble_scan_report_t *r) {
    for(unsigned at=0;at<r->data_length;){
        unsigned n=r->data[at++];
        if(!n || n>r->data_length-at)return false;
        if(n>=3 && r->data[at]==0xff && r->data[at+1]==0x4c && r->data[at+2]==0){
            for(unsigned p=at+3;p<at+n;){
                if(at+n-p<2)return false;
                unsigned size=r->data[p+1];
                if(size>at+n-p-2)return false;
                if(r->data[p]==7 && size==25 && r->data[p+2]==1)return true;
                p+=size+2;
            }
        }
        at+=n;
    }
    return false;
}
static bool resolves(const ble_scan_report_t *r) {
    if((r->address_type!=1 && r->address_type!=3) || (r->address[0]&0xc0)!=0x40)return false;
    uint8_t key[16],input[16]={0},output[16];
    for(unsigned i=0;i<16;i++)key[i]=irk[15-i];
    memcpy(input+13,r->address,3);btstack_aes128_calc(key,input,output);
    return !memcmp(output+13,r->address+3,3);
}
static void observe(const ble_scan_report_t *r,unsigned slot,void *context) {
    uint32_t now=*(uint32_t *)context;
    if(slot>=BLE_SCAN_CACHE_SIZE || r->data_length>BLE_SCAN_DATA_MAX)return;
    seen_t *s=&seen[slot];
    if(s->valid && s->count==r->reports && s->at==r->received_ms && !memcmp(s->address,r->address,6))return;
    s->valid=true;s->count=r->reports;s->at=r->received_ms;memcpy(s->address,r->address,6);
    if(!r->rssi_valid || r->rssi < -126 || r->rssi > 20 ||
        (uint32_t)(now-r->received_ms)>RECENT_MS || !proximity(r) || !resolves(r))return;
    if(heard && (uint32_t)(r->received_ms-heard_at)>=UINT32_C(0x80000000))return;
    if(!heard || (uint32_t)(r->received_ms-heard_at)>10000)clear_readings();
    readings[reading_next]=r->rssi;reading_next=(reading_next+1)%7;
    if(reading_count<7)++reading_count;
    int16_t sorted[7];memcpy(sorted,readings,reading_count*sizeof(*sorted));
    for(unsigned i=1;i<reading_count;i++){
        int16_t value=sorted[i];unsigned j=i;
        while(j && sorted[j-1]>value){sorted[j]=sorted[j-1];--j;}sorted[j]=value;
    }
    int32_t median=(int32_t)sorted[reading_count/2]*256;
    smoothed=smoothed==INT32_MIN?median:(smoothed*7+median*3)/10;
    history[history_next]=smoothed;history_next=(history_next+1)%5;
    if(history_count<5)++history_count;
    heard=true;heard_at=r->received_ms;++matches;
}
void dongle_finder_poll(void) {
    if(!active)return;
    uint32_t now=btstack_run_loop_get_time_ms();
    const uint8_t *selected=(const uint8_t *)get_device_addr();
    if(!selected || memcmp(selected,pair,6) || (uint32_t)(now-lease)>=LEASE_MS){clear();return;}
    uint8_t address[6],key[16];
    bool current=selected_key(address,key) && !memcmp(key,irk,16);
    memset(key,0,sizeof(key));
    if(!current){clear();return;}
    ble_scan_stats_t stats;ble_scan_get_stats(&stats);
    if(stats.pause_reasons || !stats.scan_requested)return;
    ble_scan_visit(observe,&now);
}
uint16_t dongle_finder_snapshot(uint8_t client,uint8_t *out,uint16_t cap) {
    if(!out || cap<24 || client>=DONGLE_CLIENT_COUNT || (active && owner!=client))return 0;
    uint32_t now=btstack_run_loop_get_time_ms();
    if(active)lease=now;
    uint8_t address[6],key[16];bool identity=selected_key(address,key);memset(key,0,sizeof(key));
    ble_scan_stats_t stats;ble_scan_get_stats(&stats);
    memset(out,0,24);out[0]=1;
    uint32_t age=heard?now-heard_at:UINT32_MAX;
    out[1]=!active?0:stats.pause_reasons?4:!heard?1:age>RECENT_MS?3:2;
    if(identity)out[1]|=0x80;
    out[2]=heard?(uint8_t)readings[(reading_next+6)%7]:127;
    if(active && !stats.pause_reasons && age<=10000 && history_count==5){
        int32_t delta=smoothed-history[history_next];
        out[3]=delta>=768?1:delta<=-768?3:2;
    }
    dongle_put32(out+4,age);dongle_put32(out+8,matches);dongle_put32(out+12,stats.pause_reasons);
    dongle_put32(out+16,session);dongle_put32(out+20,(uint32_t)smoothed);
    return 24;
}
