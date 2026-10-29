//
// Created by Sean on 8/10/23.
//

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "btstack_hci.h"
#include "peer_address.h"
#include "btstack_avdtp_source.h"
#include "btstack_ble_scan.h"
#include "btstack_aacp.h"
#include "phone_sink.h"
#include "phone_call.h"
#include "../tinyusb/uac.h"
#include "../pico_w_led.h"
#include "../slot_storage.h"
#include "btstack_tlv.h"


#define A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS 12

static btstack_packet_callback_registration_t hci_event_callback_registration;

static char device_addr_string[] = "00:00:00:00:00:00";
static bd_addr_t device_addr;
static bd_addr_t device_addr_list[2];

static bool scan_active;
static bool selected_bonded;

#define PHONE_TAG 0x4150484eu // APHN, independent of APSL and BTstack bond records.
#define PHONE_PAIRING_MS 120000u
#define MEDIA_FLUSH_COMPARISON_MS 120000u
static bd_addr_t phone_address, phone_candidate;
static bool phone_bonded, phone_pairing, phone_storage_supported;
static uint32_t phone_pairing_since;
static uint32_t phone_session_generation;
static uint16_t phone_session_handle = HCI_CON_HANDLE_INVALID;
static bd_addr_t phone_session_address;
static uint16_t credit_raw_acl_count, credit_raw_acl_length;
static bool credit_buffer_known;
static struct {
    bt_hci_credit_role_t totals;
    uint32_t last_ms;
    uint16_t handle;
    bool armed;
} credit_roles[2];

static struct {
    bd_addr_t address;
    uint32_t epoch, since, comparison_since;
    uint16_t handle, desired_ms, confirmed_ms;
    uint8_t status;
    bool failed, low_latency, comparison;
} media_flush;
static struct {
    uint32_t epoch, since, link_epoch;
    uint16_t handle, milliseconds;
    bool active;
} media_flush_pending;
// Observe BTstack's initial 200-ms ACK before issuing another command on that ACL.
static struct { uint16_t handle, confirmed_ms; uint32_t epoch; } media_flush_links[2];
static uint32_t media_flush_link_epoch;

typedef struct {
    uint8_t version;
    uint8_t address[6];
} phone_record_t;
_Static_assert(sizeof(phone_record_t) == 7, "phone identity layout changed");

static bool airpods_address(const uint8_t address[6]) {
    return peer_address_valid(address) &&
        (memcmp(address, device_addr, 6) == 0 ||
         memcmp(address, device_addr_list[0], 6) == 0 ||
         memcmp(address, device_addr_list[1], 6) == 0);
}

