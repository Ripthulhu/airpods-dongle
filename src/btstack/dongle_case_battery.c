// SPDX-License-Identifier: GPL-3.0-only
// Layout evidence: stuffz/airpods 8bd035451668388860e62fcc0a4a624a608d9d3a,
// docs/ble-research.md. This implementation uses the pinned SDK's AES decoder.
#include "btstack.h"
#include "rijndael.h"
#include "btstack_aacp.h"
#include "btstack_ble_scan.h"
#include "btstack_hci.h"
#include "dongle_case_battery.h"

#define CASE_RECENT_MS 90000u

static dongle_case_battery_snapshot_t saved;
static uint8_t selected_pair[6], selected_key[16], last_block[16];
static uint32_t seen_scan;
static uint64_t seen_at;
static bool identity_known, heard, seen_packet;

static void erase(void *storage, unsigned length) {
    volatile uint8_t *p=storage;
    while (length--) *p++=0;
}
static void clear(void) {
    memset(&saved,0,sizeof(saved));
    memset(saved.battery,255,sizeof(saved.battery));
    memset(selected_pair,0,sizeof(selected_pair));
    erase(selected_key,sizeof(selected_key));
    erase(last_block,sizeof(last_block));
    identity_known=heard=seen_packet=false;
    seen_scan=seen_at=0;
}
static const uint8_t *case_record(const ble_scan_report_t *r) {
    // This validates every AD and Apple record, including anything after type07.
    if (!ble_scan_report_has_type(r,7)) return NULL;
    const uint8_t *found=NULL;
    for (unsigned at=0;at<r->data_length;) {
        unsigned length=r->data[at++];
        if (!length) break;
        unsigned end=at+length;
        if (r->data[at]==0xff && r->data[at+1]==0x4c && r->data[at+2]==0) {
            for (unsigned p=at+3;p<end;) {
                unsigned bytes=r->data[p+1];
                if (r->data[p]==7) {
                    if (found || bytes!=17 || r->data[p+2]!=6) return NULL;
                    found=r->data+p+3;
                }
                p+=bytes+2;
            }
        }
        at=end;
    }
    return found;
}

static void observe(const ble_scan_report_t *r, unsigned slot, void *context) {
    (void)slot;
    uint64_t now=*(uint64_t *)context;
    if ((r->address_type!=1 && r->address_type!=3) ||
        now-r->received_ms>CASE_RECENT_MS) return;
    uint64_t received=r->received_ms;
    if (seen_packet && received<=seen_at) return;
    const uint8_t *block=case_record(r);
    if (!block) return;
    uint32_t schedule[44];
    uint8_t plain[16];
    int rounds=rijndaelSetupDecrypt(schedule,selected_key,128);
    rijndaelDecrypt(schedule,rounds,block,plain);
    erase(schedule,sizeof(schedule));
    if (plain[0]!=0x29 || plain[1]!=0x20 || (plain[2]&0xf0) ||
        plain[8] || plain[10] || plain[11]) return;
    for (unsigned i=3;i<=5;++i)
        if (plain[i]!=255 && (plain[i]&127)>100) return;
    seen_packet=true;
    seen_at=received;
    // Repeated content cannot make old battery values appear freshly measured.
    // Changed counters, including resets/decreases, remain opaque and acceptable.
    if (heard && !memcmp(last_block,block,16)) return;
    memcpy(last_block,block,16);
    heard=true;
    saved.report_at=received;
    saved.present=saved.charging=0;
    static const uint8_t offsets[]={4,5,3};
    for (unsigned i=0;i<3;++i) {
        uint8_t raw=plain[offsets[i]], bit=(uint8_t)(1u<<i);
        if (raw==255) continue;
        saved.battery[i]=raw&127;
        saved.known|=bit;
        saved.present|=bit;
        if (raw&128) saved.charging|=bit;
        saved.updated[i]=received;
    }
    saved.lid=(plain[2]&8)!=0;
    saved.lid_counter=plain[2]&7;
    saved.variant=(uint16_t)((uint16_t)plain[6]<<8|plain[7]);
    saved.counter=dongle_u32(plain+12);
}

void dongle_case_battery_poll(void) {
    uint64_t now=dongle_uptime_ms();
    uint8_t pair[6], key[16];
    const uint8_t *selected=(const uint8_t *)get_device_addr();
    bool current=selected && aacp_pair_case_key(pair,key) && !memcmp(pair,selected,6);
    if (!current) { clear(); erase(key,sizeof(key)); return; }
    bool changed=!identity_known || memcmp(pair,selected_pair,6) || memcmp(key,selected_key,16);
    if (changed) {
        clear();
        memcpy(selected_pair,pair,6); memcpy(selected_key,key,16);
        identity_known=true;
        // A newly selected identity cannot inherit an earlier cached packet.
        seen_packet=true; seen_at=now;
    }
    erase(key,sizeof(key));
    ble_scan_stats_t stats;
    ble_scan_get_stats(&stats);
    if (!stats.scan_requested || stats.pause_reasons ||
        (!changed && stats.accepted==seen_scan)) return;
    seen_scan=stats.accepted;
    ble_scan_visit(observe,&now);
}

bool dongle_case_battery_snapshot(dongle_case_battery_snapshot_t *out) {
    if (!out || !identity_known || !heard) return false;
    *out=saved;
    uint64_t now=dongle_uptime_ms();
    out->live=0;
    for (unsigned i=0;i<3;++i)
        if ((out->present&(1u<<i)) && now-out->updated[i]<CASE_RECENT_MS)
            out->live|=1u<<i;
    out->charging&=out->live;
    return true;
}

void dongle_case_battery_merge(dongle_readback_t *out, bool aacp_healthy,
                              uint8_t *present, uint8_t *case_source) {
    *present=*case_source=0;
    uint64_t now=dongle_uptime_ms();
    dongle_case_battery_snapshot_t report;
    bool available=dongle_case_battery_snapshot(&report);
    for (unsigned i=0;i<3;++i) {
        uint8_t bit=(uint8_t)(1u<<i);
        if (out->known[i] && out->battery[i]<=100) *present|=bit;
        if (!available || (aacp_healthy && out->known[i] && out->battery[i]<=100) ||
            (out->known[i] && report.report_at<out->updated[i])) continue;
        bool has_value=(report.known&bit)!=0;
        if (has_value && (!out->known[i] || report.updated[i]>=out->updated[i])) {
            out->battery[i]=report.battery[i]; out->updated[i]=report.updated[i]; out->known[i]=true;
        }
        *present=(uint8_t)((*present&~bit)|(report.present&bit));
        *case_source|=bit;
        out->charging=(uint8_t)((out->charging&~bit)|(report.charging&bit));
        if (!(report.present&bit) || !out->known[i] ||
            now-out->updated[i]>=CASE_RECENT_MS) out->charging&=(uint8_t)~bit;
    }
}
