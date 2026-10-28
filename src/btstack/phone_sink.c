// SPDX-License-Identifier: GPL-3.0-only
#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "classic/a2dp.h"
#include "btstack_hci.h"
#include "phone_codec.h"
#include "phone_worker.h"
#include "phone_sink.h"

#ifndef ENABLE_AVDTP_ACCEPTOR_EXPLICIT_START_STREAM_CONFIRMATION
#error "Phone input requires explicit Start confirmation while AirPods prime"
#endif
#ifndef ENABLE_A2DP_EXPLICIT_CONFIG
#error "Phone codec selection requires explicit A2DP configuration"
#endif

#define PHONE_START_TIMEOUT_MS 3000u
#define PHONE_OPEN_TIMEOUT_MS 3000u
#define PHONE_LHDC_DECODE_FAILURE_LIMIT 3u
#define PHONE_CONTROL_TIMEOUT_MS 2000u
#define PHONE_CONTROL_QUEUE 4u

// 48 kHz stereo only. LHDC uses 24-bit, 5 ms lossy frames at 256–400 kbit/s.
static const uint8_t lhdc_capabilities[] = {0x3a, 5, 0, 0, 0x35, 0x4c, 0x10, 0x92, 0x11, 0, 0};
static const uint8_t aptx_capabilities[] = {0x4f, 0, 0, 0, 1, 0, 0x12};
static const uint8_t sbc_capabilities[] = {0x13, 0x17, 2, 53};
static uint8_t lhdc_configuration[sizeof(lhdc_capabilities)];
static uint8_t aptx_configuration[sizeof(aptx_capabilities)];
static uint8_t sbc_configuration[sizeof(sbc_capabilities)];
static avdtp_stream_endpoint_t *lhdc_endpoint, *aptx_endpoint, *sbc_endpoint;
static uint8_t lhdc_seid, aptx_seid, sbc_seid;
static uint8_t sink_sdp[150];
static bool initialized, lhdc_disabled, aptx_disabled;
static bool retry_codec;

static struct {
    bd_addr_t address;
    uint16_t cid, avrcp_cid;
    uint8_t seid;
    uint8_t remote_lhdc_seid, remote_aptx_seid, remote_sbc_seid;
    uint8_t remote_lhdc_configuration[sizeof(lhdc_capabilities)];
    avdtp_configuration_sbc_t remote_sbc;
    phone_codec_t codec;
    phone_codec_t requested_codec;
    bool select_codec;
    bool configured, opening, opened, started, start_pending, start_accepted, disconnecting, controls_owned;
    bool sequence_valid, volume_valid, volume_notification_pending, volume_event_supported;
    bool requested_volume_valid;
    uint16_t last_sequence;
    uint8_t last_codec_sequence;
    uint8_t decode_failures;
    uint32_t ssrc, pending_since_ms;
    uint32_t packets, malformed, sequence_gaps;
    uint32_t submit_failures, total_decode_failures, buffer_resets, decoded_frames, last_packet_ms;
    uint8_t volume, requested_volume;
    uint8_t controls[PHONE_CONTROL_QUEUE], control_head, control_count;
    uint32_t control_since_ms;
} phone;

static void notify_phone_volume(void) {
    if (phone.avrcp_cid == 0) return;
    if (!phone.volume_event_supported) {
        if (avrcp_target_support_event(phone.avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED)
                != ERROR_CODE_SUCCESS) return;
        phone.volume_event_supported = true;
    }
    if (!phone.volume_valid || !phone.volume_notification_pending) return;
    if (avrcp_target_volume_changed(phone.avrcp_cid, phone.volume) != ERROR_CODE_SUCCESS) return;
    phone.volume_notification_pending = false;
}

static void apply_phone_volume_intent(void) {
    if (!phone.requested_volume_valid || !phone.controls_owned || phone.disconnecting) return;
    if (!bridge_phone_volume(phone.requested_volume)) return;
    phone.requested_volume_valid = false;
    phone.volume_valid = phone.volume_notification_pending = false;
    uint8_t confirmed;
    if (bridge_phone_get_volume(&confirmed)) phone_sink_volume_changed(confirmed);
}

static bool event_complete(const uint8_t *packet, uint16_t size) {
    return packet != NULL && size >= 3 && (unsigned)packet[1] + 2u == size;
}