static void media_flush_forget(void) {
    ++media_flush.epoch;
    media_flush.handle = HCI_CON_HANDLE_INVALID;
    media_flush.desired_ms = 200;
    media_flush.confirmed_ms = 0;
    media_flush.status = UINT8_MAX;
    media_flush.failed = false;
    media_flush.low_latency = media_flush.comparison = false;
    media_flush.since = btstack_run_loop_get_time_ms();
    memset(media_flush.address, 0, sizeof(media_flush.address));
}
static uint16_t media_flush_known_timeout(uint16_t handle) {
    for (unsigned i = 0; i < 2; ++i)
        if (media_flush_links[i].handle == handle) return media_flush_links[i].confirmed_ms;
    return 0;
}
static hci_connection_t *media_flush_bind(void) {
    hci_connection_t *connection = peer_address_valid(device_addr) ?
        hci_connection_for_bd_addr_and_type(device_addr, BD_ADDR_TYPE_ACL) : NULL;
    if (connection == NULL || connection->address_type != BD_ADDR_TYPE_ACL || connection->state != OPEN ||
        memcmp(connection->address, device_addr, 6)) {
        if (media_flush.handle != HCI_CON_HANDLE_INVALID) media_flush_forget();
        return NULL;
    }
    if (media_flush.handle != connection->con_handle || memcmp(media_flush.address, device_addr, 6)) {
        media_flush_forget();
        media_flush.handle = connection->con_handle;
        memcpy(media_flush.address, device_addr, 6);
        media_flush.confirmed_ms = media_flush_known_timeout(connection->con_handle);
        if (media_flush.confirmed_ms) {
            media_flush.status = ERROR_CODE_SUCCESS;
        }
    }
    return connection;
}
static void media_flush_policy(void) {
    if (!media_flush.low_latency || (media_flush.comparison &&
        (uint32_t)(btstack_run_loop_get_time_ms() - media_flush.comparison_since) >= MEDIA_FLUSH_COMPARISON_MS))
        media_flush.comparison = false;
    uint16_t desired = media_flush.low_latency && !media_flush.comparison ? 60 : 200;
    if (desired != media_flush.desired_ms) {
        media_flush.desired_ms = desired;
        media_flush.failed = false;
    }
}
void bt_hci_airpods_flush_timeout(bool low_latency) {
    if (media_flush_bind() == NULL || !hci_non_flushable_packet_boundary_flag_supported()) return;
    media_flush.low_latency = low_latency;
    media_flush_policy();
}
bool bt_hci_airpods_flush_comparison(bool enabled) {
    hci_connection_t *connection = media_flush_bind();
    media_flush_policy();
    if (!enabled) {
        media_flush.comparison = false;
        media_flush_policy();
        return true;
    }
    if (connection == NULL || !hci_non_flushable_packet_boundary_flag_supported() ||
        !media_flush.low_latency || media_flush.failed) return false;
    if (media_flush.comparison) return true; // Repeated requests cannot extend the bounded comparison.
    if (media_flush_pending.active || media_flush.confirmed_ms != 60) return false;
    media_flush.comparison = true;
    media_flush.comparison_since = btstack_run_loop_get_time_ms();
    media_flush_policy();
    return true;
}
static void media_flush_poll(void) {
    hci_connection_t *connection = media_flush_bind();
    media_flush_policy();
    if (media_flush_pending.active) {
        if ((uint32_t)(btstack_run_loop_get_time_ms() - media_flush_pending.since) >= 1000u &&
            media_flush_pending.epoch == media_flush.epoch) {
            media_flush.failed = true;
            media_flush.status = UINT8_MAX;
        }
        // A late ACK has no transaction ID; do not overlap another same-opcode command.
        return;
    }
    if (connection != NULL && !media_flush_known_timeout(connection->con_handle) &&
        hci_non_flushable_packet_boundary_flag_supported() &&
        (uint32_t)(btstack_run_loop_get_time_ms() - media_flush.since) >= 1000u) {
        media_flush.failed = true;
        media_flush.status = UINT8_MAX;
    }
    if (connection == NULL || !hci_non_flushable_packet_boundary_flag_supported() ||
        !media_flush_known_timeout(connection->con_handle) || media_flush.failed ||
        media_flush.desired_ms == media_flush.confirmed_ms || !hci_can_send_command_packet_now()) return;
    media_flush_pending.epoch = media_flush.epoch;
    media_flush_pending.handle = connection->con_handle;
    media_flush_pending.milliseconds = media_flush.desired_ms;
    media_flush_pending.since = btstack_run_loop_get_time_ms();
    for (unsigned i = 0; i < 2; ++i)
        if (media_flush_links[i].handle == connection->con_handle)
            media_flush_pending.link_epoch = media_flush_links[i].epoch;
    media_flush_pending.active = true;
    uint8_t status = hci_send_cmd(&hci_write_automatic_flush_timeout, connection->con_handle,
                                 media_flush.desired_ms * 8u / 5u);
    if (status != ERROR_CODE_SUCCESS && media_flush_pending.active &&
        media_flush_pending.epoch == media_flush.epoch) {
        media_flush_pending.active = false;
        media_flush.failed = true;
        media_flush.status = status;
    }
}
static void media_flush_event(const uint8_t *p, uint16_t size) {
    if (p[0] == BTSTACK_EVENT_STATE && size >= 3 && p[2] != HCI_STATE_WORKING) {
        media_flush_forget();
        media_flush_pending.active = false;
        media_flush_links[0].handle = media_flush_links[1].handle = HCI_CON_HANDLE_INVALID;
    } else if (p[0] == HCI_EVENT_DISCONNECTION_COMPLETE && size == 6 && p[2] == 0) {
        uint16_t handle = (uint16_t)p[3] | (uint16_t)p[4] << 8;
        for (unsigned i = 0; i < 2; ++i)
            if (media_flush_links[i].handle == handle) media_flush_links[i].handle = HCI_CON_HANDLE_INVALID;
        if (media_flush.handle == handle) media_flush_forget();
    } else if (p[0] == HCI_EVENT_COMMAND_COMPLETE && size == 8 &&
        ((uint16_t)p[3] | (uint16_t)p[4] << 8) == HCI_OPCODE_HCI_WRITE_AUTOMATIC_FLUSH_TIMEOUT) {
        uint16_t handle = (uint16_t)p[6] | (uint16_t)p[7] << 8;
        (void)media_flush_bind();
        if (media_flush_pending.active && media_flush_pending.handle == handle) {
            media_flush_pending.active = false;
            if (p[5] == ERROR_CODE_SUCCESS) {
                for (unsigned i = 0; i < 2; ++i)
                    if (media_flush_links[i].handle == handle &&
                        media_flush_links[i].epoch == media_flush_pending.link_epoch)
                        media_flush_links[i].confirmed_ms = media_flush_pending.milliseconds;
            }
            if (media_flush_pending.epoch != media_flush.epoch) return;
            media_flush.status = p[5];
            media_flush.failed = p[5] != ERROR_CODE_SUCCESS &&
                media_flush.desired_ms == media_flush_pending.milliseconds;
            if (p[5] == ERROR_CODE_SUCCESS) {
                media_flush.confirmed_ms = media_flush_pending.milliseconds;
            }
            return;
        }
        hci_connection_t *connection = hci_connection_for_handle(handle);
        if (connection == NULL || connection->address_type != BD_ADDR_TYPE_ACL ||
            connection->state != OPEN || !airpods_address(connection->address) ||
            media_flush_known_timeout(handle)) return;
        if (p[5] == ERROR_CODE_SUCCESS) {
            for (unsigned i = 0; i < 2; ++i) {
                if (media_flush_links[i].handle != HCI_CON_HANDLE_INVALID) continue;
                media_flush_links[i].handle = handle;
                media_flush_links[i].confirmed_ms = 200;
                if (++media_flush_link_epoch == 0) ++media_flush_link_epoch;
                media_flush_links[i].epoch = media_flush_link_epoch;
                break;
            }
        }
        if (media_flush.handle == handle) {
            media_flush.status = p[5];
            media_flush.failed = p[5] != ERROR_CODE_SUCCESS;
            if (p[5] == ERROR_CODE_SUCCESS) media_flush.confirmed_ms = 200;
        }
    }
}

