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
#include "../btstack/volume_authority.h"

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

// Feature 24 reports the running firmware's version, so hosts can tell whether an update
// exists. Host test builds have no project version and report 0.0.0.
#ifndef AIRPODS_DONGLE_VERSION_MAJOR
#define AIRPODS_DONGLE_VERSION_MAJOR 0
#endif
#ifndef AIRPODS_DONGLE_VERSION_MINOR
#define AIRPODS_DONGLE_VERSION_MINOR 0
#endif
#ifndef AIRPODS_DONGLE_VERSION_PATCH
#define AIRPODS_DONGLE_VERSION_PATCH 0
#endif
#ifdef AIRPODS_EXPERIMENTAL_RADIO0076
#define AIRPODS_DONGLE_VARIANT DONGLE_FIRMWARE_VARIANT_RADIO0076
#else
#define AIRPODS_DONGLE_VARIANT DONGLE_FIRMWARE_VARIANT_STANDARD
#endif

static uint16_t firmware_info(uint8_t *out,uint16_t capacity) {
    if(capacity<DONGLE_FIRMWARE_VERSION_SIZE)return 0;
    out[DONGLE_FIRMWARE_VERSION_OFFSET_VERSION]=DONGLE_FIRMWARE_VERSION_VERSION;
    out[DONGLE_FIRMWARE_VERSION_OFFSET_MAJOR]=AIRPODS_DONGLE_VERSION_MAJOR;
    out[DONGLE_FIRMWARE_VERSION_OFFSET_MINOR]=AIRPODS_DONGLE_VERSION_MINOR;
    out[DONGLE_FIRMWARE_VERSION_OFFSET_PATCH]=(uint8_t)AIRPODS_DONGLE_VERSION_PATCH;
    out[DONGLE_FIRMWARE_VERSION_OFFSET_PATCH+1]=(uint8_t)(AIRPODS_DONGLE_VERSION_PATCH>>8);
    out[DONGLE_FIRMWARE_VERSION_OFFSET_VARIANT]=AIRPODS_DONGLE_VARIANT;
    return DONGLE_FIRMWARE_VERSION_SIZE;
}