static bool raw_event_complete(const uint8_t *packet, uint16_t size) {
    if (!event_complete(packet, size) || packet[0] != HCI_EVENT_AVDTP_META) return false;
    switch (packet[2]) {
        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
        case AVDTP_SUBEVENT_STREAMING_CONNECTION_ESTABLISHED: return size == 14;
        case AVDTP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        case AVDTP_SUBEVENT_SIGNALING_SEP_DICOVERY_DONE: return size == 5;
        case AVDTP_SUBEVENT_SIGNALING_SEP_FOUND: return size == 9;
        case AVDTP_SUBEVENT_SIGNALING_ACCEPT:
        case AVDTP_SUBEVENT_SIGNALING_REJECT:
        case AVDTP_SUBEVENT_SIGNALING_GENERAL_REJECT: return size == 8;
        case AVDTP_SUBEVENT_SIGNALING_CAPABILITIES_DONE:
        case AVDTP_SUBEVENT_SIGNALING_DELAY_REPORTING_CAPABILITY:
        case AVDTP_SUBEVENT_STREAMING_CONNECTION_RELEASED: return size == 6;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY: return size == 14;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: return size == 18;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY:
            return size >= 11 && little_endian_read_16(packet, 9) == size - 11u;
        case AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
            return size >= 13 && little_endian_read_16(packet, 11) == size - 13u;
        default: return false;
    }
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void reset_phone_buffer(void) {
    ++phone.buffer_resets;
    phone_worker_reset();
}

static void audio_stop(void) {
    phone.started = phone.start_pending = phone.start_accepted = false;
    phone.opening = false;
    phone.sequence_valid = false;
    phone.decode_failures = 0;
    reset_phone_buffer();
    bridge_phone_stop();
}

static void reject_start(void) {
    if (phone.start_pending)
        a2dp_sink_start_stream_reject(phone.cid, phone.seid);
    audio_stop();
    phone.controls_owned = false;
    phone.requested_volume_valid = false;
    bt_volume_cancel_phone();
}

static void disconnect_phone(void) {
    audio_stop();
    phone.controls_owned = false;
    phone.requested_volume_valid = false;
    bt_volume_cancel_phone();
    phone.opened = phone.configured = phone.select_codec = false;
    phone.control_head = phone.control_count = 0;
    phone.requested_codec = PHONE_CODEC_NONE;
    if (phone.cid != 0 && !phone.disconnecting) {
        phone.disconnecting = true;
        a2dp_sink_disconnect(phone.cid);
    }
}

static void disable_failed_vendor_endpoints(void) {
    if (phone.cid != 0) return;
    if (lhdc_disabled && lhdc_endpoint != NULL) {
        a2dp_sink_finalize_stream_endpoint(lhdc_endpoint);
        lhdc_endpoint = NULL;
        lhdc_seid = 0;
    }
    if (aptx_disabled && aptx_endpoint != NULL) {
        a2dp_sink_finalize_stream_endpoint(aptx_endpoint);
        aptx_endpoint = NULL;
        aptx_seid = 0;
    }
}

static void vendor_codec_failed(phone_codec_t codec) {
    if (codec == PHONE_CODEC_LHDCV5) lhdc_disabled = retry_codec = true;
    if (codec == PHONE_CODEC_APTX) aptx_disabled = retry_codec = true;
}

static void configured_codec_failed(void) {
    vendor_codec_failed(phone.codec);
    reject_start();
    disconnect_phone();
}

static bool lhdc_configuration_valid(const uint8_t *configuration, unsigned length) {
    return length == sizeof(lhdc_capabilities) &&
        memcmp(configuration, lhdc_capabilities, 7) == 0 &&
        (configuration[7] == 0x92 || configuration[7] == 0xd2) &&
        configuration[8] == 0x11 && configuration[9] == 0 && configuration[10] == 0;
}

static bool parse_configuration(const uint8_t *event, uint16_t size,
                                phone_codec_t *codec, phone_sbc_config_t *sbc,
                                bool allow_unbound_remote) {
    if (!event_complete(event, size) || size < 9 ||
        little_endian_read_16(event, 3) != phone.cid || phone.cid == 0 ||
        phone.disconnecting || !bt_hci_phone_peer_allowed(phone.address) || event[8] != AVDTP_AUDIO ||
        (!allow_unbound_remote && event[6] == 0) || event[6] > 0x3e || event[7] > 1)
        return false;
    if (event[2] == A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION) {
        if (size != 18 || event[5] != sbc_seid || sbc_seid == 0 ||
            little_endian_read_16(event, 9) != 48000 || event[12] != 2 ||
            (event[11] != AVDTP_CHANNEL_MODE_STEREO &&
             event[11] != AVDTP_CHANNEL_MODE_JOINT_STEREO) ||
            event[13] != 16 || event[14] != 8 ||
            (event[15] != 1 && event[15] != 2) ||
            event[16] < 2 || event[17] > 53 || event[16] > event[17]) return false;
        *codec = PHONE_CODEC_SBC;
        *sbc = (phone_sbc_config_t){16, 8,
            event[11] == AVDTP_CHANNEL_MODE_STEREO ? 2 : 3,
            event[15] - 1u, event[16], event[17]};
        return phone_codec_available(*codec);
    }
    if (event[2] == A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION) {
        if (size == 24 && event[5] == lhdc_seid && lhdc_seid != 0 && !lhdc_disabled &&
            little_endian_read_16(event, 9) == AVDTP_CODEC_NON_A2DP &&
            little_endian_read_16(event, 11) == sizeof(lhdc_capabilities) &&
            lhdc_configuration_valid(event + 13, size - 13u)) {
            *codec = PHONE_CODEC_LHDCV5;
            return phone_codec_available(*codec);
        }
        if (size != 20 || event[5] != aptx_seid || aptx_seid == 0 || aptx_disabled ||
            little_endian_read_16(event, 9) != AVDTP_CODEC_NON_A2DP ||
            little_endian_read_16(event, 11) != sizeof(aptx_capabilities) ||
            memcmp(event + 13, aptx_capabilities, sizeof(aptx_capabilities)) != 0)
            return false;
        *codec = PHONE_CODEC_APTX;
        return phone_codec_available(*codec);
    }
    return false;
}

static uint8_t validate_configuration(const avdtp_stream_endpoint_t *endpoint,
                                      const uint8_t *event, uint16_t size) {
    phone_codec_t codec;
    phone_sbc_config_t sbc;
    if (endpoint == NULL || (endpoint != lhdc_endpoint && endpoint != aptx_endpoint && endpoint != sbc_endpoint))
        return AVDTP_ERROR_CODE_BAD_ACP_SEID;
    if (phone.started || phone.start_pending || phone.start_accepted)
        return AVDTP_ERROR_CODE_SEP_IN_USE;
    // BTstack validates an incoming proposal before binding its remote SEP.
    return parse_configuration(event, size, &codec, &sbc, true) ? ERROR_CODE_SUCCESS :
        AVDTP_ERROR_CODE_UNSUPPORTED_CONFIGURATION;
}

static void phone_packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (type != HCI_EVENT_PACKET || !event_complete(packet, size) ||
        packet[0] != HCI_EVENT_A2DP_META) return;
    if (packet[2] == A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED) {
        if (size != 14 || packet[13] != ERROR_CODE_SUCCESS) return;
        bd_addr_t address;
        a2dp_subevent_signaling_connection_established_get_bd_addr(packet, address);
        if (!bt_hci_phone_peer_allowed(address)) return; // shared source events also arrive here
        uint16_t cid = little_endian_read_16(packet, 3);
        if (cid == 0) return;
        if (phone.cid != 0) {
            if (cid != phone.cid) a2dp_sink_disconnect(cid);
            return;
        }
        if (phone.avrcp_cid != 0 && memcmp(address, phone.address, sizeof(address)) != 0) {
            a2dp_sink_disconnect(cid);
            return;
        }
        memcpy(phone.address, address, sizeof(address));
        phone.cid = cid;
        phone.disconnecting = false;
        phone.remote_lhdc_seid = phone.remote_aptx_seid = phone.remote_sbc_seid = 0;
        phone.select_codec = false;
        phone.requested_codec = PHONE_CODEC_NONE;
        return;
    }
    if (size < 5 || little_endian_read_16(packet, 3) != phone.cid || phone.cid == 0) return;
    if (phone.disconnecting && packet[2] != A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED) return;
    switch (packet[2]) {
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY:
            if (size == 22 && packet[6] == AVDTP_AUDIO && lhdc_seid != 0 && !lhdc_disabled &&
                little_endian_read_16(packet, 7) == AVDTP_CODEC_NON_A2DP &&
                little_endian_read_16(packet, 9) == sizeof(lhdc_capabilities) &&
                memcmp(packet + 11, lhdc_capabilities, 6) == 0 &&
                (packet[17] & 0x10u) && (packet[18] & 2u) && (packet[19] & 0x11u) == 0x11u &&
                packet[5] != 0 && packet[5] <= 0x3e) {
                phone.remote_lhdc_seid = packet[5];
                memcpy(phone.remote_lhdc_configuration, lhdc_capabilities, sizeof(lhdc_capabilities));
                // A source may require 400 kbit/s; intersect that with our 256–400 range.
                if ((packet[18] & 0xc0u) == 0xc0u) phone.remote_lhdc_configuration[7] = 0xd2;
            }
            if (size == 18 && packet[6] == AVDTP_AUDIO && aptx_seid != 0 && !aptx_disabled &&
                little_endian_read_16(packet, 7) == AVDTP_CODEC_NON_A2DP &&
                little_endian_read_16(packet, 9) == sizeof(aptx_capabilities) &&
                memcmp(packet + 11, aptx_capabilities, 6) == 0 &&
                (packet[17] & 0x12u) == 0x12u && packet[5] != 0 && packet[5] <= 0x3e)
                phone.remote_aptx_seid = packet[5];
            break;
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY:
            if (size == 14 && packet[6] == AVDTP_AUDIO && packet[5] != 0 && packet[5] <= 0x3e &&
                (packet[7] & 1u) && (packet[8] & 3u) && (packet[9] & 1u) &&
                (packet[10] & 1u) && (packet[11] & 3u) && packet[12] >= 2 && packet[12] <= 53 &&
                packet[13] <= 250 && packet[12] <= packet[13]) {
                phone.remote_sbc_seid = packet[5];
                phone.remote_sbc = (avdtp_configuration_sbc_t){0};
                phone.remote_sbc.sampling_frequency = 48000;
                phone.remote_sbc.channel_mode = packet[8] & 1u ? AVDTP_CHANNEL_MODE_JOINT_STEREO : AVDTP_CHANNEL_MODE_STEREO;
                phone.remote_sbc.block_length = AVDTP_SBC_BLOCK_LENGTH_16;
                phone.remote_sbc.subbands = AVDTP_SBC_SUBBANDS_8;
                phone.remote_sbc.allocation_method = packet[11] & 1u ?
                    AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS : AVDTP_SBC_ALLOCATION_METHOD_SNR;
                phone.remote_sbc.min_bitpool_value = packet[12] < 2 ? 2 : packet[12];
                phone.remote_sbc.max_bitpool_value = packet[13] > 53 ? 53 : packet[13];
            }
            break;
        case A2DP_SUBEVENT_SIGNALING_CAPABILITIES_COMPLETE:
            // Pinned BTstack emits six bytes for this five-byte event; the extra byte is unused.
            if ((size == 5 || size == 6) && !phone.configured && phone.requested_codec == PHONE_CODEC_NONE)
                phone.select_codec = true; // let the framework finish discovery before configuring
            break;
        case A2DP_SUBEVENT_COMMAND_REJECTED:
            if (size == 8 && packet[6] != 0 && packet[7] == AVDTP_SI_SET_CONFIGURATION &&
                ((phone.requested_codec == PHONE_CODEC_LHDCV5 && packet[5] == lhdc_seid) ||
                 (phone.requested_codec == PHONE_CODEC_APTX && packet[5] == aptx_seid))) {
                vendor_codec_failed(phone.requested_codec);
                disconnect_phone();
            }
            if (size == 8 && packet[6] != 0 && packet[7] == AVDTP_SI_OPEN &&
                phone.configured && !phone.opened && packet[5] == phone.seid)
                configured_codec_failed();
            break;
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION: {
            phone_codec_t codec;
            phone_sbc_config_t sbc;
            if (!parse_configuration(packet, size, &codec, &sbc, false)) return;
            bool already_open = packet[7] != 0 && phone.opened && phone.seid == packet[5];
            bool local_configuration = packet[7] == 0 && phone.requested_codec == codec;
            audio_stop();
            if (phone.controls_owned) phone.requested_volume_valid = false;
            phone.controls_owned = false;
            bt_volume_cancel_phone();
            if (!phone_worker_configure(codec, codec == PHONE_CODEC_SBC ? &sbc : NULL)) {
                disconnect_phone();
                return;
            }
            phone.codec = codec;
            phone.seid = packet[5];
            phone.configured = true;
            phone.opened = already_open;
            // The SDK automatically Opens our outgoing configuration; a peer may idle configured.
            phone.opening = !already_open && local_configuration;
            phone.pending_since_ms = btstack_run_loop_get_time_ms();
            phone.requested_codec = PHONE_CODEC_NONE;
            break;
        }
        case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
            if (size != 14 || !phone.configured || phone.opened || packet[11] != phone.seid) return;
            bd_addr_t address;
            a2dp_subevent_stream_established_get_bd_addr(packet, address);
            if (memcmp(address, phone.address, sizeof(address)) != 0) return;
            if (packet[13] != ERROR_CODE_SUCCESS) {
                configured_codec_failed();
                return;
            }
            if (!bt_hci_phone_peer_connected(address)) {
                disconnect_phone();
                return;
            }
            phone.opened = true;
            phone.opening = false;
            (void)bridge_phone_open();
            printf("[PHONE] open: %s stereo/48000\n", phone.codec == PHONE_CODEC_LHDCV5 ? "LHDCv5" :
                phone.codec == PHONE_CODEC_APTX ? "aptX" : "SBC");
            break;
        }
        case A2DP_SUBEVENT_START_STREAM_REQUESTED:
            if (size != 6 || packet[5] != phone.seid) return;
            if (!phone.configured || !phone.opened) {
                a2dp_sink_start_stream_reject(phone.cid, phone.seid);
                return;
            }
            if (!phone.start_pending && !phone.start_accepted && !phone.started) {
                phone.start_pending = true;
                phone.pending_since_ms = btstack_run_loop_get_time_ms();
            }
            break;
        case A2DP_SUBEVENT_STREAM_STARTED:
            if (size != 6 || packet[5] != phone.seid || !phone.start_accepted) return;
            phone.start_accepted = false;
            phone.started = true;
            phone.sequence_valid = false;
            break;
        case A2DP_SUBEVENT_STREAM_SUSPENDED:
        case A2DP_SUBEVENT_STREAM_STOPPED:
        case A2DP_SUBEVENT_STREAM_RELEASED:
            if (size != 6 || packet[5] != phone.seid) return;
            audio_stop();
            if (packet[2] != A2DP_SUBEVENT_STREAM_SUSPENDED) {
                phone.controls_owned = false;
                phone.requested_volume_valid = false;
                bt_volume_cancel_phone();
            }
            if (packet[2] == A2DP_SUBEVENT_STREAM_RELEASED) {
                phone.opened = phone.configured = false;
                phone.codec = PHONE_CODEC_NONE;
            }
            break;
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            if (size != 5) return;
            audio_stop();
            phone.controls_owned = false;
            phone.requested_volume_valid = false;
            bt_volume_cancel_phone();
            phone.control_head = phone.control_count = 0;
            phone.cid = 0;
            phone.disconnecting = false;
            phone.opened = phone.configured = false;
            phone.select_codec = false;
            phone.requested_codec = PHONE_CODEC_NONE;
            phone.codec = PHONE_CODEC_NONE;
            phone.seid = 0;
            break;
        default:
            break;
    }
}