static void phone_load(void) {
    const btstack_tlv_t *tlv;
    void *context;
    btstack_tlv_get_instance(&tlv, &context);
    phone_record_t record;
    memset(phone_address, 0, sizeof(phone_address));
    memset(phone_candidate, 0, sizeof(phone_candidate));
    phone_bonded = phone_pairing = false;
    phone_storage_supported = tlv != NULL;
    if (tlv == NULL) return;
    int length = tlv->get_tag(context, PHONE_TAG, NULL, 0);
    if (length == 0) return;
    phone_storage_supported = length == (int)sizeof(record) &&
        tlv->get_tag(context, PHONE_TAG, (uint8_t *)&record, sizeof(record)) == (int)sizeof(record) &&
        record.version == 1 && peer_address_valid(record.address);
    if (!phone_storage_supported || airpods_address(record.address)) return;
    memcpy(phone_address, record.address, 6);
    link_key_t key;
    link_key_type_t type;
    phone_bonded = gap_get_link_key_for_bd_addr(phone_address, key, &type);
}

static bool phone_pairing_audio_busy(void) {
    return get_usb_streaming() || usb_mic_requested() || aacp_mic_active() || phone_sink_active() || phone_call_active();
}

static uint16_t credit_u16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t credit_add(uint32_t value, uint32_t added) {
    return value > UINT32_MAX - added ? UINT32_MAX : value + added;
}
static void credit_event(const uint8_t *p, uint16_t size) {
    if (p[0] == HCI_EVENT_COMMAND_COMPLETE && size >= 13 &&
        credit_u16(p + 3) == HCI_OPCODE_HCI_READ_BUFFER_SIZE && p[5] == 0) {
        credit_raw_acl_length = credit_u16(p + 6);
        credit_raw_acl_count = credit_u16(p + 9);
        credit_buffer_known = true;
    } else if (p[0] == HCI_EVENT_DISCONNECTION_COMPLETE && size == 6 && p[2] == 0) {
        uint16_t handle = credit_u16(p + 3) & 0x0fffu;
        for (unsigned i = 0; i < 2; ++i)
            if (credit_roles[i].handle == handle) credit_roles[i].armed = false;
    } else if (p[0] == BTSTACK_EVENT_STATE && size >= 3 && p[2] != HCI_STATE_WORKING) {
        credit_buffer_known = false;
        credit_roles[0].armed = credit_roles[1].armed = false;
    } else if (p[0] == HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS && size >= 3 && size == 3u + p[2] * 4u) {
        uint32_t now = btstack_run_loop_get_time_ms();
        bool active = phone_pairing_audio_busy();
        for (unsigned offset = 3; offset < size; offset += 4) {
            uint16_t handle = credit_u16(p + offset) & 0x0fffu;
            hci_connection_t *connection = hci_connection_for_handle(handle);
            if (connection == NULL || connection->address_type != BD_ADDR_TYPE_ACL) continue;
            int role = memcmp(connection->address, device_addr, 6) == 0 ? 0 :
                ((peer_address_valid(phone_address) && memcmp(connection->address, phone_address, 6) == 0) ||
                 (peer_address_valid(phone_candidate) && memcmp(connection->address, phone_candidate, 6) == 0)) ? 1 : -1;
            if (role < 0) continue;
            uint16_t completed = credit_u16(p + offset + 2);
            credit_roles[role].totals.events = credit_add(credit_roles[role].totals.events, 1);
            credit_roles[role].totals.packets = credit_add(credit_roles[role].totals.packets, completed);
            if (!completed) continue;
            if (active && credit_roles[role].armed && credit_roles[role].handle == handle) {
                uint32_t gap = now - credit_roles[role].last_ms;
                if (gap > credit_roles[role].totals.max_active_gap_ms)
                    credit_roles[role].totals.max_active_gap_ms = gap;
            }
            credit_roles[role].handle = handle;
            credit_roles[role].last_ms = now;
            credit_roles[role].armed = active;
        }
    }
}