// Maps report-2 style results to report-3 failure codes; zero means success.
static uint8_t error(uint8_t result) {
    switch(result){
        case DONGLE_ACCEPTED:return 0;
        case DONGLE_BUSY:return DONGLE_RPC_ERROR_BUSY;
        case DONGLE_STALE:return DONGLE_RPC_ERROR_STALE;
        case DONGLE_DISCONNECTED:return DONGLE_RPC_ERROR_DISCONNECTED;
        case DONGLE_UNSUPPORTED:return DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_TRANSPORT:return DONGLE_RPC_ERROR_TRANSPORT;
        default:return DONGLE_RPC_ERROR_MALFORMED;
    }
}
uint8_t dongle_features_request(uint8_t client,const uint8_t *p,uint16_t size,
    uint8_t *out,uint16_t capacity,uint16_t *length) {
    if(!p || !size || !out || !length)return DONGLE_RPC_ERROR_MALFORMED;
    *length=0;
    // The request and response share bounded storage; consume request arguments first.
    uint8_t op=p[0],result;
    switch(op){
        case DONGLE_FEATURE_WRITE_SETTING:
            if(size!=6)return DONGLE_RPC_ERROR_MALFORMED;
            return error(aacp_setting_write(p[1],dongle_u32(p+2)));
        case DONGLE_FEATURE_READ_SETTINGS_SNAPSHOT:
            if(size!=1 && (size!=2 || p[1]!=DONGLE_SETTINGS_SNAPSHOT_EXTENDED_SELECTOR))return DONGLE_RPC_ERROR_MALFORMED;
            *length=size==2 ? aacp_settings_snapshot_extended(out,capacity) : aacp_settings_snapshot(out,capacity);
            return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_CONFIGURE_AUDIO_PROFILE:
            if(size!=DONGLE_CONFIGURE_AUDIO_PROFILE_REQUEST_SIZE || capacity<DONGLE_AUDIO_PROFILE_SIZE)return DONGLE_RPC_ERROR_MALFORMED;
            result=dongle_playback_configure(p[DONGLE_CONFIGURE_AUDIO_PROFILE_OFFSET_REQUESTED_CODEC],
                p[DONGLE_CONFIGURE_AUDIO_PROFILE_OFFSET_RATE_MODE],
                dongle_u32(p+DONGLE_CONFIGURE_AUDIO_PROFILE_OFFSET_REQUESTED_BITRATE));
            if(result!=DONGLE_ACCEPTED)return error(result);
            *length=dongle_playback_status(out,capacity);return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_SET_GAME_MODE:
            if(size!=DONGLE_SET_GAME_MODE_REQUEST_SIZE || p[DONGLE_SET_GAME_MODE_OFFSET_ENABLED]>DONGLE_SET_GAME_MODE_MAXIMUM_ENABLED ||
                capacity<DONGLE_AUDIO_PROFILE_SIZE)return DONGLE_RPC_ERROR_MALFORMED;
            result=dongle_playback_game_mode(p[DONGLE_SET_GAME_MODE_OFFSET_ENABLED]!=0);
            if(result!=DONGLE_ACCEPTED)return error(result);
            *length=dongle_playback_status(out,capacity);return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_READ_PERFORMANCE:
            if(size!=1)return DONGLE_RPC_ERROR_MALFORMED;
            *length=dongle_playback_diagnostics(out,capacity);return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_START_NEARBY_FINDER:
            if(size!=1 || capacity<DONGLE_NEARBY_FINDER_SIZE)return DONGLE_RPC_ERROR_MALFORMED;
            result=dongle_finder_start(client);
            if(result!=DONGLE_ACCEPTED)return error(result);
            *length=dongle_finder_snapshot(client,out,capacity);return *length?0:DONGLE_RPC_ERROR_BUSY;
        case DONGLE_FEATURE_READ_NEARBY_FINDER:
            if(size!=1)return DONGLE_RPC_ERROR_MALFORMED;
            *length=dongle_finder_snapshot(client,out,capacity);return *length?0:DONGLE_RPC_ERROR_BUSY;
        case DONGLE_FEATURE_STOP_NEARBY_FINDER:
            if(size!=1)return DONGLE_RPC_ERROR_MALFORMED;
            return error(dongle_finder_stop(client));
        case DONGLE_FEATURE_READ_AUDIO_PROFILE:
            if(size!=1)return DONGLE_RPC_ERROR_MALFORMED;
            *length=dongle_playback_status(out,capacity);return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_READ_AUDIO_TRACE:
            if(size!=1)return DONGLE_RPC_ERROR_MALFORMED;
            *length=dongle_playback_trace(out,capacity);return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_SET_DIAGNOSTIC_BLE_SCAN:
            if(size!=2 || p[1]>1)return DONGLE_RPC_ERROR_MALFORMED;
            ble_scan_diagnostic_pause(p[1]==0);return 0;
        case DONGLE_FEATURE_READ_AAC_ELD_INSPECTION:
            if(size!=1)return DONGLE_RPC_ERROR_MALFORMED;
            *length=dongle_playback_inspect(out,capacity);return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_ENABLE_AAC_ELD_INSPECTION:
            if(size!=2 || p[1]>1)return DONGLE_RPC_ERROR_MALFORMED;
            dongle_playback_inspect_enable(p[1]!=0);return 0;
        case DONGLE_FEATURE_READ_PHONE_INPUT_COUNTERS:
            if(size!=1)return DONGLE_RPC_ERROR_MALFORMED;
            *length=phone_sink_diagnostics(out,capacity);return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_READ_PHYSICAL_VOLUME:
            if(size!=1 || capacity<DONGLE_PHYSICAL_VOLUME_SIZE)return DONGLE_RPC_ERROR_MALFORMED;
            *length=dongle_volume_snapshot(client,out,capacity);return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_USB_HOST_VOLUME_AUTHORITY:
            if(!volume_authority_request_valid(p,size) || capacity<DONGLE_PHYSICAL_VOLUME_SIZE)
                return DONGLE_RPC_ERROR_MALFORMED;
            if(client!=DONGLE_CLIENT_USB)return DONGLE_RPC_ERROR_UNSUPPORTED;
            return error(dongle_volume_request(client,p,size,out,capacity,length));
        case DONGLE_FEATURE_DIAGNOSTIC_FLUSH_COMPARISON:
            if(size!=2 || p[1]>1)return DONGLE_RPC_ERROR_MALFORMED;
            if(client!=DONGLE_CLIENT_USB)return DONGLE_RPC_ERROR_UNSUPPORTED;
            return bt_hci_airpods_flush_comparison(p[1]!=0)?0:DONGLE_RPC_ERROR_BUSY;
        case DONGLE_FEATURE_DIAGNOSTIC_A2DP_PRIORITY:
            if (client != DONGLE_CLIENT_USB) return DONGLE_RPC_ERROR_UNSUPPORTED;
            if (capacity < 9 || (size != 1 && (size != 2 || p[1] > 1))) return DONGLE_RPC_ERROR_MALFORMED;
            if (size == 2) return DONGLE_RPC_ERROR_UNSUPPORTED;
            bt_hci_priority_snapshot(out);
            *length = 9;
            return 0;
        case DONGLE_FEATURE_READ_RADIO_AND_BITRATE:
            if(size!=1)return DONGLE_RPC_ERROR_MALFORMED;
            *length=radio_snapshot(out,capacity);return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_READ_ROUTING_STATUS:
            if(size!=1)return DONGLE_RPC_ERROR_MALFORMED;
            *length=dongle_routing_snapshot(out,capacity);return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_REQUEST_ROUTING_HANDOVER:
            if(size!=10 || capacity<32)return DONGLE_RPC_ERROR_MALFORMED;
            result=dongle_routing_request(p[1],dongle_u32(p+2),dongle_u32(p+6));
            if(result!=DONGLE_ACCEPTED)return error(result);
            *length=dongle_routing_snapshot(out,capacity);return *length?0:DONGLE_RPC_ERROR_UNSUPPORTED;
        case DONGLE_FEATURE_RECONNECT_POLICY: {
            if ((size!=1 && size!=2) || capacity<2 || (size==2 && p[1]>DONGLE_RECONNECT_POLICY_AUTOMATIC)) return DONGLE_RPC_ERROR_MALFORMED;
            bool automatic=false;
            result=dongle_reconnect_policy(size==2,size==2 && p[1]!=0,&automatic);
            if (result!=DONGLE_ACCEPTED) return error(result);
            out[0]=1;out[1]=automatic?DONGLE_RECONNECT_POLICY_AUTOMATIC:DONGLE_RECONNECT_POLICY_MANUAL;*length=2;return 0;
        }
        case DONGLE_FEATURE_READ_FIRMWARE_VERSION:
            if(size!=1)return DONGLE_RPC_ERROR_MALFORMED;
            *length=firmware_info(out,capacity);return *length?0:DONGLE_RPC_ERROR_MALFORMED;
        default:return DONGLE_RPC_ERROR_UNSUPPORTED;
    }
}
void dongle_features_disconnect(uint8_t client) {
    dongle_finder_disconnect(client);dongle_volume_client_disconnect(client);
    if(client==DONGLE_CLIENT_USB) {
        (void)bt_hci_airpods_flush_comparison(false);
        bt_hci_priority_transition(PRIORITY_USB_LOST);
    }
}
void dongle_features_poll(void) { dongle_playback_poll();dongle_finder_poll(); }