static void phone_avdtp_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (type != HCI_EVENT_PACKET || !raw_event_complete(packet, size)) return;
    if (packet[2] == AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED) {
        if (size != 14 || packet[13] != ERROR_CODE_SUCCESS) return;
        bd_addr_t address;
        avdtp_subevent_signaling_connection_established_get_bd_addr(packet, address);
        if (!bt_hci_phone_peer_allowed(address)) return;
        uint16_t cid = little_endian_read_16(packet, 3);
        if (cid == 0) return;
        if (phone.cid != 0 && cid != phone.cid) {
            a2dp_sink_disconnect(cid);
            return;
        }
    } else if (size < 5 || phone.cid == 0 || little_endian_read_16(packet, 3) != phone.cid) {
        return;
    }
    if (!phone.disconnecting && phone.configured && !phone.opened && !phone.opening &&
        packet[2] == AVDTP_SUBEVENT_SIGNALING_ACCEPT && packet[5] == phone.seid &&
        packet[6] <= 1 && packet[7] == AVDTP_SI_OPEN) {
        phone.opening = true;
        phone.pending_since_ms = btstack_run_loop_get_time_ms();
    }
    // The shared framework otherwise discovers the AirPods in both A2DP roles.
    a2dp_config_process_avdtp_event_handler(AVDTP_ROLE_SINK, packet, size);
}

