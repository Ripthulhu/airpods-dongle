// SPDX-License-Identifier: GPL-3.0-only
#include <string.h>
#include <stdio.h>
#include "btstack.h"
#include "classic/hfp_hf.h"
#include "phone_call.h"
#include "phone_call_audio.h"
#include "phone_sink.h"
#include "btstack_hci.h"
#include "btstack_avdtp_source.h"
#include "btstack_aacp.h"
#include "../tinyusb/uac.h"

#define CALL_CHANNEL 8u
#define CALL_CONNECT_MS 10000u
#define CALL_RETRY_MS 30000u
#define CALL_PREPARE_MS 8000u
#define CALL_COMMAND_MS 2000u
#define CALL_SCO_PAYLOAD_MAX 120u

static uint8_t call_sdp[150];
// Mandatory HFP narrowband codec; wideband contexts exceed the guarded RAM budget.
static const uint8_t call_codecs[] = {HFP_CODEC_CVSD};
static struct {
    bd_addr_t peer;
    uint16_t acl, sco;
    uint8_t codec, attempts, speaker_gain, microphone_gain, send_failures;
    bool initialized, connecting, retry_wait, ringing, in_call, setup;
    bool send_requested, output_open, cleanup, command_pending;
    uint32_t connecting_at, retry_at, audio_at, command_at, last_packet_at, mic_at, mic_samples;
    uint32_t received, sent, malformed, bad_frames;
    uint8_t pending[CALL_SCO_PAYLOAD_MAX];
    uint8_t pending_length;
} call = {.acl = HCI_CON_HANDLE_INVALID, .sco = HCI_CON_HANDLE_INVALID};

static bool usb_priority(void) { return get_usb_streaming() || usb_mic_requested(); }

static bool current_phone(void) {
    if (call.acl == HCI_CON_HANDLE_INVALID || !bt_hci_phone_peer_connected(call.peer)) return false;
    hci_connection_t *connection = hci_connection_for_bd_addr_and_type(call.peer, BD_ADDR_TYPE_ACL);
    return connection != NULL && connection->con_handle == call.acl;
}

bool phone_call_sco_allowed(const uint8_t address[6]) {
    return address != NULL && call.initialized && current_phone() && !usb_priority() &&
           memcmp(address, call.peer, 6) == 0;
}

bool phone_call_audio_requested(void) {
    return call.sco != HCI_CON_HANDLE_INVALID && current_phone() && !usb_priority();
}

bool phone_call_mic_requested(void) { return phone_call_audio_requested(); }

bool phone_call_active(void) {
    return call.ringing || call.in_call || call.setup || call.sco != HCI_CON_HANDLE_INVALID ||
           (call.cleanup && aacp_mic_active());
}

static void audio_stop(bool release) {
    uint16_t acl = call.acl;
    bool had_audio = call.sco != HCI_CON_HANDLE_INVALID;
    call.sco = HCI_CON_HANDLE_INVALID;
    if (had_audio) aacp_mic_call_changed();
    call.codec = call.pending_length = 0;
    call.send_failures = 0;
    call.send_requested = false;
    if (had_audio) call.cleanup = true;
    if (call.output_open) {
        call.output_open = false;
        call.cleanup = true;
        bridge_call_stop();
    }
    (void)phone_call_audio_reset(0);
    if (release && had_audio && acl != HCI_CON_HANDLE_INVALID)
        (void)hfp_hf_release_audio_connection(acl);
}

static void connection_stop(bool release) {
    uint16_t acl = call.acl;
    audio_stop(release);
    call.acl = HCI_CON_HANDLE_INVALID;
    memset(call.peer, 0, sizeof(call.peer));
    call.connecting = call.ringing = call.in_call = call.setup = call.command_pending = false;
    call.retry_wait = true;
    call.retry_at = btstack_run_loop_get_time_ms();
    if (release && acl != HCI_CON_HANDLE_INVALID)
        (void)hfp_hf_release_service_level_connection(acl);
}

static bool codec_start(uint8_t codec) {
    if (codec != HFP_CODEC_CVSD || !phone_call_audio_reset(8000u)) return false;
    call.codec = codec;
    return true;
}