static void credit_link(const uint8_t address[6], uint16_t *handle, uint8_t *role) {
    *handle = HCI_CON_HANDLE_INVALID;
    *role = UINT8_MAX;
    if (!peer_address_valid(address)) return;
    hci_connection_t *connection = hci_connection_for_bd_addr_and_type(address, BD_ADDR_TYPE_ACL);
    if (connection == NULL || connection->state != OPEN) return;
    *handle = connection->con_handle;
    if (connection->role == HCI_ROLE_MASTER || connection->role == HCI_ROLE_SLAVE)
        *role = (uint8_t)connection->role;
}

bool bt_hci_credit_diagnostics(bt_hci_credit_diagnostics_t *out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    out->raw_acl_count = credit_raw_acl_count;
    out->raw_acl_length = credit_raw_acl_length;
    out->airpods = credit_roles[0].totals;
    out->phone = credit_roles[1].totals;
    out->media_flush_desired_ms = media_flush.desired_ms;
    out->media_flush_confirmed_ms = media_flush.confirmed_ms;
    out->media_flush_status = media_flush.status;
    out->media_flush_pending = media_flush_pending.active;
    out->media_flush_failed = media_flush.failed;
    credit_link(device_addr, &out->airpods_handle, &out->airpods_local_role);
    credit_link(phone_address, &out->phone_handle, &out->phone_local_role);
    if (out->phone_handle == HCI_CON_HANDLE_INVALID)
        credit_link(phone_candidate, &out->phone_handle, &out->phone_local_role);
    if (credit_buffer_known) {
        out->effective_acl_limit = credit_raw_acl_count < MAX_NR_CONTROLLER_ACL_BUFFERS ?
            credit_raw_acl_count : MAX_NR_CONTROLLER_ACL_BUFFERS;
        out->free_acl_slots = hci_number_free_acl_slots_for_connection_type(BD_ADDR_TYPE_ACL);
        out->outstanding_acl_packets = out->free_acl_slots <= out->effective_acl_limit ?
            out->effective_acl_limit - out->free_acl_slots : 0;
        out->packet_buffer_reserved = hci_is_packet_buffer_reserved();
    }
    return credit_buffer_known;
}