static void decoded_pcm(const int16_t *stereo, uint16_t frames, void *context) {
    (void)context;
    if (phone.started && frames != 0 && (bridge_phone_ready() || bridge_phone_recovering())) {
        phone.decode_failures = 0;
        phone.decoded_frames += frames;
        bridge_phone_pcm_push(stereo, frames);
    }
}

static void bad_packet(void) {
    phone.malformed++;
    reset_phone_buffer();
    bridge_phone_drop_pcm();
}

static void phone_media_handler(uint8_t seid, uint8_t *packet, uint16_t size) {
    if (!phone.started || seid != phone.seid || (!bridge_phone_ready() && !bridge_phone_recovering())) return;
    phone.last_packet_ms = btstack_run_loop_get_time_ms();
    // Classic aptX on Android has raw codewords, without RTP or a media header.
    if (phone.codec == PHONE_CODEC_APTX) {
        if (packet == NULL || size == 0 || size > PHONE_CODEC_MAX_PAYLOAD || size % 4u != 0) {
            bad_packet();
            return;
        }
        if (!phone_worker_submit(packet, size, 0)) {
            ++phone.submit_failures;
            // Keep older audio; the worker marks the first post-drop decode boundary.
            return;
        }
        phone.packets++;
        return;
    }
    if (packet == NULL || size < 12 || packet[0] >> 6 != 2) {
        bad_packet();
        return;
    }
    unsigned offset = 12u + (packet[0] & 15u) * 4u;
    if (offset > size) { bad_packet(); return; }
    if (packet[0] & 0x10u) {
        if (size - offset < 4u) { bad_packet(); return; }
        unsigned extension = ((unsigned)packet[offset + 2] << 8) | packet[offset + 3];
        offset += 4u;
        if (extension > (size - offset) / 4u) { bad_packet(); return; }
        offset += extension * 4u;
    }
    unsigned length = size - offset;
    if (packet[0] & 0x20u) {
        unsigned padding = packet[size - 1u];
        if (padding == 0 || padding > length) { bad_packet(); return; }
        length -= padding;
    }
    if (length == 0 || length > PHONE_CODEC_MAX_PAYLOAD + 2u) { bad_packet(); return; }
    uint8_t frames = 0;
    uint8_t codec_sequence = 0;
    if (phone.codec == PHONE_CODEC_SBC) {
        uint8_t header = packet[offset++];
        if ((header & 0xf0u) != 0 || (header & 15u) == 0) { bad_packet(); return; }
        frames = header & 15u;
        --length;
    } else if (phone.codec == PHONE_CODEC_LHDCV5) {
        // Android/Savitech send [frame_count << 2, sequence], without fragmentation.
        if (length < 2u || (packet[offset] & 3u) != 0) { bad_packet(); return; }
        frames = packet[offset] >> 2;
        if (frames == 0 || frames > 8) { bad_packet(); return; }
        codec_sequence = packet[offset + 1u];
        offset += 2u;
        length -= 2u;
    } else {
        bad_packet();
        return;
    }
    if (length == 0 || length > PHONE_CODEC_MAX_PAYLOAD) { bad_packet(); return; }
    uint16_t sequence = ((uint16_t)packet[2] << 8) | packet[3];
    uint32_t ssrc = read_be32(packet + 8);
    if (phone.sequence_valid && ssrc == phone.ssrc) {
        uint16_t advance = (uint16_t)(sequence - phone.last_sequence);
        if (advance == 0 || advance >= 0x8000u) return; // duplicate or late packet
        if (advance != 1u || (phone.codec == PHONE_CODEC_LHDCV5 &&
            codec_sequence != (uint8_t)(phone.last_codec_sequence + 1u))) {
            phone.sequence_gaps++;
            reset_phone_buffer();
            bridge_phone_drop_pcm();
        }
    } else if (phone.sequence_valid) {
        reset_phone_buffer();
        bridge_phone_drop_pcm();
    }
    phone.sequence_valid = true;
    phone.last_sequence = sequence;
    phone.ssrc = ssrc;
    phone.last_codec_sequence = codec_sequence;
    if (!phone_worker_submit(packet + offset, length, frames)) {
        ++phone.submit_failures;
        return;
    }
    phone.packets++;
}

