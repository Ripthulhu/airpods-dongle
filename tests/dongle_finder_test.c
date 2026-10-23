// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#ifdef FINDER_REAL_AES
#include "rijndael.h"
#endif
#include "../src/btstack/dongle_finder.c"
static uint8_t selected_peer[6]={1,2,3,4,5,6};
static uint8_t key_fixture[16]={0x79,4,0x65,0x1e,0xe2,0xcc,0xd9,0x26,0xf2,0x6e,0x20,0xee,0x3e,0xcc,0xde,0x79};
static bool identity=true;
static uint64_t now=100;
static ble_scan_stats_t scan={.controller_ready=true,.le_supported=true,.scan_requested=true};
static ble_scan_report_t records[8];static unsigned record_count;
uint32_t btstack_run_loop_get_time_ms(void){return (uint32_t)now;}
uint64_t dongle_uptime_ms(void){return now;}
bd_addr_t *get_device_addr(void){return (bd_addr_t *)&selected_peer;}
bool aacp_pair_irk(uint8_t address[6],uint8_t key[16]){if(!identity)return false;memcpy(address,selected_peer,6);memcpy(key,key_fixture,16);return true;}
void ble_scan_get_stats(ble_scan_stats_t *out){*out=scan;}
void ble_scan_visit(void(*visit)(const ble_scan_report_t *,unsigned,void *),void *ctx){for(unsigned i=0;i<record_count;i++)visit(&records[i],i,ctx);}
void btstack_aes128_calc(const uint8_t *key,const uint8_t *input,uint8_t *out){
#ifdef FINDER_REAL_AES
    uint32_t schedule[44];int rounds=rijndaelSetupEncrypt(schedule,key,128);rijndaelEncrypt(schedule,rounds,input,out);
#else
    // Deterministic fixture cipher for policy tests; --real test uses pinned AES.
    memset(out,0,16);out[13]=(uint8_t)(key[0]^input[13]^0xb2);
    out[14]=(uint8_t)(key[1]^input[14]^0x19);out[15]=(uint8_t)(key[2]^input[15]^0x2a);
#endif
}
static void address_for(uint8_t out[6],uint8_t prand){
    uint8_t key[16],input[16]={0},cipher[16];
    for(unsigned i=0;i<16;i++)key[i]=key_fixture[15-i];
    input[13]=prand;input[14]=0x16;input[15]=0x2b;
    btstack_aes128_calc(key,input,cipher);memcpy(out,input+13,3);memcpy(out+3,cipher+13,3);
}
static void report(unsigned slot,uint8_t prand,int8_t rssi){
    ble_scan_report_t *r=&records[slot];memset(r,0,sizeof(*r));address_for(r->address,prand);
    r->address_type=1;r->rssi=rssi;r->rssi_valid=true;r->received_ms=now;r->reports=1;
    const uint8_t header[]={30,0xff,0x4c,0,7,25,1};memcpy(r->data,header,sizeof(header));r->data_length=31;
    if(record_count<=slot)record_count=slot+1;
}
static uint32_t field(const uint8_t *p,unsigned at){return dongle_u32(p+at);}
int main(void){
    uint8_t out[24];assert(dongle_finder_snapshot(0,out,24)==24 && out[1]==0x80 && out[2]==127);
    identity=false;assert(dongle_finder_start(0)==DONGLE_UNSUPPORTED);identity=true;
    assert(dongle_finder_start(0)==DONGLE_ACCEPTED);assert(dongle_finder_start(1)==DONGLE_BUSY);
    report(0,0x5a,-70);
#ifdef FINDER_REAL_AES
    const uint8_t capod_fixture[]={0x5a,0x16,0x2b,0x91,0xd1,0xcd};assert(!memcmp(records[0].address,capod_fixture,6));
#endif
    dongle_finder_poll();assert(dongle_finder_snapshot(0,out,24)==24 && out[1]==0x82 && field(out,8)==1 && field(out,20)==(uint32_t)(-70*256));
    report(1,0x5a,-10);records[1].address[5]^=1;dongle_finder_poll();dongle_finder_snapshot(0,out,24);assert(field(out,8)==1);
    now++;report(1,0x5b,-65);dongle_finder_poll();dongle_finder_poll();dongle_finder_snapshot(0,out,24);assert(field(out,8)==2);
    assert(!dongle_finder_snapshot(1,out,24) && dongle_finder_stop(1)==DONGLE_BUSY);
    for(unsigned i=0;i<5;i++){now+=20000;dongle_finder_snapshot(0,out,24);dongle_finder_poll();}
    dongle_finder_snapshot(0,out,24);assert((out[1]&0x7f)==3 && out[3]==0 && field(out,8)==2);
    scan.pause_reasons=BLE_SCAN_PAUSE_MIC;now++;report(0,0x5c,-30);dongle_finder_poll();dongle_finder_snapshot(0,out,24);
    assert((out[1]&0x7f)==4 && field(out,8)==2);
    scan.pause_reasons=0;dongle_finder_poll();dongle_finder_snapshot(0,out,24);assert(field(out,8)==3 && field(out,20)==(uint32_t)(-30*256));
    identity=false;dongle_finder_poll();assert(!dongle_finder_active());identity=true;
    assert(dongle_finder_start(0)==1);dongle_finder_disconnect(1);assert(dongle_finder_active());
    selected_peer[0]++;dongle_finder_poll();assert(!dongle_finder_active());
    assert(dongle_finder_start(0)==1);now+=30000;dongle_finder_poll();assert(!dongle_finder_active());
    assert(dongle_finder_start(0)==1);dongle_finder_disconnect(0);assert(!dongle_finder_active());
    assert(dongle_finder_stop(0)==1 && !dongle_finder_snapshot(0,out,23));
    now=UINT32_MAX-10;assert(dongle_finder_start(0)==1);now=(UINT64_C(1)<<32)+20;dongle_finder_poll();assert(dongle_finder_active());
    report(0,0x5d,-40);dongle_finder_poll();dongle_finder_snapshot(0,out,24);
    assert((out[1]&0x7f)==2 && field(out,4)==0);
    now+=UINT64_C(1)<<32;dongle_finder_snapshot(0,out,24);dongle_finder_poll();
    dongle_finder_snapshot(0,out,24);assert((out[1]&0x7f)==3 && field(out,4)==UINT32_MAX-1u);
    dongle_finder_stop(0);for(unsigned i=0;i<16;i++)assert(irk[i]==0);
    puts("finder: selected IRK, address rotation, two pairs, long gaps, HQ mic pause, cancellation, lease and wrap passed");
}
