// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/btstack_ble_scan.c"
#include "../src/btstack/dongle_case_battery.c"

static uint64_t now;
static uint8_t peer[6]={1,2,3,4,5,6}, key_fixture[16];
static bool supported;
static unsigned decrypts;

uint32_t btstack_run_loop_get_time_ms(void) { return (uint32_t)now; }
bd_addr_t *get_device_addr(void) { return (bd_addr_t *)&peer; }
bool aacp_pair_case_key(uint8_t address[6],uint8_t key[16]) {
    if (!supported) return false;
    memcpy(address,peer,6); memcpy(key,key_fixture,16); return true;
}
void hci_add_event_handler(btstack_packet_callback_registration_t *r) { (void)r; }
bool hci_le_supported(void) { return true; }
void gap_set_scan_params(uint8_t t,uint16_t i,uint16_t w,uint8_t p) { (void)t;(void)i;(void)w;(void)p; }
void gap_set_scan_duplicate_filter(bool enabled) { assert(!enabled); }
void gap_start_scan(void) {}
void gap_stop_scan(void) {}

#ifndef CASE_REAL_AES
int rijndaelSetupEncrypt(uint32_t *schedule,const uint8_t *key,int bits) {
    assert(bits==128); memcpy(schedule,key,16); return 10;
}
int rijndaelSetupDecrypt(uint32_t *schedule,const uint8_t *key,int bits) {
    return rijndaelSetupEncrypt(schedule,key,bits);
}
void rijndaelEncrypt(const uint32_t *schedule,int rounds,const uint8_t *input,uint8_t *output) {
    assert(rounds==10);
    for (unsigned i=0;i<16;++i) output[i]=input[i]^((const uint8_t *)schedule)[i];
}
void rijndaelDecrypt(const uint32_t *schedule,int rounds,const uint8_t *input,uint8_t *output) {
    ++decrypts; rijndaelEncrypt(schedule,rounds,input,output);
}
#endif