bool phone_sink_init(void) {
    if (initialized) return sbc_endpoint != NULL;
    initialized = true;
    a2dp_sink_init();
    a2dp_sink_register_packet_handler(phone_packet_handler);
    avdtp_sink_register_packet_handler(phone_avdtp_handler);
    a2dp_sink_register_media_handler(phone_media_handler);
    a2dp_sink_register_media_config_validator(validate_configuration);
    if (phone_codec_available(PHONE_CODEC_LHDCV5)) {
        lhdc_endpoint = a2dp_sink_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
            lhdc_capabilities, sizeof(lhdc_capabilities), lhdc_configuration, sizeof(lhdc_configuration));
        if (lhdc_endpoint != NULL) lhdc_seid = avdtp_local_seid(lhdc_endpoint);
    }
    if (phone_codec_available(PHONE_CODEC_APTX)) {
        aptx_endpoint = a2dp_sink_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_NON_A2DP,
            aptx_capabilities, sizeof(aptx_capabilities), aptx_configuration, sizeof(aptx_configuration));
        if (aptx_endpoint != NULL) aptx_seid = avdtp_local_seid(aptx_endpoint);
    }
    sbc_endpoint = a2dp_sink_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC,
        sbc_capabilities, sizeof(sbc_capabilities), sbc_configuration, sizeof(sbc_configuration));
    if (sbc_endpoint == NULL) {
        if (lhdc_endpoint != NULL) a2dp_sink_finalize_stream_endpoint(lhdc_endpoint);
        if (aptx_endpoint != NULL) a2dp_sink_finalize_stream_endpoint(aptx_endpoint);
        lhdc_endpoint = NULL;
        lhdc_seid = 0;
        aptx_endpoint = NULL;
        aptx_seid = 0;
        return false;
    }
    sbc_seid = avdtp_local_seid(sbc_endpoint);
    a2dp_sink_create_sdp_record(sink_sdp, sdp_create_service_record_handle(),
        AVDTP_SINK_FEATURE_MASK_HEADPHONE, NULL, NULL);
    sdp_register_service(sink_sdp);
    return true;
}