static bool prepare_uplink(uint16_t length) {
    if (call.pending_length != 0) return call.pending_length == length;
    if (call.codec != HFP_CODEC_CVSD || (length & 1u)) return false;
    int16_t pcm[CALL_SCO_PAYLOAD_MAX / 2];
    phone_call_audio_uplink(pcm, length / 2u);
    for (unsigned i = 0; i < length / 2u; ++i) {
        call.pending[2u * i] = (uint8_t)pcm[i];
        call.pending[2u * i + 1u] = (uint8_t)((uint16_t)pcm[i] >> 8);
    }
    call.pending_length = (uint8_t)length;
    return true;
}

static void sco_packet(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet == NULL || !phone_call_audio_requested()) return;
    if (type == HCI_SCO_DATA_PACKET) {
        if (size < 3 || packet[2] != size - 3u || packet[2] == 0 ||
            packet[2] > CALL_SCO_PAYLOAD_MAX || (little_endian_read_16(packet, 0) & 0x0fffu) != call.sco ||
            (packet[2] & 1u)) {
            ++call.malformed;
            return;
        }
        uint8_t status = (packet[1] >> 4) & 3u;
        ++call.received;
        if (status) ++call.bad_frames;
        call.last_packet_at = btstack_run_loop_get_time_ms();
        int16_t pcm[CALL_SCO_PAYLOAD_MAX / 2];
        for (unsigned i = 0; i < packet[2] / 2u; ++i)
            pcm[i] = (int16_t)little_endian_read_16(packet, 3u + 2u * i);
        (void)phone_call_audio_downlink(pcm, packet[2] / 2u, status != 0);
    } else if (type == HCI_EVENT_PACKET && size == 4 && packet[0] == HCI_EVENT_SCO_CAN_SEND_NOW &&
               packet[1] == 2 && little_endian_read_16(packet, 2) == call.sco) {
        call.send_requested = false;
        if (!hci_can_send_sco_packet_now_for_con_handle(call.sco)) return;
        int length = hci_get_sco_packet_length_for_connection(call.sco) - 3;
        if (length <= 0 || length > (int)CALL_SCO_PAYLOAD_MAX || !prepare_uplink((uint16_t)length)) {
            audio_stop(true);
            return;
        }
        hci_reserve_packet_buffer();
        uint8_t *out = hci_get_outgoing_packet_buffer();
        little_endian_store_16(out, 0, call.sco);
        out[2] = (uint8_t)length;
        memcpy(out + 3, call.pending, (unsigned)length);
        if (hci_send_sco_packet_buffer(length + 3) == ERROR_CODE_SUCCESS) {
            ++call.sent;
            call.pending_length = 0;
            call.send_failures = 0;
        } else if (++call.send_failures >= 3u) audio_stop(true);
        // The next main-loop pass requests another packet; callbacks never recurse.
    }
}

static bool event_complete(const uint8_t *packet, uint16_t size) {
    return packet != NULL && size >= 3 && (unsigned)packet[1] + 2u == size &&
           packet[0] == HCI_EVENT_HFP_META;
}