bool bt_hci_phone_pairing_busy(void) {
    return phone_pairing_audio_busy() || dongle_playback_transitioning();
}

bool bt_hci_phone_pairing_active(void) {
    return phone_pairing &&
        // An idle suspend/start must not revoke an already opened pairing window.
        !phone_pairing_audio_busy() &&
        (uint32_t)(btstack_run_loop_get_time_ms() - phone_pairing_since) < PHONE_PAIRING_MS;
}

static void phone_pairing_stop(void) {
    bd_addr_t candidate;
    memcpy(candidate, phone_candidate, sizeof(candidate));
    phone_pairing = false;
    memset(phone_candidate, 0, sizeof(phone_candidate));
    gap_discoverable_control(0);
    // Close only the uncommitted candidate; an existing phone bond remains usable.
    if (peer_address_valid(candidate) &&
        !(phone_bonded && memcmp(candidate, phone_address, 6) == 0)) {
        hci_connection_t *connection = hci_connection_for_bd_addr_and_type(candidate, BD_ADDR_TYPE_ACL);
        if (connection != NULL) gap_disconnect(connection->con_handle);
    }
}

void bt_hci_phone_poll(void) {
    media_flush_poll();
    if (!phone_pairing_audio_busy()) credit_roles[0].armed = credit_roles[1].armed = false;
    if (phone_pairing && !bt_hci_phone_pairing_active()) phone_pairing_stop();
}

bool bt_hci_phone_pairing_start(void) {
    if (bt_hci_phone_pairing_busy()) {
        bt_hci_phone_poll();
        printf("Pause audio before pairing a new phone\n");
        return false;
    }
    if (!phone_storage_supported) {
        printf("Phone identity storage unavailable or unsupported\n");
        return false;
    }
    if (phone_pairing) phone_pairing_stop();
    phone_pairing_since = btstack_run_loop_get_time_ms();
    phone_pairing = true;
    gap_discoverable_control(1);
    printf("Phone pairing available for 120 seconds\n");
    return true;
}

bool bt_hci_phone_peer_allowed(const uint8_t address[6]) {
    if (address == NULL || !peer_address_valid(address) || airpods_address(address)) return false;
    if (phone_bonded && memcmp(address, phone_address, 6) == 0) return true;
    bt_hci_phone_poll();
    return bt_hci_phone_pairing_active() && memcmp(address, phone_candidate, 6) == 0;
}