static uint8_t send_control(uint8_t operation) {
    switch (operation) {
        case AVRCP_OPERATION_ID_PLAY: return avrcp_controller_play(phone.avrcp_cid);
        case AVRCP_OPERATION_ID_PAUSE: return avrcp_controller_pause(phone.avrcp_cid);
        case AVRCP_OPERATION_ID_STOP: return avrcp_controller_stop(phone.avrcp_cid);
        case AVRCP_OPERATION_ID_FORWARD: return avrcp_controller_forward(phone.avrcp_cid);
        case AVRCP_OPERATION_ID_BACKWARD: return avrcp_controller_backward(phone.avrcp_cid);
        case AVRCP_OPERATION_ID_VOLUME_UP: return avrcp_controller_volume_up(phone.avrcp_cid);
        case AVRCP_OPERATION_ID_VOLUME_DOWN: return avrcp_controller_volume_down(phone.avrcp_cid);
        default: return ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE;
    }
}

void phone_sink_poll(void) {
    if (!phone_worker_poll(decoded_pcm, NULL)) {
        ++phone.total_decode_failures;
        bad_packet();
        if (phone.started && phone.codec == PHONE_CODEC_LHDCV5 &&
            ++phone.decode_failures >= PHONE_LHDC_DECODE_FAILURE_LIMIT)
            configured_codec_failed();
    }
    if (phone.select_codec && phone.cid != 0 && !phone.configured) {
        phone.select_codec = false;
        uint8_t status = ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE;
        if (phone.remote_lhdc_seid != 0 && lhdc_seid != 0 && !lhdc_disabled) {
            status = a2dp_sink_set_config_other(phone.cid, lhdc_seid, phone.remote_lhdc_seid,
                phone.remote_lhdc_configuration, sizeof(phone.remote_lhdc_configuration));
            if (status == ERROR_CODE_SUCCESS) phone.requested_codec = PHONE_CODEC_LHDCV5;
        }
        if (status != ERROR_CODE_SUCCESS && phone.remote_aptx_seid != 0 && aptx_seid != 0 && !aptx_disabled) {
            status = a2dp_sink_set_config_other(phone.cid, aptx_seid, phone.remote_aptx_seid,
                                                aptx_capabilities, sizeof(aptx_capabilities));
            if (status == ERROR_CODE_SUCCESS) phone.requested_codec = PHONE_CODEC_APTX;
        }
        if (status != ERROR_CODE_SUCCESS && phone.remote_sbc_seid != 0) {
            status = a2dp_sink_set_config_sbc(phone.cid, sbc_seid, phone.remote_sbc_seid, &phone.remote_sbc);
            if (status == ERROR_CODE_SUCCESS) phone.requested_codec = PHONE_CODEC_SBC;
        }
        if (status != ERROR_CODE_SUCCESS) disconnect_phone();
    }
    if (phone.configured && !phone_worker_ready() && phone_worker_prepare() == PHONE_WORKER_FAILED) {
        printf("[PHONE] decoder unavailable; reconnect with a fallback codec\n");
        configured_codec_failed();
    }
    if (phone.opening &&
        (uint32_t)(btstack_run_loop_get_time_ms() - phone.pending_since_ms) >= PHONE_OPEN_TIMEOUT_MS)
        configured_codec_failed();
    if (!phone.configured) phone_worker_close();
    disable_failed_vendor_endpoints();
    if (retry_codec && phone.cid == 0) {
        retry_codec = false;
        uint16_t cid = 0;
        if (bt_hci_phone_peer_allowed(phone.address) &&
            a2dp_sink_establish_stream(phone.address, &cid) != ERROR_CODE_SUCCESS)
            printf("[PHONE] fallback reconnect failed\n");
    }
    if ((phone.started || phone.start_accepted) && !bridge_phone_ready() && !bridge_phone_recovering()) {
        printf("[PHONE] output lost or USB took ownership\n");
        disconnect_phone();
    }
    if (phone.start_accepted &&
        (uint32_t)(btstack_run_loop_get_time_ms() - phone.pending_since_ms) >= PHONE_START_TIMEOUT_MS) {
        disconnect_phone();
    }
    if (phone.start_pending) {
        uint32_t now = btstack_run_loop_get_time_ms();
        if ((uint32_t)(now - phone.pending_since_ms) >= PHONE_START_TIMEOUT_MS) {
            printf("[PHONE] AirPods priming timed out\n");
            reject_start();
        } else if (phone_worker_ready() && bridge_phone_open() && bridge_phone_ready()) {
            if (!bridge_phone_start()) {
                reject_start();
            } else {
                phone.start_pending = false;
                phone.start_accepted = true;
                phone.pending_since_ms = now;
                reset_phone_buffer();
                if (a2dp_sink_start_stream_accept(phone.cid, phone.seid) != ERROR_CODE_SUCCESS) {
                    audio_stop();
                    phone.controls_owned = false;
                    phone.requested_volume_valid = false;
                    bt_volume_cancel_phone();
                } else {
                    phone.controls_owned = true;
                }
            }
        }
    }
    if (phone.control_count != 0 && phone.avrcp_cid != 0) {
        uint32_t now = btstack_run_loop_get_time_ms();
        uint8_t operation = phone.controls[phone.control_head];
        if (send_control(operation) == ERROR_CODE_SUCCESS ||
            (uint32_t)(now - phone.control_since_ms) >= PHONE_CONTROL_TIMEOUT_MS) {
            phone.control_head = (phone.control_head + 1u) % PHONE_CONTROL_QUEUE;
            phone.control_count--;
            phone.control_since_ms = now;
        }
    }
    apply_phone_volume_intent();
    notify_phone_volume();
}