static void hfp_packet(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (type != HCI_EVENT_PACKET || !event_complete(packet, size)) return;
    if (packet[2] == HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED) {
        if (size != 12) return;
        call.connecting = false;
        if (packet[5] != ERROR_CODE_SUCCESS) {
            call.retry_wait = true;
            call.retry_at = btstack_run_loop_get_time_ms();
            return;
        }
        bd_addr_t peer;
        reverse_bytes(packet + 6, peer, 6);
        uint16_t acl = little_endian_read_16(packet, 3);
        hci_connection_t *connection = hci_connection_for_bd_addr_and_type(peer, BD_ADDR_TYPE_ACL);
        if (!bt_hci_phone_peer_connected(peer) || connection == NULL || connection->con_handle != acl ||
            (call.acl != HCI_CON_HANDLE_INVALID && call.acl != acl)) {
            (void)hfp_hf_release_service_level_connection(acl);
            return;
        }
        memcpy(call.peer, peer, 6);
        call.acl = acl;
        call.attempts = 0;
        call.retry_wait = false;
        call.speaker_gain = call.microphone_gain = 15;
        phone_call_audio_set_gain(15, 15);
        return;
    }
    if (size < 5 || little_endian_read_16(packet, 3) != call.acl ||
        call.acl == HCI_CON_HANDLE_INVALID) return;
    switch (packet[2]) {
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
            if (size == 5) connection_stop(false);
            break;
        case HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED: {
            if (size != 21 || packet[5] != ERROR_CODE_SUCCESS) break;
            bd_addr_t peer;
            reverse_bytes(packet + 8, peer, 6);
            uint16_t sco = little_endian_read_16(packet, 6);
            if (call.sco != HCI_CON_HANDLE_INVALID) break;
            if (!phone_call_sco_allowed(peer) || sco > 0x0effu) {
                (void)hfp_hf_release_audio_connection(call.acl);
                break;
            }
            audio_stop(false);
            call.sco = sco;
            call.audio_at = btstack_run_loop_get_time_ms();
            call.last_packet_at = call.audio_at;
            call.mic_at = call.audio_at;
            if (!codec_start(packet[14])) audio_stop(true);
            else {
                aacp_mic_call_changed();
                call.mic_samples = phone_call_audio_mic_samples();
            }
            break;
        }
        case HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED:
            if (size == 7 && little_endian_read_16(packet, 5) == call.sco) audio_stop(false);
            break;
        case HFP_SUBEVENT_RING:
        case HFP_SUBEVENT_START_RINGING:
            if (size == 5 && current_phone()) call.ringing = true;
            break;
        case HFP_SUBEVENT_STOP_RINGING:
            if (size == 5) call.ringing = false;
            break;
        case HFP_SUBEVENT_CALL_ANSWERED:
            if (size == 5) { call.ringing = call.setup = false; call.in_call = true; }
            break;
        case HFP_SUBEVENT_CALL_TERMINATED:
            if (size == 5) call.ringing = call.setup = call.in_call = false;
            break;
        case HFP_SUBEVENT_AG_INDICATOR_STATUS_CHANGED:
            if (size >= 13 && memchr(packet + 12, 0, size - 12u) != NULL) {
                const char *name = (const char *)packet + 12;
                if (!strcmp(name, "call") && packet[6] <= 1) call.in_call = packet[6] != 0;
                else if (!strcmp(name, "callsetup") && packet[6] <= 3) {
                    call.setup = packet[6] != 0;
                    call.ringing = packet[6] == 1;
                }
            }
            break;
        case HFP_SUBEVENT_COMPLETE:
            if (size == 6) call.command_pending = false;
            break;
        case HFP_SUBEVENT_SPEAKER_VOLUME:
        case HFP_SUBEVENT_MICROPHONE_VOLUME:
            if (size == 6 && packet[5] <= 15) {
                if (packet[2] == HFP_SUBEVENT_SPEAKER_VOLUME) call.speaker_gain = packet[5];
                else call.microphone_gain = packet[5];
                phone_call_audio_set_gain(call.speaker_gain, call.microphone_gain);
            }
            break;
        default: break;
    }
}

bool phone_call_media_control(uint8_t operation) {
    if ((operation != AVRCP_OPERATION_ID_PLAY && operation != AVRCP_OPERATION_ID_PAUSE &&
         operation != AVRCP_OPERATION_ID_STOP) || !current_phone() || usb_priority() ||
        !(call.ringing || call.in_call || call.setup)) return false;
    if (call.command_pending) return true;
    uint8_t result = call.ringing ? hfp_hf_answer_incoming_call(call.acl) : hfp_hf_terminate_call(call.acl);
    if (result == ERROR_CODE_SUCCESS) {
        call.command_pending = true;
        call.command_at = btstack_run_loop_get_time_ms();
    }
    return true;
}