bool bt_hci_phone_peer_connected(const uint8_t address[6]) {
    if (!bt_hci_phone_peer_allowed(address)) return false;
    hci_connection_t *connection = hci_connection_for_bd_addr_and_type(address, BD_ADDR_TYPE_ACL);
    if (connection == NULL || gap_security_level(connection->con_handle) < LEVEL_2) return false;
    link_key_t key;
    link_key_type_t type;
    if (!gap_get_link_key_for_bd_addr((uint8_t *)address, key, &type)) return false;
    if (phone_bonded && memcmp(address, phone_address, 6) == 0) return true;
    const btstack_tlv_t *tlv;
    void *context;
    btstack_tlv_get_instance(&tlv, &context);
    if (!phone_storage_supported || tlv == NULL) return false;
    phone_record_t record = {.version = 1}, verify;
    memcpy(record.address, address, 6);
    if (!bt_hci_phone_pairing_active()) {
        bt_hci_phone_poll();
        return false;
    }
    // shortcut: BTstack may already have stored an in-flight link-key notification
    // before our callback; coordinating that boundary needs a separate SDK change.
    if (tlv->store_tag(context, PHONE_TAG, (const uint8_t *)&record, sizeof(record)) != 0 ||
        tlv->get_tag(context, PHONE_TAG, NULL, 0) != (int)sizeof(verify) ||
        tlv->get_tag(context, PHONE_TAG, (uint8_t *)&verify, sizeof(verify)) != (int)sizeof(verify) ||
        memcmp(&record, &verify, sizeof(record)) != 0) return false;
    memcpy(phone_address, address, 6);
    phone_bonded = true;
    phone_pairing = false;
    memset(phone_candidate, 0, sizeof(phone_candidate));
    gap_discoverable_control(0);
    return true;
}

bool bt_hci_phone_address(uint8_t address[6]) {
    if (address == NULL || !phone_bonded || !peer_address_valid(phone_address)) return false;
    memcpy(address, phone_address, 6);
    return true;
}

uint32_t bt_hci_phone_session(const uint8_t address[6]) {
    if (!bt_hci_phone_peer_connected(address)) return 0;
    hci_connection_t *connection = hci_connection_for_bd_addr_and_type(address, BD_ADDR_TYPE_ACL);
    if (connection == NULL || connection->state != OPEN) return 0;
    if (phone_session_handle != connection->con_handle || memcmp(phone_session_address, address, 6)) {
        if (++phone_session_generation == 0) ++phone_session_generation;
        phone_session_handle = connection->con_handle;
        memcpy(phone_session_address, address, 6);
    }
    return phone_session_generation;
}

static int accept_classic_peer(bd_addr_t address, hci_link_type_t link_type) {
    if (!peer_address_valid(address)) return 0;
    if (link_type != HCI_LINK_TYPE_ACL)
        return (link_type == HCI_LINK_TYPE_SCO || link_type == HCI_LINK_TYPE_ESCO) &&
            phone_call_sco_allowed(address) && bt_hci_phone_peer_connected(address);
    if (peer_address_valid(device_addr) && memcmp(address, device_addr, 6) == 0) return 1;
    if (bt_hci_phone_peer_allowed(address)) return 1;
    if (!bt_hci_phone_pairing_active() || airpods_address(address) ||
        peer_address_valid(phone_candidate)) return 0;
    memcpy(phone_candidate, address, 6);
    return 1;
}

static bool pairing_peer_allowed(const uint8_t address[6]) {
    bt_hci_phone_poll();
    return (peer_address_valid(device_addr) && memcmp(address, device_addr, 6) == 0) ||
        (bt_hci_phone_pairing_active() && bt_hci_phone_peer_allowed(address));
}


const char * get_device_addr_string(){
    return device_addr_string;
}

bd_addr_t * get_device_addr(){
    return &device_addr;
}

bd_addr_t * get_device_addr_from_list(uint8_t i){
    return i < 2 ? &device_addr_list[i] : NULL;
}

bool gap_start_scanning(void){
    if (scan_active) return true;
    printf("Start scanning...\n");
    ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, true);
    scan_active = gap_inquiry_start(A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS) == ERROR_CODE_SUCCESS;
    if (!scan_active) ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, false);
    set_led_mode_off();
    set_led_mode_pairing();
    return scan_active;
}


