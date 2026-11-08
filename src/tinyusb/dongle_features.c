// SPDX-License-Identifier: GPL-3.0-only
#include "dongle_features.h"
#include "dongle_protocol.h"
#include "dongle_client.h"
#include "../btstack/btstack_aacp.h"
#include "../btstack/btstack_avdtp_source.h"
#include "../btstack/btstack_hci.h"
#include "../btstack/dongle_finder.h"
#include "../btstack/btstack_ble_scan.h"
#include "../btstack/phone_sink.h"

static uint16_t radio_snapshot(uint8_t *out,uint16_t capacity) {
    if(capacity<128)return 0;
    bt_hci_radio_diagnostics_t radio={0};
    bool connected=bt_hci_radio_diagnostics(&radio);
    memset(out,0,128);out[0]=1;out[1]=128;
    out[2]=(connected?1u:0u)|(radio.active?2u:0u)|(radio.pending?4u:0u);
    out[3]=radio.valid;dongle_put32(out+4,btstack_run_loop_get_time_ms());
    dongle_put32(out+8,radio.epoch);
    out[12]=(uint8_t)radio.handle;out[13]=(uint8_t)(radio.handle>>8);
    out[14]=radio.failed;out[15]=radio.unavailable;
    out[16]=(uint8_t)radio.rssi;out[17]=radio.quality;
    out[18]=(uint8_t)radio.tx_power;out[19]=(uint8_t)radio.max_tx_power;
    out[20]=radio.afh_mode;memcpy(out+21,radio.afh_map,10);
    for(unsigned i=0;i<5;++i)dongle_put32(out+32+4*i,radio.age_ms[i]);
    dongle_put32(out+52,radio.flush_events);dongle_put32(out+56,radio.completed_packets);
    return dongle_playback_adaptive_snapshot(out+64,64)==64?128:0;
}

static uint8_t error(uint8_t result) {
    switch(result){
        case DONGLE_ACCEPTED:return 0;
        case DONGLE_BUSY:return 2;
        case DONGLE_STALE:return 3;
        case DONGLE_DISCONNECTED:return 4;
        case DONGLE_UNSUPPORTED:return 7;
        case DONGLE_TRANSPORT:return 6;
        default:return 1;
    }
}
uint8_t dongle_features_request(uint8_t client,const uint8_t *p,uint16_t size,
    uint8_t *out,uint16_t capacity,uint16_t *length) {
    if(!p || !size || !out || !length)return 1;
    *length=0;
    // The request and response share bounded storage; consume request arguments first.
    uint8_t op=p[0],result;
    switch(op){
        case 1:
            if(size!=6)return 1;
            return error(aacp_setting_write(p[1],dongle_u32(p+2)));
        case 2:
            if(size!=1)return 1;
            *length=aacp_settings_snapshot(out,capacity);return *length?0:7;
        case 3:
            if(size!=7 || capacity<20)return 1;
            result=dongle_playback_configure(p[1],p[2],dongle_u32(p+3));
            if(result!=DONGLE_ACCEPTED)return error(result);
            *length=dongle_playback_status(out,capacity);return *length?0:7;
        case 4:
            if(size!=2 || p[1]>1 || capacity<20)return 1;
            result=dongle_playback_game_mode(p[1]!=0);
            if(result!=DONGLE_ACCEPTED)return error(result);
            *length=dongle_playback_status(out,capacity);return *length?0:7;
        case 5:
            if(size!=1)return 1;
            *length=dongle_playback_diagnostics(out,capacity);return *length?0:7;
        case 6:
            if(size!=1 || capacity<24)return 1;
            result=dongle_finder_start(client);
            if(result!=DONGLE_ACCEPTED)return error(result);
            *length=dongle_finder_snapshot(client,out,capacity);return *length?0:2;
        case 7:
            if(size!=1)return 1;
            *length=dongle_finder_snapshot(client,out,capacity);return *length?0:2;
        case 8:
            if(size!=1)return 1;
            return error(dongle_finder_stop(client));
        case 9:
            if(size!=1)return 1;
            *length=dongle_playback_status(out,capacity);return *length?0:7;
        case 11:
            if(size!=1)return 1;
            *length=dongle_playback_trace(out,capacity);return *length?0:7;
        case 12:
            if(size!=2 || p[1]>1)return 1;
            ble_scan_diagnostic_pause(p[1]==0);return 0;
        case 13:
            if(size!=1)return 1;
            *length=dongle_playback_inspect(out,capacity);return *length?0:7;
        case 14:
            if(size!=2 || p[1]>1)return 1;
            dongle_playback_inspect_enable(p[1]!=0);return 0;
        case 15:
            if(size!=1)return 1;
            *length=phone_sink_diagnostics(out,capacity);return *length?0:7;
        case 16:
            if(size!=1 || capacity<24)return 1;
            *length=dongle_volume_snapshot(client,out,capacity);return *length?0:7;
        case 17:
            if(size!=7 || capacity<24 || p[1]>3 || p[6]>127 || (p[1]!=3 && p[6]) ||
                (!p[1] && dongle_u32(p+2)) || (p[1] && !dongle_u32(p+2))) return 1;
            if(client!=DONGLE_CLIENT_USB)return 7;
            return error(dongle_volume_request(client,p,size,out,capacity,length));
        case 18:
            if(size!=2 || p[1]>1)return 1;
            if(client!=DONGLE_CLIENT_USB)return 7;
            return bt_hci_airpods_flush_comparison(p[1]!=0)?0:2;
        case 19:
            if(size!=1)return 1;
            *length=radio_snapshot(out,capacity);return *length?0:7;
        case 20:
            if(size!=1)return 1;
            *length=dongle_routing_snapshot(out,capacity);return *length?0:7;
        case 21:
            if(size!=10 || capacity<32)return 1;
            result=dongle_routing_request(p[1],dongle_u32(p+2),dongle_u32(p+6));
            if(result!=DONGLE_ACCEPTED)return error(result);
            *length=dongle_routing_snapshot(out,capacity);return *length?0:7;
        case 23: {
            if ((size!=1 && size!=2) || capacity<2 || (size==2 && p[1]>1)) return 1;
            bool automatic=false;
            result=dongle_reconnect_policy(size==2,size==2 && p[1]!=0,&automatic);
            if (result!=DONGLE_ACCEPTED) return error(result);
            out[0]=1;out[1]=automatic?1:0;*length=2;return 0;
        }
        default:return 7;
    }
}
void dongle_features_disconnect(uint8_t client) {
    dongle_finder_disconnect(client);dongle_volume_client_disconnect(client);
    if(client==DONGLE_CLIENT_USB)(void)bt_hci_airpods_flush_comparison(false);
}
void dongle_features_poll(void) { dongle_playback_poll();dongle_finder_poll(); }