void phone_sink_yield(void) {
    retry_codec = false;
    if (!phone.disconnecting) disconnect_phone();
    phone_worker_close();
}

bool phone_sink_decoder_released(void) { return phone_worker_released(); }

bool phone_sink_connected(void) { return phone.cid != 0; }
bool phone_sink_active(void) { return phone.started || phone.start_accepted; }
bool phone_sink_owns_controls(void) { return phone.controls_owned; }

uint16_t phone_sink_diagnostics(uint8_t *out, uint16_t capacity) {
    if (out == NULL || capacity < 52u) return 0;
    phone_worker_queue_snapshot_t queue = phone_worker_queue_snapshot();
    out[0] = 1;
    out[1] = (uint8_t)phone.codec;
    out[2] = (phone.cid != 0 ? 1u : 0u) | (phone.configured ? 2u : 0u) |
        (phone.opened ? 4u : 0u) | (phone.started ? 8u : 0u) |
        (phone.start_pending ? 16u : 0u) | (phone.start_accepted ? 32u : 0u) |
        (phone_worker_ready() ? 64u : 0u) | (bridge_phone_ready() ? 128u : 0u);
    out[3] = 12;
    const uint32_t fields[] = {phone.packets, phone.malformed, phone.sequence_gaps,
        phone.submit_failures, phone.total_decode_failures, phone.buffer_resets,
        phone.decoded_frames, phone.last_packet_ms, queue.queued_packets,
        queue.output_ready ? 1u : 0u, phone_worker_last_decode_us(), phone_worker_max_decode_us()};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); ++i)
        for (unsigned byte = 0; byte < 4; ++byte)
            out[4u + i * 4u + byte] = (uint8_t)(fields[i] >> (byte * 8u));
    return 52;
}