static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    if (packet_type != HCI_EVENT_PACKET || size < 2 || (unsigned)packet[1] + 2 != size) return;
    credit_event(packet, size);
    media_flush_event(packet, size);
    dongle_playback_controller_event(packet,size);
    uint8_t status;
    UNUSED(status);

    bd_addr_t address;
    uint32_t cod;

    // Service Class: Rendering | Audio, Major Device Class: Audio
    const uint32_t bluetooth_speaker_cod = 0x200000 | 0x040000 | 0x000400;

    switch (hci_event_packet_get_type(packet)){
        case  BTSTACK_EVENT_STATE:
            if (size < 3) return;
            ble_scan_controller_ready(btstack_event_state_get_state(packet) == HCI_STATE_WORKING);
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) {
                phone_session_handle = HCI_CON_HANDLE_INVALID;
                scan_active = false;
                phone_pairing = false;
                memset(phone_candidate, 0, sizeof(phone_candidate));
                gap_discoverable_control(0);
                ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, false);
                return;
            }
            // Control must remain reliable when only media is allowed to expire.
            gap_enable_link_watchdog(hci_non_flushable_packet_boundary_flag_supported() ? 200 : 0);
            if (selected_bonded) {
                avdtp_source_establish_stream();
            } else if (!peer_address_valid(device_addr_list[0]) &&
                       !peer_address_valid(device_addr_list[1])) {
                gap_start_scanning();
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (size < 6) return;
            if (packet[2] == ERROR_CODE_SUCCESS &&
                (credit_u16(packet + 3) & 0x0fffu) == phone_session_handle)
                phone_session_handle = HCI_CON_HANDLE_INVALID;
            printf("[HCI] disconnect: status=0x%02x reason=0x%02x\n", packet[2], packet[5]);
            break;
        case HCI_EVENT_COMMAND_STATUS:
            if (size < 6 || !scan_active ||
                hci_event_command_status_get_command_opcode(packet) != HCI_OPCODE_HCI_INQUIRY ||
                hci_event_command_status_get_status(packet) == ERROR_CODE_SUCCESS) break;
            scan_active = false;
            ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, false);
            set_led_mode_off();
            printf("Pairing inquiry rejected: 0x%02x\n", hci_event_command_status_get_status(packet));
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            if (size < 8) return;
            hci_event_pin_code_request_get_bd_addr(packet, address);
            if (peer_address_valid(device_addr) && memcmp(address, device_addr, 6) == 0)
                gap_pin_code_response(address, "0000");
            else gap_pin_code_negative(address);
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            if (size < 12) return;
            hci_event_user_confirmation_request_get_bd_addr(packet, address);
            if (pairing_peer_allowed(address)) gap_ssp_confirmation_response(address);
            else gap_ssp_confirmation_negative(address);
            break;
        case HCI_EVENT_USER_PASSKEY_REQUEST:
            if (size < 8) return;
            hci_event_user_passkey_request_get_bd_addr(packet, address);
            gap_ssp_passkey_negative(address); // There is no local passkey input.
            break;
        case GAP_EVENT_INQUIRY_RESULT:
            if (size < 27) return;
            gap_event_inquiry_result_get_bd_addr(packet, address);
            // print info
            printf("Device found: %s ",  bd_addr_to_str(address));
            cod = gap_event_inquiry_result_get_class_of_device(packet);
            printf("with COD: %06" PRIx32, cod);
            if (gap_event_inquiry_result_get_rssi_available(packet)){
                printf(", rssi %d dBm", (int8_t) gap_event_inquiry_result_get_rssi(packet));
            }
            if (gap_event_inquiry_result_get_name_available(packet)){
                char name_buffer[240];
                size_t name_len = gap_event_inquiry_result_get_name_len(packet);
                if (name_len > size - 27u) break;
                if (name_len >= sizeof(name_buffer)) name_len = sizeof(name_buffer) - 1;
                memcpy(name_buffer, gap_event_inquiry_result_get_name(packet), name_len);
                name_buffer[name_len] = 0;
                printf(", name '%s'", name_buffer);
            }
            printf("\n");
            if (scan_active && (cod & bluetooth_speaker_cod) == bluetooth_speaker_cod &&
                memcmp(address, phone_address, 6) != 0 && memcmp(address, phone_candidate, 6) != 0){
                scan_active = false;
                gap_inquiry_stop();
                ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, false);

                uint8_t currect_slot = read_uint8_last_flash();
                bool saved = (currect_slot == 1 && write_slot1_mac(address)) ||
                             (currect_slot == 2 && write_slot2_mac(address));
                if (!saved) {
                    set_led_mode_off();
                    printf("Pairing address could not be saved\n");
                    break;
                }
                memcpy(device_addr, address, 6);
                snprintf(device_addr_string, sizeof(device_addr_string), "%s", bd_addr_to_str(address));
                printf("Bluetooth speaker detected, trying to connect to %s...\n", bd_addr_to_str(device_addr));
                avdtp_source_establish_stream();
            }
            break;
        case GAP_EVENT_INQUIRY_COMPLETE:
            if (scan_active){
                printf("No Bluetooth speakers found, scanning again...\n");
                scan_active = gap_inquiry_start(A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS) == ERROR_CODE_SUCCESS;
                if (!scan_active) ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, false);
            }
            break;
        default:
            break;
    }
}


