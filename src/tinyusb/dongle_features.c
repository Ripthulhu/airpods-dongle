// SPDX-License-Identifier: GPL-3.0-only
#include "dongle_features.h"
#include "dongle_protocol.h"
#include "../btstack/btstack_aacp.h"
#include "../btstack/btstack_avdtp_source.h"
#include "../btstack/dongle_finder.h"
#include "../btstack/btstack_ble_scan.h"

static uint8_t error(uint8_t result) {
    switch(result){
        case DONGLE_ACCEPTED:return 0;
        case DONGLE_BUSY:return 2;
        case DONGLE_STALE:return 3;
        case DONGLE_DISCONNECTED:return 4;
        case DONGLE_UNSUPPORTED:return 7;
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
        default:return 7;
    }
}
void dongle_features_disconnect(uint8_t client) { dongle_finder_disconnect(client); }
void dongle_features_poll(void) { dongle_playback_poll();dongle_finder_poll(); }