bool phone_call_init(void) {
    if (call.initialized) return true;
    gap_set_security_level(LEVEL_2);
    uint32_t features = (1u << HFP_HFSF_ESCO_S4) | (1u << HFP_HFSF_REMOTE_VOLUME_CONTROL);
    if (hfp_hf_init(CALL_CHANNEL) != ERROR_CODE_SUCCESS) return false;
    hfp_hf_init_supported_features(features);
    hfp_hf_init_codecs(sizeof(call_codecs), call_codecs);
    (void)hfp_hf_set_default_speaker_gain(15);
    (void)hfp_hf_set_default_microphone_gain(15);
    hfp_hf_register_packet_handler(hfp_packet);
    hfp_hf_create_sdp_record_with_codecs(call_sdp, sdp_create_service_record_handle(), CALL_CHANNEL,
                                       "AirPods Dongle Calls", (uint16_t)features,
                                       sizeof(call_codecs), call_codecs);
    if (de_get_len(call_sdp) > sizeof(call_sdp) || sdp_register_service(call_sdp) != ERROR_CODE_SUCCESS)
        return false;
    hci_set_sco_voice_setting(0x60);
    hci_register_sco_packet_handler(sco_packet);
    call.initialized = true;
    return true;
}

void phone_call_report(void) {
    printf("[CALL] acl=%04x sco=%04x codec=%u requested=%u ready=%u rx=%lu tx=%lu malformed=%lu bad=%lu mic_frames=%lu\n",
           call.acl, call.sco, call.codec, (unsigned)phone_call_audio_requested(),
           (unsigned)bridge_call_ready(), (unsigned long)call.received, (unsigned long)call.sent,
           (unsigned long)call.malformed, (unsigned long)call.bad_frames,
           (unsigned long)phone_call_audio_mic_samples());
}

void phone_call_poll(void) {
    if (!call.initialized) return;
    uint32_t now = btstack_run_loop_get_time_ms();
    if (call.cleanup && !aacp_mic_active()) call.cleanup = false;
    if (call.acl != HCI_CON_HANDLE_INVALID && !current_phone()) connection_stop(true);
    if (call.command_pending && (uint32_t)(now - call.command_at) >= CALL_COMMAND_MS)
        call.command_pending = false;
    if (call.sco != HCI_CON_HANDLE_INVALID && !phone_call_audio_requested()) audio_stop(true);
    phone_sink_resume_after_call();
    if (phone_call_audio_requested()) {
        if ((uint32_t)(now - call.last_packet_at) >= CALL_PREPARE_MS) {
            audio_stop(true);
            return;
        }
        if (bridge_call_open()) call.output_open = true;
        if (bridge_call_ready()) call.audio_at = now;
        else if ((uint32_t)(now - call.audio_at) >= CALL_PREPARE_MS) {
            audio_stop(true);
            return;
        }
        phone_call_audio_poll();
        uint32_t mic_samples = phone_call_audio_mic_samples();
        if (mic_samples != call.mic_samples) {
            call.mic_samples = mic_samples;
            call.mic_at = now;
        } else if ((uint32_t)(now - call.mic_at) >= CALL_PREPARE_MS) {
            audio_stop(true);
            return;
        }
        if (!call.send_requested) {
            call.send_requested = true;
            hci_request_sco_can_send_now_event_for_con_handle(call.sco);
        }
    }
    if (call.acl != HCI_CON_HANDLE_INVALID || usb_priority()) return;
    if (call.connecting) {
        if ((uint32_t)(now - call.connecting_at) < CALL_CONNECT_MS) return;
        // Cancel a timed-out outgoing SLC using its authenticated ACL handle.
        hci_connection_t *connection = hci_connection_for_bd_addr_and_type(call.peer, BD_ADDR_TYPE_ACL);
        if (connection != NULL) (void)hfp_hf_release_service_level_connection(connection->con_handle);
        call.connecting = false;
        call.retry_wait = true;
        call.retry_at = now;
    }
    if (call.attempts >= 3 || (call.retry_wait && (uint32_t)(now - call.retry_at) < CALL_RETRY_MS)) return;
    bd_addr_t peer;
    if (!bt_hci_phone_address(peer)) return;
    memcpy(call.peer, peer, 6);
    ++call.attempts;
    if (hfp_hf_establish_service_level_connection(peer) == ERROR_CODE_SUCCESS) {
        call.connecting = true;
        call.connecting_at = now;
    } else {
        call.retry_wait = true;
        call.retry_at = now;
    }
}