// Shared public synthetic vector; no captured identifiers or device keys.
static const uint8_t original[]={0x29,0x20,0x0a,0x3d,0xde,0xdd,0x51,0x0b,0,0,0,0,0xf7,0x82,0x16,0};
static void reset(void) {
    clear(); memset(&scan_state,0,sizeof(scan_state)); scan_initialized=false; uptime_ms=0;
    now=100; supported=true; peer[0]=1; decrypts=0;
    for (unsigned i=0;i<16;++i) key_fixture[i]=(uint8_t)i;
    ble_scan_init(); ble_scan_controller_ready(true);
    ble_scan_set_paused(BLE_SCAN_PAUSE_USB_SUSPEND,false);
    dongle_case_battery_poll();
}
static void advertisement(uint8_t out[31],const uint8_t plain[16]) {
    const uint8_t prefix[]={22,0xff,0x4c,0,7,17,6};
    memset(out,0,31); memcpy(out,prefix,sizeof(prefix));
    uint32_t schedule[44];
    int rounds=rijndaelSetupEncrypt(schedule,key_fixture,128);
    rijndaelEncrypt(schedule,rounds,plain,out+7);
}
static void receive(const uint8_t *data,unsigned length,uint8_t address,uint8_t kind) {
    uint8_t packet[64]={GAP_EVENT_ADVERTISING_REPORT};
    assert(length<=31);
    packet[1]=(uint8_t)(length+10); packet[2]=0; packet[3]=kind;
    packet[4]=address; packet[9]=0xc0; packet[10]=(uint8_t)-42; packet[11]=(uint8_t)length;
    memcpy(packet+12,data,length);
    scan_packet_handler(HCI_EVENT_PACKET,0,packet,(uint16_t)(length+12));
    dongle_case_battery_poll();
}
static void send(const uint8_t plain[16],uint8_t address) {
    uint8_t data[31]; advertisement(data,plain); ++now; receive(data,23,address,1);
}
static void layouts(void) {
    reset();
    uint8_t data[31]; advertisement(data,original);
#ifdef CASE_REAL_AES
    const uint8_t expected[]={0xae,0xa2,0x48,0x93,0x3e,0x21,0x59,0x87,0x2f,0x2c,0x5a,0xe5,0xc8,0xbf,0x0f,0x31};
    assert(!memcmp(data+7,expected,16));
#endif
    ++now; receive(data,23,1,1);
    dongle_case_battery_snapshot_t report;
    assert(dongle_case_battery_snapshot(&report));
    assert(report.battery[0]==94 && report.battery[1]==93 && report.battery[2]==61);
    assert(report.present==7 && report.known==7 && report.live==7 && report.charging==3);
    assert(report.lid==1 && report.lid_counter==2 && report.variant==0x510b);
    assert(report.counter==0x001682f7 && report.report_at==now);
    uint8_t changed[16]; memcpy(changed,original,16);
    changed[2]=3; changed[6]=0x5a; changed[7]=0x86; dongle_put32(changed+12,1);
    send(changed,2); assert(dongle_case_battery_snapshot(&report));
    assert(!report.lid && report.variant==0x5a86 && report.counter==1);
    changed[6]=0x77; changed[7]=0xfe; dongle_put32(changed+12,0);
    send(changed,2); assert(dongle_case_battery_snapshot(&report));
    assert(report.variant==0x77fe && report.counter==0);
    changed[3]=0; changed[4]=0xe4; changed[5]=0x80;
    send(changed,2); assert(dongle_case_battery_snapshot(&report));
    assert(report.battery[0]==100 && report.battery[1]==0 && report.battery[2]==0 && report.charging==3);
}
static void malformed(void) {
    uint8_t data[31], changed[16]; dongle_case_battery_snapshot_t report;
    for (unsigned length=0;length<23;++length) {
        reset(); advertisement(data,original); ++now; receive(data,length,1,1);
        assert(!dongle_case_battery_snapshot(&report));
    }
    const unsigned fields[]={0,1,2,8,10,11};
    for (unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i) {
        reset(); memcpy(changed,original,16); changed[fields[i]]^=fields[i]==2?0x10:1;
        send(changed,1); assert(!dongle_case_battery_snapshot(&report));
    }
    for (unsigned slot=3;slot<=5;++slot) for (unsigned value=101;value<=127;++value) {
        reset(); memcpy(changed,original,16); changed[slot]=(uint8_t)value;
        send(changed,1); assert(!dongle_case_battery_snapshot(&report));
        changed[slot]=(uint8_t)(value|128); if (changed[slot]==255) continue;
        send(changed,1); assert(!dongle_case_battery_snapshot(&report));
    }
    for (unsigned field=0;field<7;++field) {
        reset(); advertisement(data,original);
        if (field==0) data[4]=0x12;
        if (field==1) data[5]=18;
        if (field==2) data[6]=7;
        if (field==3) { data[0]=24; data[23]=7; data[24]=0; }
        if (field==4) { data[23]=2; data[24]=0xff; }
        if (field==5) key_fixture[0]^=0x80;
        ++now; receive(data,field==3 || field==4?25:23,1,field==6?0:1);
        assert(!dongle_case_battery_snapshot(&report));
    }
}
static void freshness_and_ownership(void) {
    reset(); send(original,1);
    dongle_case_battery_snapshot_t report;
    assert(dongle_case_battery_snapshot(&report)); uint64_t measured=report.report_at;
    now+=20000; send(original,2); // New address and reception, identical block.
    assert(dongle_case_battery_snapshot(&report) && report.report_at==measured);
    uint64_t before=report.updated[1];
    uint8_t changed[16]; memcpy(changed,original,16); changed[5]=255;
    send(changed,2); assert(dongle_case_battery_snapshot(&report));
    assert(report.battery[1]==93 && report.updated[1]==before && report.present==5 && report.charging==1);
    now+=CASE_RECENT_MS; dongle_case_battery_snapshot(&report);
    assert(!report.live && !report.charging);
    key_fixture[0]++; dongle_case_battery_poll(); assert(!dongle_case_battery_snapshot(&report));
    send(original,3); assert(dongle_case_battery_snapshot(&report));
    peer[0]++; dongle_case_battery_poll(); assert(!dongle_case_battery_snapshot(&report));
    send(original,4); assert(dongle_case_battery_snapshot(&report));
    supported=false; dongle_case_battery_poll(); assert(!dongle_case_battery_snapshot(&report));
    for (unsigned i=0;i<16;++i) assert(selected_key[i]==0);
    supported=true; dongle_case_battery_poll(); assert(!dongle_case_battery_snapshot(&report));
    scan_state.stats.pause_reasons=BLE_SCAN_PAUSE_MIC;
    send(original,4); assert(!dongle_case_battery_snapshot(&report));
    scan_state.stats.pause_reasons=0; send(original,4); assert(dongle_case_battery_snapshot(&report));
    reset(); clear(); now=UINT32_MAX-10u; dongle_case_battery_poll(); send(original,1);
    now=(UINT64_C(1)<<32)+20; dongle_case_battery_snapshot(&report); assert(report.live==7);
}
static void merge_priority(void) {
    reset(); send(original,1);
    dongle_readback_t aap; dongle_readback_clear(&aap);
    uint8_t present,source;
    dongle_case_battery_merge(&aap,false,&present,&source);
    assert(aap.battery[0]==94 && aap.battery[1]==93 && aap.battery[2]==61 && present==7 && source==7);
    uint64_t age=aap.updated[1];
    uint8_t changed[16]; memcpy(changed,original,16); changed[5]=255; send(changed,1);
    dongle_case_battery_merge(&aap,false,&present,&source);
    assert(aap.battery[1]==93 && aap.updated[1]==age && present==5 && !(aap.charging&2));
    aap.battery[0]=80; aap.updated[0]=now-1;
    dongle_case_battery_merge(&aap,true,&present,&source);
    assert(aap.battery[0]==80 && source==0);
    aap.updated[0]=now+1;
    dongle_case_battery_merge(&aap,false,&present,&source);
    assert(aap.battery[0]==80 && !(source&1));
    now+=CASE_RECENT_MS; dongle_readback_clear(&aap);
    dongle_case_battery_merge(&aap,false,&present,&source); assert(!aap.charging);

    reset(); send(original,1); dongle_readback_clear(&aap);
    aap.known[0]=aap.known[1]=aap.known[2]=true;
    aap.battery[0]=255; aap.updated[0]=now-1; // Older AACP unavailable can be filled.
    aap.battery[1]=80; aap.updated[1]=now-1; // Healthy actual AACP level keeps priority.
    aap.battery[2]=255; aap.updated[2]=now+1; // Newer AACP absence still wins.
    dongle_case_battery_merge(&aap,true,&present,&source);
    assert(aap.battery[0]==94 && aap.updated[0]==now && (present&1) && source==1);
    assert(aap.battery[1]==80 && aap.battery[2]==255 && !(present&4));
}
static void advance(uint64_t elapsed) {
    while (elapsed) {
        uint64_t step=elapsed>UINT32_C(0x40000000)?UINT32_C(0x40000000):elapsed;
        now+=step; elapsed-=step; dongle_case_battery_poll();
    }
}
static void long_idle_ordering(void) {
    reset(); send(original,1);
    dongle_readback_t aap; dongle_readback_clear(&aap);
    aap.known[2]=true; aap.battery[2]=255; aap.updated[2]=now;
    advance((UINT64_C(1)<<31)+4096);
    uint8_t changed[16]; memcpy(changed,original,16); changed[3]=90;
    send(changed,1);
    dongle_case_battery_snapshot_t report;
    assert(dongle_case_battery_snapshot(&report) && report.report_at==now && report.battery[2]==90);
    uint8_t present,source;
    dongle_case_battery_merge(&aap,true,&present,&source);
    assert(aap.battery[2]==90 && aap.updated[2]==now && source==7);

    reset(); send(original,1);
    advance((UINT64_C(1)<<31)+4096);
    dongle_readback_clear(&aap);
    aap.known[2]=true; aap.battery[2]=255; aap.updated[2]=now;
    dongle_case_battery_merge(&aap,true,&present,&source);
    assert(aap.battery[2]==255 && !(source&4)); // An old report cannot replace newer absence.
    changed[3]=255; send(changed,1);
    dongle_case_battery_merge(&aap,true,&present,&source);
    assert(aap.battery[2]==255 && !(present&4)); // Nor can its retained old percentage.
}
static void long_idle_cache_and_lifetime(void) {
    reset(); send(original,1);
    uint8_t changed[16]; memcpy(changed,original,16); changed[3]=62; send(changed,2);
    uint64_t measured=now;
    const uint8_t offline[]={5,0xff,0x4c,0,0x12,0};
    for (unsigned address=3;address<=8;++address) receive(offline,sizeof(offline),(uint8_t)address,1);
    assert(scan_state.stats.cache_count==8);
    advance(UINT64_C(1)<<32);
    dongle_case_battery_snapshot_t report;
    assert(dongle_case_battery_snapshot(&report) && !report.live && !report.charging);
    receive(offline,sizeof(offline),3,1); // A different device revisits the unchanged full cache.
    assert(dongle_case_battery_snapshot(&report) && report.report_at==measured && report.battery[2]==62);
    dongle_readback_t aap; dongle_readback_clear(&aap); uint8_t present,source;
    dongle_case_battery_merge(&aap,false,&present,&source);
    assert(aap.battery[2]==62 && present==7 && source==7 && !aap.charging);
    assert(dongle_readback_age(&aap,2,now)==UINT32_MAX-1u);
    send(changed,2); // A newly received duplicate still cannot renew measurements.
    assert(dongle_case_battery_snapshot(&report) && report.report_at==measured && !report.live);
    changed[3]=90; send(changed,2);
    assert(dongle_case_battery_snapshot(&report) && report.live==7 && report.updated[2]==now);
    key_fixture[0]++; dongle_case_battery_poll(); assert(!dongle_case_battery_snapshot(&report));
    send(original,2); assert(dongle_case_battery_snapshot(&report) && report.report_at==now);
    peer[0]++; dongle_case_battery_poll(); assert(!dongle_case_battery_snapshot(&report));
    supported=false; advance(UINT64_C(1)<<32);
    supported=true; dongle_case_battery_poll(); assert(!dongle_case_battery_snapshot(&report));
    scan_state.stats.pause_reasons=BLE_SCAN_PAUSE_MIC; advance(UINT64_C(1)<<32);
    scan_state.stats.pause_reasons=0; send(original,2);
    assert(dongle_case_battery_snapshot(&report) && report.report_at==now && report.live==7);
}
int main(void) {
    _Static_assert(sizeof(dongle_case_battery_snapshot_t)<=48,"Keep the case snapshot small");
    layouts(); malformed(); freshness_and_ownership(); merge_priority();
    long_idle_ordering(); long_idle_cache_and_lifetime();
    puts("case BLE: bounded format, selected key, opaque counter, repeat suppression, age, absence, AACP priority and wrap passed");
}