void get_link_keys(void){
    read_slot1_mac(device_addr_list[0]);
    read_slot2_mac(device_addr_list[1]);
    bool bonded[2] = {false, false};
    link_key_t link_key;
    link_key_type_t type;
    for (unsigned i = 0; i < 2; ++i) {
        if (peer_address_valid(device_addr_list[i])) {
            bonded[i] = gap_get_link_key_for_bd_addr(device_addr_list[i], link_key, &type);
        }
    }
    // Slot identity is authoritative; key iteration order cannot identify a peer.
    selected_bonded = peer_select(device_addr_list, bonded, read_uint8_last_flash(), device_addr);
    snprintf(device_addr_string, sizeof(device_addr_string), "%s", bd_addr_to_str(device_addr));
}


static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    bd_addr_t local_addr;
    if (packet_type != HCI_EVENT_PACKET || size < 2 || (unsigned)packet[1] + 2 != size) return;
    switch(hci_event_packet_get_type(packet)){
        case BTSTACK_EVENT_STATE:
            if (size < 3) return;
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
            gap_local_bd_addr(local_addr);
            printf("BTstack up and running on %s.\n", bd_addr_to_str(local_addr));
            break;
        default:
            break;
    }
}


// separate registration for the state logger — reusing hci_event_callback_registration
// for both handlers means the second hci_add_event_handler() call is rejected as a
// duplicate list item and only the callback assigned last would ever run
static btstack_packet_callback_registration_t hci_state_callback_registration;

void bt_hci_init(void){
    media_flush_forget();
    media_flush_pending.active = false;
    media_flush_links[0].handle = media_flush_links[1].handle = HCI_CON_HANDLE_INVALID;
    phone_session_generation = 0;
    phone_session_handle = HCI_CON_HANDLE_INVALID;
    memset(phone_session_address, 0, sizeof(phone_session_address));
    credit_buffer_known = false;
    credit_raw_acl_count = credit_raw_acl_length = 0;
    memset(credit_roles, 0, sizeof(credit_roles));

    // inform about BTstack state
    hci_state_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_state_callback_registration);

    // Request role change on reconnecting headset to always use them in slave mode
    hci_set_master_slave_policy(0);
    // enabled EIR
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    // Set local name with a template Bluetooth address, that will be automatically
    // replaced with a actual address once it is available, i.e. when BTstack boots
    // up and starts talking to a Bluetooth module.
    gap_set_local_name("AirPods Dongle");
    gap_discoverable_control(0);
    gap_set_class_of_device(0x240408);
    gap_connectable_control(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(0);
    gap_register_classic_connection_filter(accept_classic_peer);

    /* Register for HCI events */
    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    get_link_keys();
    phone_load();

}