bool phone_sink_handle_avrcp(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (type != HCI_EVENT_PACKET || !event_complete(packet, size) ||
        packet[0] != HCI_EVENT_AVRCP_META) return false;
    if (packet[2] == AVRCP_SUBEVENT_CONNECTION_ESTABLISHED) {
        if (size != 14) return false;
        bd_addr_t address;
        avrcp_subevent_connection_established_get_bd_addr(packet, address);
        if (!bt_hci_phone_peer_allowed(address)) return false;
        if (packet[3] != ERROR_CODE_SUCCESS) return true;
        uint16_t cid = little_endian_read_16(packet, 4);
        if (cid == 0 || (phone.cid != 0 && memcmp(address, phone.address, sizeof(address)) != 0)) return true;
        if (phone.avrcp_cid != 0 && phone.avrcp_cid != cid) return true;
        memcpy(phone.address, address, sizeof(address));
        phone.avrcp_cid = cid;
        phone_sink_volume_invalidated();
        // shortcut: BTstack can answer unsolicited early REGISTER from its default;
        // defer it if Android captures require it. That value never becomes intent.
        uint8_t confirmed;
        if (bridge_phone_get_volume(&confirmed)) phone_sink_volume_changed(confirmed);
        else notify_phone_volume();
        return true;
    }
    if (size < 5 || phone.avrcp_cid == 0 || little_endian_read_16(packet, 3) != phone.avrcp_cid)
        return false;
    if (packet[2] == AVRCP_SUBEVENT_CONNECTION_RELEASED && size == 5) {
        bt_volume_cancel_phone();
        phone.avrcp_cid = 0;
        phone.control_head = phone.control_count = 0;
        phone_sink_volume_invalidated();
    }
    return true;
}

bool phone_sink_handle_controller(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    return type == HCI_EVENT_PACKET && event_complete(packet, size) && size >= 5 &&
        packet[0] == HCI_EVENT_AVRCP_META && phone.avrcp_cid != 0 &&
        little_endian_read_16(packet, 3) == phone.avrcp_cid;
}

bool phone_sink_handle_target(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (!phone_sink_handle_controller(type, channel, packet, size)) return false;
    if (packet[2] == AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED && size == 7 && packet[6] <= 127) {
        if (phone.disconnecting) return true;
        // This target event represents an explicit phone SET_ABSOLUTE_VOLUME.
        phone.requested_volume = packet[6];
        phone.requested_volume_valid = true;
        apply_phone_volume_intent();
    }
    return true;
}

bool phone_sink_media_control(uint8_t operation) {
    if (phone.avrcp_cid == 0) return false;
    switch (operation) {
        case AVRCP_OPERATION_ID_PLAY: case AVRCP_OPERATION_ID_PAUSE:
        case AVRCP_OPERATION_ID_STOP: case AVRCP_OPERATION_ID_FORWARD:
        case AVRCP_OPERATION_ID_BACKWARD: case AVRCP_OPERATION_ID_VOLUME_UP:
        case AVRCP_OPERATION_ID_VOLUME_DOWN: break;
        default: return false;
    }
    if (phone.control_count == PHONE_CONTROL_QUEUE) return true;
    phone.controls[(phone.control_head + phone.control_count) % PHONE_CONTROL_QUEUE] = operation;
    if (phone.control_count++ == 0) phone.control_since_ms = btstack_run_loop_get_time_ms();
    return true;
}

void phone_sink_volume_changed(uint8_t volume) {
    if (volume > 127) return;
    phone.volume = volume;
    phone.volume_valid = true;
    phone.volume_notification_pending = true;
    notify_phone_volume();
}

void phone_sink_volume_invalidated(void) {
    phone.volume_valid = phone.volume_notification_pending = phone.volume_event_supported = false;
    phone.requested_volume_valid = false;
}
