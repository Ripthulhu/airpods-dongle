#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdlib.h>
#include "hci_stubs/btstack.h"

#define HCI_EVENT_CONNECTION_PACKET_TYPE_CHANGED 0x1d
#define HCI_OPCODE_HCI_CHANGE_CONNECTION_PACKET_TYPE 0x040f
#define ACL_PACKET_TYPES_DM1 0x0008
#define ACL_PACKET_TYPES_EDR3 0x2204
#define ACL_PACKET_TYPES_ALL 0xff1e
extern const hci_cmd_t hci_change_connection_packet_type;
uint16_t hci_usable_acl_packet_types(void);

// The transport header has unrelated static encoder declarations.
#define PICOW_USB_BT_AUDIO_SSP_COUNTER_H
void avdtp_source_establish_stream(void);
bool get_usb_streaming(void);
bool dongle_playback_transitioning(void);
void dongle_playback_controller_event(const uint8_t *p,uint16_t size) {(void)p;(void)size;}
#include "../src/btstack/btstack_hci.c"

static bd_addr_t saved[2];
static bool has_key[2], non_flushable;
static uint8_t slot, inquiry_result;
static unsigned connect_calls, inquiry_calls, inquiry_stops, writes, pin_calls;
static unsigned registered;
static uint16_t flush_timeout;
static unsigned flush_writes;
static uint16_t flush_handle, flush_slots;
static bool command_ready;
static uint8_t command_result;
static unsigned packet_writes;
static uint16_t packet_handle, packet_mask, usable_packet_mask;
static uint8_t packet_result;
static bool packet_sync_complete, consume_command_credit;
static unsigned radio_writes;
static uint16_t radio_opcode, radio_handle;
static uint8_t radio_power_type, radio_result;
static void packet_status(uint8_t status);
static void packet_changed(uint16_t handle,uint16_t mask,uint8_t status);
static bool ble_ready, pairing_led;
static bool fail_storage;
static uint32_t now;
static jmp_buf recovery;
static bool recovery_expected;
static unsigned recovery_count;
_Noreturn void panic(const char *message, ...) {
    assert(!strcmp(message,"Controller command reply stalled for 35000 ms"));
    if(!recovery_expected){fprintf(stderr,"Unexpected controller recovery at %u ms\n",now);abort();}
    ++recovery_count;longjmp(recovery,1);
}
static unsigned pin_rejections, confirmations, confirmation_rejections, passkey_rejections;
static uint8_t discoverable, security_level;
static bool phone_has_key, missing_acl, tlv_missing, tlv_fail, tlv_corrupt;
static unsigned phone_writes;
static unsigned busy_routes, disconnect_calls;
static bool busy_during_key_read;
static bool call_sco_allowed;
static hci_connection_t credit_connections[3];
static uint16_t credit_free_slots;
static bool credit_packet_reserved;
static phone_record_t stored_phone;
static int stored_phone_length;
static bd_addr_t phone_key_address;
static int (*connection_filter)(bd_addr_t, hci_link_type_t);
static uint32_t ble_pauses;
static btstack_packet_callback_registration_t *callbacks[2];
static const bd_addr_t first = {0x10, 2, 3, 4, 5, 6};
static const bd_addr_t second = {0x20, 8, 9, 10, 11, 12};
static const bd_addr_t nearby = {0x30, 0x22, 0x33, 0x44, 0x55, 0x66};

uint8_t gap_inquiry_start(uint8_t duration) {
    assert(duration == A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS);
    inquiry_calls++;
    return inquiry_result;
}
void gap_inquiry_stop(void) { inquiry_stops++; }
void ble_scan_controller_ready(bool ready) { ble_ready = ready; }
void ble_scan_set_paused(uint32_t reason, bool paused) {
    if (paused) ble_pauses |= reason;
    else ble_pauses &= ~reason;
}
void gap_enable_link_watchdog(uint16_t timeout) { flush_timeout = timeout; }
const hci_cmd_t hci_write_automatic_flush_timeout = {HCI_OPCODE_HCI_WRITE_AUTOMATIC_FLUSH_TIMEOUT,"H2"};
const hci_cmd_t hci_change_connection_packet_type = {HCI_OPCODE_HCI_CHANGE_CONNECTION_PACKET_TYPE,"H2"};
const hci_cmd_t hci_read_rssi={HCI_OPCODE_HCI_READ_RSSI,"H"};
const hci_cmd_t hci_read_link_quality={HCI_OPCODE_HCI_READ_LINK_QUALITY,"H"};
const hci_cmd_t hci_read_transmit_power_level={HCI_OPCODE_HCI_READ_TRANSMIT_POWER_LEVEL,"H1"};
uint16_t hci_usable_acl_packet_types(void) {return usable_packet_mask;}
bool hci_can_send_command_packet_now(void) { return command_ready; }
uint8_t hci_send_cmd(const hci_cmd_t *cmd, ...) {
    va_list args;
    va_start(args, cmd);
    uint16_t handle = (uint16_t)va_arg(args, int);
    if(cmd==&hci_read_rssi || cmd==&hci_read_link_quality || cmd==&hci_read_transmit_power_level || cmd->opcode==0x1406) {
        radio_opcode=cmd->opcode;radio_handle=handle;++radio_writes;
        radio_power_type=cmd==&hci_read_transmit_power_level?(uint8_t)va_arg(args,int):0;
        va_end(args);
        if(consume_command_credit)command_ready=false;
        return radio_result;
    }
    uint16_t value = (uint16_t)va_arg(args, int);
    va_end(args);
    if(consume_command_credit)command_ready=false;
    if (cmd == &hci_write_automatic_flush_timeout) {
        flush_handle=handle;flush_slots=value;++flush_writes;return command_result;
    }
    assert(cmd == &hci_change_connection_packet_type);
    packet_handle=handle;packet_mask=value;++packet_writes;
    if(packet_sync_complete) {packet_status(0);packet_changed(handle,value,0);}
    return packet_result;
}
bool hci_non_flushable_packet_boundary_flag_supported(void) { return non_flushable; }
void gap_pin_code_response(bd_addr_t address, const char *pin) {
    (void)address;
    assert(strcmp(pin, "0000") == 0);
    pin_calls++;
}
bool gap_get_link_key_for_bd_addr(bd_addr_t address, link_key_t key, link_key_type_t *type) {
    if (phone_has_key && memcmp(address, phone_key_address, 6) == 0) {
        if (busy_during_key_read) busy_routes = 1;
        memset(key, 0x22, sizeof(link_key_t));
        *type = 1;
        return true;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (memcmp(address, saved[i], 6) == 0 && has_key[i]) {
            memset(key, 0x11, sizeof(link_key_t));
            *type = 1;
            return true;
        }
    }
    return false;
}
void gap_local_bd_addr(bd_addr_t address) { memcpy(address, first, 6); }
void hci_add_event_handler(btstack_packet_callback_registration_t *registration) {
    assert(registered < 2);
    callbacks[registered++] = registration;
}
void hci_set_master_slave_policy(uint8_t policy) { assert(policy == 0); }
void hci_set_inquiry_mode(uint8_t mode) { assert(mode == INQUIRY_MODE_RSSI_AND_EIR); }
void gap_set_local_name(const char *name) { assert(name != NULL); }
void gap_discoverable_control(uint8_t enabled) { assert(enabled <= 1); discoverable = enabled; }
void gap_set_class_of_device(uint32_t cod) { assert(cod == 0x240408); }
void gap_connectable_control(uint8_t enabled) { assert(enabled == 1); }
void gap_ssp_set_io_capability(int capability) { assert(capability == SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT); }
void gap_ssp_set_authentication_requirement(int requirement) {
    assert(requirement == SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
}
void gap_ssp_set_auto_accept(int auto_accept) { assert(auto_accept == 0); }
void gap_register_classic_connection_filter(int (*callback)(bd_addr_t, hci_link_type_t)) {
    connection_filter = callback;
}
int gap_pin_code_negative(bd_addr_t address) { (void)address; ++pin_rejections; return 0; }
int gap_ssp_confirmation_response(const uint8_t address[6]) { (void)address; ++confirmations; return 0; }
int gap_ssp_confirmation_negative(const uint8_t address[6]) { (void)address; ++confirmation_rejections; return 0; }
int gap_ssp_passkey_negative(const uint8_t address[6]) { (void)address; ++passkey_rejections; return 0; }
uint32_t btstack_run_loop_get_time_ms(void) { return now; }
hci_connection_t *hci_connection_for_bd_addr_and_type(const uint8_t address[6], uint8_t type) {
    assert(type == BD_ADDR_TYPE_ACL && address != NULL);
    if (missing_acl) return NULL;
    for (unsigned i=0;i<3;++i)
        if (credit_connections[i].address_type==type && !memcmp(credit_connections[i].address,address,6))
            return &credit_connections[i];
    static hci_connection_t connection = {.con_handle = 1};
    return &connection;
}
hci_connection_t *hci_connection_for_handle(uint16_t handle) {
    for (unsigned i=0;i<3;++i)
        if (credit_connections[i].con_handle==handle) return &credit_connections[i];
    return NULL;
}
uint16_t hci_number_free_acl_slots_for_connection_type(uint8_t address_type) {
    assert(address_type==BD_ADDR_TYPE_ACL);return credit_free_slots;
}
bool hci_is_packet_buffer_reserved(void) { return credit_packet_reserved; }
uint8_t gap_security_level(uint16_t handle) { assert(handle == 1); return security_level; }
uint8_t gap_disconnect(uint16_t handle) { assert(handle == 1); ++disconnect_calls; return 0; }
bool get_usb_streaming(void) { return (busy_routes & 1u) != 0; }
bool dongle_playback_transitioning(void) { return (busy_routes & 2u) != 0; }
bool usb_mic_requested(void) { return (busy_routes & 4u) != 0; }
bool aacp_mic_active(void) { return (busy_routes & 8u) != 0; }
void aacp_routing_override(void) {}
bool aacp_takeover_request(const uint8_t peer[6]) {
    assert(peer_address_valid(peer) && !memcmp(peer, device_addr, 6));
    return true;
}
bool phone_sink_active(void) { return (busy_routes & 16u) != 0; }
bool phone_call_active(void) { return (busy_routes & 32u) != 0; }
bool phone_call_sco_allowed(const uint8_t address[6]) { (void)address; return call_sco_allowed; }
static int phone_get_tag(void *context, uint32_t tag, uint8_t *buffer, uint32_t size) {
    (void)context;
    assert(tag == PHONE_TAG);
    if (buffer == NULL) return stored_phone_length;
    unsigned copy = (unsigned)stored_phone_length;
    if (copy > size) copy = size;
    if (copy > sizeof(stored_phone)) copy = sizeof(stored_phone);
    memcpy(buffer, &stored_phone, copy);
    if (tlv_corrupt && copy != 0) buffer[0] ^= 1;
    return (int)copy;
}
static int phone_store_tag(void *context, uint32_t tag, const uint8_t *buffer, uint32_t size) {
    (void)context;
    assert(tag == PHONE_TAG && size == sizeof(stored_phone));
    if (tlv_fail) return -1;
    memcpy(&stored_phone, buffer, size);
    stored_phone_length = (int)size;
    ++phone_writes;
    return 0;
}
void btstack_tlv_get_instance(const btstack_tlv_t **tlv, void **context) {
    static const btstack_tlv_t instance = {.get_tag = phone_get_tag, .store_tag = phone_store_tag};
    *context = NULL;
    *tlv = tlv_missing ? NULL : &instance;
}
const char *bd_addr_to_str(const uint8_t *address) {
    static char text[18];
    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
             address[0], address[1], address[2], address[3], address[4], address[5]);
    return text;
}
void avdtp_source_establish_stream(void) {
    assert(peer_address_valid(device_addr));
    connect_calls++;
}
void set_led_mode_off(void) { pairing_led = false; }
void set_led_mode_pairing(void) { pairing_led = true; }
uint8_t read_uint8_last_flash(void) { return slot; }
void read_slot1_mac(uint8_t mac[MAC_LEN]) { memcpy(mac, saved[0], MAC_LEN); }
void read_slot2_mac(uint8_t mac[MAC_LEN]) { memcpy(mac, saved[1], MAC_LEN); }
bool write_slot1_mac(const uint8_t mac[MAC_LEN]) {
    if (fail_storage) return false;
    memcpy(saved[0], mac, MAC_LEN);
    writes++;
    return true;
}
bool write_slot2_mac(const uint8_t mac[MAC_LEN]) {
    if (fail_storage) return false;
    memcpy(saved[1], mac, MAC_LEN);
    writes++;
    return true;
}

static void fixture(void) {
    memset(saved, 0xff, sizeof(saved));
    memset(device_addr, 0, sizeof(device_addr));
    memset(device_addr_list, 0, sizeof(device_addr_list));
    memset(device_addr_bonded, 0, sizeof(device_addr_bonded));
    has_key[0] = has_key[1] = false;
    scan_active = selected_bonded = false;
    non_flushable = true;
    slot = 1;
    inquiry_result = 0;
    connect_calls = inquiry_calls = inquiry_stops = writes = pin_calls = registered = 0;
    flush_timeout = UINT16_MAX;
    flush_writes = 0;flush_handle = flush_slots = 0;
    command_ready = true;command_result = 0;
    packet_writes=0;packet_handle=packet_mask=0;usable_packet_mask=0xcc18;packet_result=0;
    packet_sync_complete=false;
    consume_command_credit=false;
    radio_writes=0;radio_opcode=radio_handle=0;radio_power_type=radio_result=0;
    ble_ready = false;
    pairing_led = false;
    fail_storage = false;
    ble_pauses = 0;
    now = 0;
    pin_rejections = confirmations = confirmation_rejections = passkey_rejections = 0;
    discoverable = 0;
    security_level = LEVEL_2;
    phone_has_key = missing_acl = tlv_missing = tlv_fail = tlv_corrupt = false;
    phone_writes = 0;
    busy_routes = disconnect_calls = 0;
    busy_during_key_read = false;
    call_sco_allowed = false;
    memset(credit_connections,0,sizeof(credit_connections));
    for (unsigned i = 0; i < 3; ++i) credit_connections[i].address_type = BD_ADDR_TYPE_ACL;
    credit_free_slots=MAX_NR_CONTROLLER_ACL_BUFFERS;credit_packet_reserved=false;
    stored_phone_length = 0;
    memset(&stored_phone, 0, sizeof(stored_phone));
    memset(phone_key_address, 0, sizeof(phone_key_address));
    connection_filter = NULL;
}
static void working(void) {
    uint8_t p[] = {BTSTACK_EVENT_STATE, 1, HCI_STATE_WORKING};
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}
static void complete(void) {
    uint8_t p[] = {GAP_EVENT_INQUIRY_COMPLETE, 1, 0};
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}
static void result(uint8_t *p, unsigned size, const uint8_t *address, uint32_t cod) {
    assert(size >= 27 && size <= 257);
    memset(p, 0, size);
    p[0] = GAP_EVENT_INQUIRY_RESULT;
    p[1] = (uint8_t)(size - 2);
    for (unsigned i = 0; i < 6; ++i) p[2 + i] = address[5 - i];
    p[9] = (uint8_t)cod;
    p[10] = (uint8_t)(cod >> 8);
    p[11] = (uint8_t)(cod >> 16);
    p[16] = 1; // Ordinary existing fixtures represent an Apple USB Device ID.
    p[17] = 2;
    p[19] = 0xac;
    p[20] = 0x05;
}

static void result_name(uint8_t *p, unsigned size, const uint8_t address[6], const char *name) {
    result(p, size, address, 0x340404);
    assert(strlen(name) <= size - 27u);
    p[25] = 1;
    p[26] = (uint8_t)strlen(name);
    memcpy(p + 27, name, strlen(name));
}

static void test_unrelated_speaker_cannot_win_airpods_pairing(void) {
    fixture();
    bt_hci_init();
    working();
    assert(gap_start_scanning());
    uint8_t p[80];
    result_name(p, sizeof(p), nearby, "Power Dynamics");
    p[16] = 0;
    p[14] = 1;
    p[15] = (uint8_t)-97;
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(scan_active && pairing_led && !writes && !connect_calls && !inquiry_stops);
    assert(!peer_address_valid(device_addr));
    result_name(p, sizeof(p), first, "Dylan's earbuds");
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(!scan_active && writes == 1 && connect_calls == 1 && inquiry_stops == 1);
    assert(memcmp(device_addr, first, 6) == 0 && memcmp(saved[0], first, 6) == 0);
}

static void test_pairing_device_id_name_and_authenticated_identity_fallbacks(void) {
    fixture();
    bt_hci_init();
    uint8_t p[80];
    result_name(p, sizeof(p), first, "Dylan's earbuds");
    assert(inquiry_airpods(p, sizeof(p), first));
    p[17] = 1;
    p[19] = 0x4c;
    p[20] = 0;
    assert(inquiry_airpods(p, sizeof(p), first));
    p[17] = 3; // Unknown Device ID namespace cannot reuse either Apple vendor number.
    assert(!inquiry_airpods(p, sizeof(p), first));
    result_name(p, sizeof(p), first, "AirPods Pro");
    p[19] = 0x34;
    p[20] = 0x12;
    assert(!inquiry_airpods(p, sizeof(p), first));
    p[16] = 0;
    assert(inquiry_airpods(p, sizeof(p), first));
    result_name(p, sizeof(p), first, "Dylan's aIrPoDs Max");
    p[16] = 0;
    assert(inquiry_airpods(p, sizeof(p), first));
    result_name(p, sizeof(p), first, "NotAirPods");
    p[16] = 0;
    assert(!inquiry_airpods(p, sizeof(p), first));
    p[26] = 255;
    assert(!inquiry_airpods(p, sizeof(p), first));
    assert(!inquiry_airpods(p, 26, first));

    for (unsigned authenticated = 0; authenticated < 2; ++authenticated) {
        fixture();
        memcpy(saved[0], first, 6);
        has_key[0] = authenticated != 0;
        bt_hci_init();
        result(p, sizeof(p), first, 0x240404);
        p[16] = 0;
        assert(inquiry_airpods(p, sizeof(p), first) == (authenticated != 0));
        assert(!inquiry_airpods(p, sizeof(p), nearby));
        p[16] = 1;
        p[19] = 0x34;
        p[20] = 0x12;
        assert(!inquiry_airpods(p, sizeof(p), first));
    }
}

static void test_retired_pair_has_no_ram_target_but_keeps_authenticated_discovery_hint(void) {
    const uint8_t empty[6] = {0};
    for (unsigned selected = 1; selected <= 2; ++selected) {
        fixture();
        memcpy(saved[0], first, 6); memcpy(saved[1], second, 6);
        has_key[0] = has_key[1] = true; slot = selected;
        bt_hci_init();
        uint8_t previous[6]; memcpy(previous, device_addr, sizeof(previous));
        assert(selected_bonded);
        credit_connections[0] = (hci_connection_t){.con_handle = 1, .state = OPEN, .address_type = BD_ADDR_TYPE_ACL};
        memcpy(credit_connections[0].address, previous, sizeof(previous));
        credit_connections[1] = (hci_connection_t){.con_handle = 2, .state = OPEN, .address_type = BD_ADDR_TYPE_ACL};
        memcpy(credit_connections[1].address, nearby, 6); // The independent phone ACL must remain untouched.
        credit_connections[2] = (hci_connection_t){.con_handle = 3, .state = OPEN, .address_type = BD_ADDR_TYPE_ACL};
        memcpy(credit_connections[2].address, selected == 1 ? second : first, 6);
        assert(selected == 1 ? write_slot1_mac(empty) : write_slot2_mac(empty));
        has_key[selected - 1u] = false; // The caller drops the cleared slot's key before retiring RAM.
        bt_hci_retire_selected_peer();
        assert(!selected_bonded && !memcmp(device_addr, empty, sizeof(empty)));
        assert(disconnect_calls == 1); // The stub accepts only handle 1, belonging to the retired pair.
        assert(credit_connections[1].state == OPEN && credit_connections[2].state == OPEN);
        assert(!strcmp(get_device_addr_string(), "00:00:00:00:00:00"));
        assert(!memcmp(saved[selected - 1u], empty, sizeof(empty)));
        assert(!memcmp(saved[selected == 1 ? 1 : 0], selected == 1 ? second : first, 6));
        assert(device_addr_bonded[0] && device_addr_bonded[1]);
        uint8_t packet[80]; result_name(packet, sizeof(packet), previous, "Dylan's earbuds");
        packet[16] = 0;
        assert(inquiry_airpods(packet, sizeof(packet), previous));
        assert(!inquiry_airpods(packet, sizeof(packet), nearby));
        assert(writes == 1);
    }
    fixture();
    memcpy(saved[0], first, 6); has_key[0] = true;
    bt_hci_init(); missing_acl = true;
    bt_hci_retire_selected_peer();
    assert(!disconnect_calls && !peer_address_valid(device_addr));
    fixture();
    memcpy(saved[0], first, 6); has_key[0] = true;
    bt_hci_init();
    credit_connections[0] = (hci_connection_t){.con_handle = 1, .address_type = BD_ADDR_TYPE_ACL};
    memcpy(credit_connections[0].address, first, 6);
    bt_hci_retire_selected_peer();
    assert(!disconnect_calls && !peer_address_valid(device_addr));
}

static void test_selected_bond_stays_passive_at_boot(void) {
    fixture();
    memcpy(saved[0], first, 6);
    memcpy(saved[1], second, 6);
    has_key[0] = has_key[1] = true;
    slot = 2;
    bt_hci_init();
    assert(registered == 2 && callbacks[0] != callbacks[1]);
    assert(callbacks[0]->callback != callbacks[1]->callback);
    assert(selected_bonded && memcmp(device_addr, second, 6) == 0);
    assert(strcmp(get_device_addr_string(), "20:08:09:0A:0B:0C") == 0);
    assert(get_device_addr_from_list(0) == &device_addr_list[0]);
    assert(get_device_addr_from_list(2) == NULL);
    working();
    assert(connect_calls == 0 && inquiry_calls == 0 && flush_timeout == 200);
    assert(ble_ready && ble_pauses == 0);
    assert(writes == 0);

    fixture();
    memcpy(saved[0], first, 6);
    has_key[0] = true;
    non_flushable = false;
    bt_hci_init();
    working();
    assert(connect_calls == 0 && inquiry_calls == 0 && flush_timeout == 0);
}

static void test_first_boot_can_pair_but_missing_selected_bond_cannot_fallback(void) {
    fixture();
    bt_hci_init();
    working();
    assert(!scan_active && inquiry_calls == 0 && connect_calls == 0);
    complete();assert(inquiry_calls == 0);
    assert(gap_start_scanning()); // Pairing is explicitly requested, never a boot side effect.
    complete();
    assert(inquiry_calls == 2);

    fixture();
    memcpy(saved[0], first, 6);
    memcpy(saved[1], second, 6);
    has_key[1] = true;
    slot = 1;
    bt_hci_init();
    working();
    assert(!selected_bonded && !scan_active && !peer_address_valid(device_addr));
    assert(inquiry_calls == 0 && connect_calls == 0);
    complete();
    assert(inquiry_calls == 0);

    slot = 3;
    get_link_keys();
    working();
    assert(inquiry_calls == 0 && connect_calls == 0 && !selected_bonded);
}

static void test_only_live_pairing_inquiry_can_select_a_peer(void) {
    fixture();
    memcpy(saved[0], first, 6);
    has_key[0] = true;
    bt_hci_init();
    uint8_t p[27];
    result(p, sizeof(p), nearby, 0x240404);
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(writes == 0 && connect_calls == 0 && memcmp(device_addr, first, 6) == 0);

    gap_start_scanning();
    assert(ble_pauses & BLE_SCAN_PAUSE_INQUIRY);
    result(p, sizeof(p), nearby, 0x000540); // Keyboard is not an audio sink.
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(scan_active && writes == 0 && connect_calls == 0);
    result(p, sizeof(p), nearby, 0x240404);
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(!scan_active && writes == 1 && connect_calls == 1 && inquiry_stops == 1);
    assert(!(ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
    assert(memcmp(saved[0], nearby, 6) == 0 && memcmp(device_addr, nearby, 6) == 0);
    result(p, sizeof(p), second, 0x240404);
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(writes == 1 && connect_calls == 1 && memcmp(device_addr, nearby, 6) == 0);

    inquiry_result = 1;
    gap_start_scanning();
    assert(!scan_active);
    assert(!pairing_led);
    assert(!(ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(writes == 1 && connect_calls == 1);
}

static void test_short_events_and_unbounded_names_are_rejected(void) {
    fixture();
    bt_hci_init();
    for (unsigned n = 0; n < 27; ++n) {
        uint8_t p[27] = {GAP_EVENT_INQUIRY_RESULT};
        p[1] = n >= 2 ? (uint8_t)(n - 2) : 0;
        hci_packet_handler(HCI_EVENT_PACKET, 0, p, (uint16_t)n);
        assert(connect_calls == 0 && writes == 0);
    }
    for (unsigned n = 0; n < 8; ++n) {
        uint8_t p[8] = {HCI_EVENT_PIN_CODE_REQUEST};
        p[1] = n >= 2 ? (uint8_t)(n - 2) : 0;
        hci_packet_handler(HCI_EVENT_PACKET, 0, p, (uint16_t)n);
        assert(pin_calls == 0);
    }
    uint8_t state[] = {BTSTACK_EVENT_STATE, 0};
    hci_packet_handler(HCI_EVENT_PACKET, 0, state, sizeof(state));
    packet_handler(HCI_EVENT_PACKET, 0, state, sizeof(state));
    assert(flush_timeout == UINT16_MAX);

    gap_start_scanning();
    uint8_t p[257];
    result(p, sizeof(p), nearby, 0x240404);
    p[25] = 1;
    p[26] = 255;
    memset(p + 27, 'x', sizeof(p) - 27);
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(scan_active && connect_calls == 0 && writes == 0);
    p[26] = sizeof(p) - 27;
    hci_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
    assert(!scan_active && connect_calls == 1 && writes == 1);
}

static void test_controller_off_and_failed_inquiry_release_ble_pause(void) {
    fixture();
    bt_hci_init();
    working();
    assert(gap_start_scanning());
    assert(scan_active && ble_ready && (ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
    inquiry_result = 1;
    unsigned calls = inquiry_calls;
    gap_start_scanning(); // A busy duplicate must not cancel ownership of the live inquiry.
    assert(inquiry_calls == calls && scan_active && (ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
    complete();
    assert(!scan_active && !(ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
    assert(!pairing_led);
    inquiry_result = 0;
    gap_start_scanning();
    assert(scan_active && (ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
    uint8_t state[] = {BTSTACK_EVENT_STATE, 1, 0};
    hci_packet_handler(HCI_EVENT_PACKET, 0, state, sizeof(state));
    assert(!ble_ready && !scan_active && !(ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
    complete();
    assert(!scan_active);
}

static void test_controller_rejection_releases_ble_pause(void) {
    fixture();
    bt_hci_init();
    working();
    assert(gap_start_scanning());
    uint8_t status[] = {HCI_EVENT_COMMAND_STATUS, 4, 0x0c, 1, 1, 4};
    hci_packet_handler(HCI_EVENT_PACKET, 0, status, 5);
    assert(scan_active);
    status[4] = 2; // A different rejected command does not own inquiry.
    hci_packet_handler(HCI_EVENT_PACKET, 0, status, sizeof(status));
    assert(scan_active);
    status[4] = 1;
    status[2] = 0;
    hci_packet_handler(HCI_EVENT_PACKET, 0, status, sizeof(status));
    assert(scan_active);
    status[2] = 0x0c;
    unsigned calls = inquiry_calls;
    hci_packet_handler(HCI_EVENT_PACKET, 0, status, sizeof(status));
    assert(!scan_active && !(ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
    assert(inquiry_calls == calls);
    complete();
    assert(inquiry_calls == calls);
}

static void test_failed_storage_cannot_replace_selected_peer(void) {
    fixture();
    memcpy(saved[0], first, 6);
    has_key[0] = true;
    bt_hci_init();
    gap_start_scanning();
    fail_storage = true;
    uint8_t packet[27];
    result(packet, sizeof(packet), nearby, 0x240404);
    hci_packet_handler(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
    assert(!scan_active && writes == 0 && connect_calls == 0);
    assert(!(ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
    assert(memcmp(device_addr, first, 6) == 0 && memcmp(saved[0], first, 6) == 0);
}

static int allow(const uint8_t address[6], hci_link_type_t type) {
    bd_addr_t copy;
    memcpy(copy, address, 6);
    assert(connection_filter != NULL);
    return connection_filter(copy, type);
}

static void authentication_event(uint8_t type, const uint8_t address[6]) {
    uint8_t packet[12] = {type, 10};
    if (type != HCI_EVENT_USER_CONFIRMATION_REQUEST) packet[1] = 6;
    for (unsigned i = 0; i < 6; ++i) packet[2 + i] = address[5 - i];
    hci_packet_handler(HCI_EVENT_PACKET, 0, packet, packet[1] + 2);
}

static void test_phone_pairing_is_bounded_and_keeps_airpods_slots(void) {
    fixture();
    memcpy(saved[0], first, 6);
    memcpy(saved[1], second, 6);
    has_key[0] = has_key[1] = true;
    bt_hci_init();
    assert(allow(first, HCI_LINK_TYPE_ACL));
    assert(!allow(second, HCI_LINK_TYPE_ACL));
    assert(!allow(nearby, HCI_LINK_TYPE_ACL));
    assert(!bt_hci_phone_peer_allowed(nearby));
    assert(!bt_hci_phone_peer_allowed(NULL));
    uint8_t address[6];
    assert(!bt_hci_phone_address(address) && !bt_hci_phone_address(NULL));

    bt_hci_phone_pairing_start();
    assert(bt_hci_phone_pairing_active() && discoverable == 1);
    assert(!allow(second, HCI_LINK_TYPE_ACL)); // A saved AirPods slot is never a phone.
    assert(!allow(nearby, HCI_LINK_TYPE_SCO)); // Pairing does not authorize call audio.
    assert(allow(nearby, HCI_LINK_TYPE_ACL));
    assert(bt_hci_phone_peer_allowed(nearby));
    const uint8_t another[] = {0x40, 1, 2, 3, 4, 5};
    assert(!allow(another, HCI_LINK_TYPE_ACL)); // Only the first candidate can pair.
    assert(!bt_hci_phone_peer_connected(nearby)); // ACL alone is not a bond.
    memcpy(phone_key_address, nearby, 6);
    phone_has_key = true;
    security_level = 1;
    assert(!bt_hci_phone_peer_connected(nearby)); // A cached key is not encryption.
    security_level = LEVEL_2;
    missing_acl = true;
    assert(!bt_hci_phone_peer_connected(nearby));
    missing_acl = false;
    assert(bt_hci_phone_peer_connected(nearby));
    assert(phone_writes == 1 && writes == 0 && !bt_hci_phone_pairing_active() && discoverable == 0);
    assert(bt_hci_phone_peer_allowed(nearby) && allow(nearby, HCI_LINK_TYPE_ACL));
    assert(bt_hci_phone_peer_connected(nearby) && phone_writes == 1);
    assert(bt_hci_phone_address(address) && memcmp(address, nearby, 6) == 0);
    assert(!allow(nearby, HCI_LINK_TYPE_SCO)); // A matching live HFP service is required too.
    call_sco_allowed = true;
    assert(allow(nearby, HCI_LINK_TYPE_SCO) && allow(nearby, HCI_LINK_TYPE_ESCO));
    assert(!allow(nearby, 3) && !allow(first, HCI_LINK_TYPE_SCO) && !allow(another, HCI_LINK_TYPE_SCO));
    security_level = 1;
    assert(!allow(nearby, HCI_LINK_TYPE_SCO));
    security_level = LEVEL_2;
    missing_acl = true;
    assert(!allow(nearby, HCI_LINK_TYPE_ESCO));
    missing_acl = false;
    call_sco_allowed = false;
    assert(memcmp(saved[0], first, 6) == 0 && memcmp(saved[1], second, 6) == 0);
    get_link_keys();
    assert(memcmp(device_addr, first, 6) == 0 && selected_bonded);

    registered = 0;
    bt_hci_init();
    assert(!bt_hci_phone_pairing_active() && bt_hci_phone_peer_allowed(nearby));
    assert(allow(nearby, HCI_LINK_TYPE_ACL) && memcmp(device_addr, first, 6) == 0);
    phone_has_key = false;
    registered = 0;
    bt_hci_init();
    assert(!bt_hci_phone_peer_allowed(nearby) && !allow(nearby, HCI_LINK_TYPE_ACL));
}

static void test_phone_pairing_expiry_authentication_and_inquiry_roles(void) {
    fixture();
    memcpy(saved[0], first, 6);
    has_key[0] = true;
    bt_hci_init();
    authentication_event(HCI_EVENT_USER_CONFIRMATION_REQUEST, nearby);
    assert(confirmation_rejections == 1 && confirmations == 0);
    authentication_event(HCI_EVENT_USER_CONFIRMATION_REQUEST, first);
    authentication_event(HCI_EVENT_PIN_CODE_REQUEST, first);
    assert(confirmations == 1 && pin_calls == 1);
    bt_hci_phone_pairing_start();
    assert(allow(nearby, HCI_LINK_TYPE_ACL));
    authentication_event(HCI_EVENT_USER_CONFIRMATION_REQUEST, nearby);
    authentication_event(HCI_EVENT_PIN_CODE_REQUEST, nearby);
    authentication_event(HCI_EVENT_USER_PASSKEY_REQUEST, nearby);
    assert(confirmations == 2 && pin_rejections == 1 && passkey_rejections == 1);
    gap_start_scanning();
    uint8_t packet[27];
    result(packet, sizeof(packet), nearby, 0x240404); // Even misleading COD cannot steal a slot.
    hci_packet_handler(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
    assert(scan_active && writes == 0 && memcmp(device_addr, first, 6) == 0);
    now = PHONE_PAIRING_MS;
    assert(!bt_hci_phone_pairing_active() && !bt_hci_phone_peer_allowed(nearby));
    authentication_event(HCI_EVENT_USER_CONFIRMATION_REQUEST, nearby);
    assert(confirmation_rejections == 2);
    bt_hci_phone_poll();
    assert(discoverable == 0 && !peer_address_valid(phone_candidate));
    assert(!allow(nearby, HCI_LINK_TYPE_ACL));
    now = UINT32_MAX - 60000u;
    bt_hci_phone_pairing_start();
    now += PHONE_PAIRING_MS - 1u;
    assert(bt_hci_phone_pairing_active());
    ++now;
    bt_hci_phone_poll();
    assert(!bt_hci_phone_pairing_active() && discoverable == 0);
}

static void test_phone_storage_rejects_unknown_and_failed_writes(void) {
    fixture();
    stored_phone.version = 2;
    memcpy(stored_phone.address, nearby, 6);
    stored_phone_length = sizeof(stored_phone);
    bt_hci_init();
    assert(!bt_hci_phone_pairing_start());
    assert(!bt_hci_phone_pairing_active() && discoverable == 0 && !allow(nearby, HCI_LINK_TYPE_ACL));
    fixture();
    tlv_missing = true;
    bt_hci_init();
    assert(!bt_hci_phone_pairing_start());
    assert(!bt_hci_phone_pairing_active());

    for (unsigned fault = 0; fault < 2; ++fault) {
        fixture();
        memcpy(saved[0], first, 6);
        has_key[0] = true;
        bt_hci_init();
        bt_hci_phone_pairing_start();
        assert(allow(nearby, HCI_LINK_TYPE_ACL));
        memcpy(phone_key_address, nearby, 6);
        phone_has_key = true;
        tlv_fail = fault == 0;
        tlv_corrupt = fault == 1;
        assert(!bt_hci_phone_peer_connected(nearby));
        assert(!phone_bonded && writes == 0 && memcmp(saved[0], first, 6) == 0);
    }
}

static void test_new_phone_pairing_requires_quiet_routes(void) {
    for (unsigned busy = 1; busy <= 32; busy <<= 1) {
        if (busy == 2u) continue; // Idle output transitions have their own pairing-window test.
        fixture();
        bt_hci_init();
        busy_routes = busy;
        assert(bt_hci_phone_pairing_busy());
        assert(!bt_hci_phone_pairing_start());
        assert(!phone_pairing && discoverable == 0);
        assert(!allow(nearby, HCI_LINK_TYPE_ACL));

        busy_routes = 0;
        assert(bt_hci_phone_pairing_start());
        busy_routes = busy;
        assert(!allow(nearby, HCI_LINK_TYPE_ACL)); // Demand can arrive before the first ACL.
        assert(!phone_pairing && discoverable == 0 && disconnect_calls == 0);

        busy_routes = 0;
        assert(bt_hci_phone_pairing_start() && allow(nearby, HCI_LINK_TYPE_ACL));
        memcpy(phone_key_address, nearby, 6);
        phone_has_key = true;
        busy_routes = busy;
        assert(!bt_hci_phone_peer_connected(nearby)); // Nor can a candidate persist after demand returns.
        assert(!phone_writes && !phone_bonded && disconnect_calls == 1);
        assert(!phone_pairing && discoverable == 0);
        authentication_event(HCI_EVENT_USER_CONFIRMATION_REQUEST, nearby);
        assert(confirmations == 0 && confirmation_rejections == 1);
        bt_hci_phone_poll();
        assert(disconnect_calls == 1); // Cancellation is idempotent.

        busy_routes = 0;
        assert(bt_hci_phone_pairing_start() && allow(nearby, HCI_LINK_TYPE_ACL));
        assert(bt_hci_phone_peer_connected(nearby));
        assert(phone_writes == 1 && phone_bonded);
        busy_routes = busy;
        assert(allow(nearby, HCI_LINK_TYPE_ACL)); // A durable phone reconnect needs no flash write.
        assert(bt_hci_phone_peer_connected(nearby) && phone_writes == 1);
        assert(disconnect_calls == 1);
    }
}

static void test_phone_pairing_survives_idle_output_transitions(void) {
    for (unsigned candidate_first = 0; candidate_first < 2; ++candidate_first) {
        fixture();
        bt_hci_init();
        busy_routes = 2;
        assert(bt_hci_phone_pairing_busy() && !bt_hci_phone_pairing_start());
        busy_routes = 0;
        assert(bt_hci_phone_pairing_start());
        if (candidate_first) assert(allow(nearby, HCI_LINK_TYPE_ACL));
        now = 500;
        busy_routes = 2; // Automatic idle suspend/start carries no playback or recording demand.
        bt_hci_phone_poll();
        assert(phone_pairing && bt_hci_phone_pairing_active() && discoverable == 1);
        if (!candidate_first) assert(allow(nearby, HCI_LINK_TYPE_ACL));
        authentication_event(HCI_EVENT_USER_CONFIRMATION_REQUEST, nearby);
        assert(confirmations == 1 && confirmation_rejections == 0);
        memcpy(phone_key_address, nearby, 6);
        phone_has_key = true;
        assert(bt_hci_phone_peer_connected(nearby));
        assert(phone_writes == 1 && phone_bonded && disconnect_calls == 0);
    }
    fixture();
    bt_hci_init();
    assert(bt_hci_phone_pairing_start() && allow(nearby, HCI_LINK_TYPE_ACL));
    busy_routes = 2;
    now = PHONE_PAIRING_MS;
    bt_hci_phone_poll();
    assert(!phone_pairing && discoverable == 0 && disconnect_calls == 1);
}

static void test_phone_candidate_timeout_and_busy_poll_close_only_candidate(void) {
    fixture();
    bt_hci_init();
    assert(bt_hci_phone_pairing_start() && allow(nearby, HCI_LINK_TYPE_ACL));
    now = PHONE_PAIRING_MS;
    bt_hci_phone_poll();
    assert(!phone_pairing && discoverable == 0 && disconnect_calls == 1 && !phone_writes);
    bt_hci_phone_poll();
    assert(disconnect_calls == 1);

    assert(bt_hci_phone_pairing_start() && allow(nearby, HCI_LINK_TYPE_ACL));
    busy_routes = 1;
    bt_hci_phone_poll();
    assert(!phone_pairing && discoverable == 0 && disconnect_calls == 2 && !phone_writes);
    busy_routes = 0;
    assert(bt_hci_phone_pairing_start() && allow(nearby, HCI_LINK_TYPE_ACL));
    missing_acl = true;
    busy_routes = 4;
    bt_hci_phone_poll();
    assert(!phone_pairing && disconnect_calls == 2);

    busy_routes = 0;
    missing_acl = false;
    assert(bt_hci_phone_pairing_start() && allow(nearby, HCI_LINK_TYPE_ACL));
    memcpy(phone_key_address, nearby, 6);
    phone_has_key = busy_during_key_read = true;
    assert(!bt_hci_phone_peer_connected(nearby));
    assert(!phone_pairing && !phone_writes && !phone_bonded && disconnect_calls == 3);
}

static void credit_completion(uint16_t handle,uint16_t packets) {
    uint8_t p[]={HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS,5,1,
        (uint8_t)handle,(uint8_t)(handle>>8),(uint8_t)packets,(uint8_t)(packets>>8)};
    hci_packet_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void test_credit_diagnostics(void) {
    fixture();memcpy(saved[0],first,6);has_key[0]=true;bt_hci_init();
    packet_sync_complete=true; // Flush-only tests acknowledge the unrelated packet policy.
    bt_hci_credit_diagnostics_t data;
    assert(!bt_hci_credit_diagnostics(NULL) && !bt_hci_credit_diagnostics(&data));
    assert(data.raw_acl_count==0 && data.free_acl_slots==0);
    uint8_t buffer[]={HCI_EVENT_COMMAND_COMPLETE,11,1,5,0x10,0,0xfd,3,0,12,0,0,0};
    hci_packet_handler(HCI_EVENT_PACKET,0,buffer,sizeof(buffer));
    assert(bt_hci_credit_diagnostics(&data));
    unsigned limit=12u<MAX_NR_CONTROLLER_ACL_BUFFERS?12u:MAX_NR_CONTROLLER_ACL_BUFFERS;
    assert(data.raw_acl_count==12 && data.raw_acl_length==1021 && data.effective_acl_limit==limit);
    credit_free_slots=1;credit_packet_reserved=true;bt_hci_credit_diagnostics(&data);
    assert(data.free_acl_slots==1 && data.outstanding_acl_packets==limit-1 && data.packet_buffer_reserved==1);
    buffer[5]=1;buffer[9]=99;hci_packet_handler(HCI_EVENT_PACKET,0,buffer,sizeof(buffer));
    buffer[5]=0;buffer[1]=10;hci_packet_handler(HCI_EVENT_PACKET,0,buffer,sizeof(buffer)-1);
    bt_hci_credit_diagnostics(&data);assert(data.raw_acl_count==12); // Failed/truncated replies cannot replace metadata.
    buffer[1]=11;buffer[9]=2;hci_packet_handler(HCI_EVENT_PACKET,0,buffer,sizeof(buffer));
    bt_hci_credit_diagnostics(&data);
    assert(data.raw_acl_count==2 && data.effective_acl_limit==2 && data.outstanding_acl_packets==1);
    memcpy(phone_address,nearby,6);phone_bonded=true;
    credit_connections[0].con_handle=0x11;memcpy(credit_connections[0].address,first,6);
    credit_connections[1].con_handle=0x22;memcpy(credit_connections[1].address,nearby,6);
    credit_connections[2].con_handle=0x33;memcpy(credit_connections[2].address,second,6);
    busy_routes=1;now=100;
    uint8_t both[]={HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS,9,2,0x11,0,1,0,0x22,0,2,0};
    hci_packet_handler(HCI_EVENT_PACKET,0,both,sizeof(both));
    now=120;credit_completion(0x11,1);credit_completion(0x22,1);
    credit_completion(0x33,7);credit_completion(0x77,9);
    uint8_t malformed[]={HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS,5,2,0x11,0,1,0};
    hci_packet_handler(HCI_EVENT_PACKET,0,malformed,sizeof(malformed));
    bt_hci_credit_diagnostics(&data);
    assert(data.airpods.events==2 && data.airpods.packets==2 && data.airpods.max_active_gap_ms==20);
    assert(data.phone.events==2 && data.phone.packets==3 && data.phone.max_active_gap_ms==20);
    memcpy(credit_connections[2].address, nearby, 6);
    credit_connections[2].address_type = BD_ADDR_TYPE_SCO;
    credit_completion(0x33, 10);
    bt_hci_credit_diagnostics(&data);
    assert(data.phone.events==2 && data.phone.packets==3); // Voice credits are not ACL delivery evidence.
    busy_routes=0;bt_hci_phone_poll();now=5000;credit_completion(0x11,1);
    busy_routes=1;now=9000;credit_completion(0x11,1);now=9025;credit_completion(0x11,1);
    bt_hci_credit_diagnostics(&data);assert(data.airpods.max_active_gap_ms==25); // Idle duration is excluded.
    uint8_t disconnect[]={HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0x11,0,0x13};
    hci_packet_handler(HCI_EVENT_PACKET,0,disconnect,sizeof(disconnect));
    now=20000;credit_completion(0x11,1);bt_hci_credit_diagnostics(&data);
    assert(data.airpods.max_active_gap_ms==25); // Reconnection is a new gap epoch.
    credit_connections[0].con_handle=0x44;now=30000;credit_completion(0x44,1);
    bt_hci_credit_diagnostics(&data);assert(data.airpods.max_active_gap_ms==25);
    busy_routes=0;bt_hci_phone_poll();busy_routes=1;
    now=UINT32_MAX-9u;credit_completion(0x44,1);now=5;credit_completion(0x44,1);
    bt_hci_credit_diagnostics(&data);assert(data.airpods.max_active_gap_ms==25);
    credit_roles[0].totals.events=UINT32_MAX;credit_roles[0].totals.packets=UINT32_MAX-1u;
    credit_completion(0x44,2);bt_hci_credit_diagnostics(&data);
    assert(data.airpods.events==UINT32_MAX && data.airpods.packets==UINT32_MAX);
}

static void test_authenticated_phone_acl_session(void) {
    fixture();bt_hci_init();
    memcpy(phone_address,nearby,6);memcpy(phone_key_address,nearby,6);
    phone_bonded=phone_has_key=true;
    credit_connections[0]=(hci_connection_t){.con_handle=1,.address_type=BD_ADDR_TYPE_ACL,.state=OPEN};
    memcpy(credit_connections[0].address,nearby,6);
    uint32_t session=bt_hci_phone_session(nearby);
    assert(session!=0 && bt_hci_phone_session(nearby)==session);
    assert(!bt_hci_phone_session(first));
    security_level=1;assert(!bt_hci_phone_session(nearby));security_level=LEVEL_2;
    missing_acl=true;assert(!bt_hci_phone_session(nearby));missing_acl=false;
    credit_connections[0].state=0;assert(!bt_hci_phone_session(nearby));
    credit_connections[0].state=OPEN;
    assert(bt_hci_phone_session(nearby)==session);

    uint8_t disconnect[]={HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0x11,0,0x13};
    hci_packet_handler(HCI_EVENT_PACKET,0,disconnect,sizeof(disconnect));
    assert(bt_hci_phone_session(nearby)==session); // The AirPods ACL is independent.
    disconnect[3]=1;disconnect[2]=1;
    hci_packet_handler(HCI_EVENT_PACKET,0,disconnect,sizeof(disconnect));
    assert(bt_hci_phone_session(nearby)==session); // A failed disconnect did not end this session.
    disconnect[2]=0;
    hci_packet_handler(HCI_EVENT_PACKET,0,disconnect,sizeof(disconnect));
    // BTstack can reuse this handle/address before main polls after the event.
    uint32_t reconnected=bt_hci_phone_session(nearby);
    assert(reconnected!=0 && reconnected!=session);
    session=reconnected;
    uint8_t off[]={BTSTACK_EVENT_STATE,1,0};
    hci_packet_handler(HCI_EVENT_PACKET,0,off,sizeof(off));
    reconnected=bt_hci_phone_session(nearby);
    assert(reconnected!=0 && reconnected!=session);

    memcpy(phone_address,second,6);
    assert(!bt_hci_phone_session(nearby));
    memcpy(phone_key_address,second,6);memcpy(credit_connections[0].address,second,6);
    session=bt_hci_phone_session(second);
    assert(session!=0 && session!=reconnected);
    phone_session_generation=UINT32_MAX;phone_session_handle=HCI_CON_HANDLE_INVALID;
    assert(bt_hci_phone_session(second)==1); // Zero is reserved for no authenticated connection.
}

static void test_current_link_roles(void) {
    fixture();
    bt_hci_credit_diagnostics_t data;
    assert(!bt_hci_credit_diagnostics(&data));
    assert(data.airpods_handle==HCI_CON_HANDLE_INVALID && data.phone_handle==HCI_CON_HANDLE_INVALID);
    assert(data.airpods_local_role==UINT8_MAX && data.phone_local_role==UINT8_MAX);
    memcpy(device_addr,first,6);memcpy(phone_address,nearby,6);
    credit_connections[0]=(hci_connection_t){.con_handle=0x11,.address_type=BD_ADDR_TYPE_ACL,
        .state=OPEN,.role=HCI_ROLE_MASTER};
    credit_connections[1]=(hci_connection_t){.con_handle=0x22,.address_type=BD_ADDR_TYPE_ACL,
        .state=OPEN,.role=HCI_ROLE_SLAVE};
    memcpy(credit_connections[0].address,first,6);memcpy(credit_connections[1].address,nearby,6);
    bt_hci_credit_diagnostics(&data);
    assert(data.airpods_handle==0x11 && data.airpods_local_role==HCI_ROLE_MASTER);
    assert(data.phone_handle==0x22 && data.phone_local_role==HCI_ROLE_SLAVE);
    credit_connections[1].role=HCI_ROLE_MASTER;
    bt_hci_credit_diagnostics(&data);assert(data.phone_local_role==HCI_ROLE_MASTER);
    assert(!data.airpods.events && !data.phone.events); // Role reads need no new completion event.
    credit_connections[0].state=0;
    credit_connections[1].address_type=BD_ADDR_TYPE_SCO;
    bt_hci_credit_diagnostics(&data);
    assert(data.airpods_handle==HCI_CON_HANDLE_INVALID && data.phone_handle==HCI_CON_HANDLE_INVALID);
    assert(data.airpods_local_role==UINT8_MAX && data.phone_local_role==UINT8_MAX);
    credit_connections[1].address_type=BD_ADDR_TYPE_ACL;
    memcpy(phone_candidate,phone_address,6);memset(phone_address,0,6);
    bt_hci_credit_diagnostics(&data);assert(data.phone_handle==0x22 && data.phone_local_role==HCI_ROLE_MASTER);
    missing_acl=true;bt_hci_credit_diagnostics(&data);
    assert(data.phone_handle==HCI_CON_HANDLE_INVALID && data.phone_local_role==UINT8_MAX);
}

static void flush_ack(uint16_t handle, uint8_t status) {
    uint8_t p[] = {HCI_EVENT_COMMAND_COMPLETE,6,1,0x28,0x0c,status,
        (uint8_t)handle,(uint8_t)(handle >> 8)};
    hci_packet_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void packet_status(uint8_t status) {
    uint8_t p[]={HCI_EVENT_COMMAND_STATUS,4,status,1,0x0f,0x04};
    hci_packet_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void packet_changed(uint16_t handle,uint16_t mask,uint8_t status) {
    uint8_t p[]={HCI_EVENT_CONNECTION_PACKET_TYPE_CHANGED,5,status,(uint8_t)handle,(uint8_t)(handle>>8),
        (uint8_t)mask,(uint8_t)(mask>>8)};
    hci_packet_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void flush_fixture(void) {
    fixture();memcpy(saved[0],first,6);has_key[0]=true;bt_hci_init();
    packet_sync_complete=true; // Flush-only tests acknowledge the unrelated packet policy.
    credit_connections[0] = (hci_connection_t){.con_handle=0x11,.address_type=BD_ADDR_TYPE_ACL,.state=OPEN};
    credit_connections[1] = (hci_connection_t){.con_handle=0x22,.address_type=BD_ADDR_TYPE_ACL,.state=OPEN};
    memcpy(credit_connections[0].address,first,6);memcpy(credit_connections[1].address,nearby,6);
}
static void flush_dirty_timeout(void) {
    assert(media_flush.confirmed_ms==200);
    media_flush.confirmed_ms=60;
    for(unsigned i=0;i<2;++i)if(media_flush_links[i].handle==media_flush.handle)
        media_flush_links[i].confirmed_ms=60;
}
static void test_media_flush_game_retains_baseline_and_restores_dirty_timeout(void) {
    flush_fixture();
    for(unsigned i=0;i<10;++i) {
        bt_hci_airpods_flush_timeout(i&1);bt_hci_phone_poll();
        assert(!flush_writes && !media_flush.confirmed_ms);
    }
    flush_ack(0x11,0);
    for(unsigned i=0;i<10;++i) {
        bt_hci_airpods_flush_timeout(i&1);bt_hci_phone_poll();
        assert(!flush_writes && media_flush.desired_ms==200 && media_flush.confirmed_ms==200);
        assert(!media_flush.comparison && media_packet_game==(bool)(i&1));
        assert(!bt_hci_airpods_flush_comparison(true)); // No legacy short timeout to compare.
    }
    flush_dirty_timeout();
    bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();
    assert(flush_writes==1 && flush_slots==320 && media_flush_pending.active);
    bt_hci_airpods_flush_timeout(false);bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();
    assert(flush_writes==1); // A real pending restore keeps its command/ACK fencing.
    flush_ack(0x11,0);bt_hci_phone_poll();
    assert(media_flush.confirmed_ms==200 && !media_flush_pending.active && flush_writes==1);
}
static void test_media_flush_targets_only_airpods_and_confirms_ack(void) {
    flush_fixture();
    bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();
    assert(!flush_writes); // BTstack's initial 200-ms write must finish first.
    flush_ack(0x22,0);bt_hci_phone_poll();assert(!flush_writes);
    flush_ack(0x11,0);flush_dirty_timeout();
    command_ready=false;bt_hci_phone_poll();assert(!flush_writes);
    command_ready=true;bt_hci_phone_poll();
    assert(flush_writes==1 && flush_handle==0x11 && flush_slots==320 && media_flush_pending.active);
    assert(media_flush.confirmed_ms==60);
    bt_hci_phone_poll();bt_hci_airpods_flush_timeout(true);assert(flush_writes==1);
    flush_ack(0x22,0);assert(media_flush_pending.active && media_flush.confirmed_ms==60);
    uint8_t short_ack[]={HCI_EVENT_COMMAND_COMPLETE,5,1,0x28,0x0c,0,0x11};
    hci_packet_handler(HCI_EVENT_PACKET,0,short_ack,sizeof(short_ack));assert(media_flush_pending.active);
    flush_ack(0x11,0);assert(!media_flush_pending.active && media_flush.confirmed_ms==200);
    bt_hci_airpods_flush_timeout(false);bt_hci_phone_poll();
    assert(flush_writes==1 && flush_timeout==UINT16_MAX); // No phone-ACL/global-policy mutation.

    flush_fixture();non_flushable=false;
    bt_hci_airpods_flush_timeout(true);flush_ack(0x11,0);bt_hci_phone_poll();assert(!flush_writes);
}
static void test_media_flush_failures_and_superseded_commands_are_bounded(void) {
    flush_fixture();flush_ack(0x11,0);flush_dirty_timeout();bt_hci_airpods_flush_timeout(true);
    command_result=0x0c;bt_hci_phone_poll();
    assert(flush_writes==1 && media_flush.failed && !media_flush_pending.active);
    for(unsigned i=0;i<100;++i)bt_hci_phone_poll();
    assert(flush_writes==1 && media_flush.confirmed_ms==60);
    bt_hci_airpods_flush_timeout(false);bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();
    assert(flush_writes==1 && media_flush.failed); // Mode changes do not retry a failed controller command.

    flush_fixture();flush_ack(0x11,0);flush_dirty_timeout();bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();
    flush_ack(0x11,0x0c);assert(media_flush.failed && media_flush.confirmed_ms==60);
    bt_hci_phone_poll();assert(flush_writes==1);

    flush_fixture();flush_ack(0x11,0);flush_dirty_timeout();bt_hci_airpods_flush_timeout(true);
    now=UINT32_MAX-499u;bt_hci_phone_poll();assert(flush_writes==1);
    now=500;bt_hci_phone_poll();
    assert(media_flush.failed && media_flush_pending.active && media_flush.status==UINT8_MAX);
    bt_hci_airpods_flush_timeout(false);bt_hci_phone_poll();assert(flush_writes==1);
    flush_ack(0x11,0);assert(media_flush.confirmed_ms==200 && !media_flush.failed);
    bt_hci_phone_poll();assert(flush_writes==1);

    flush_fixture();bt_hci_airpods_flush_timeout(true);now=1000;bt_hci_phone_poll();
    assert(!flush_writes && media_flush.failed && !media_flush.confirmed_ms);
    flush_ack(0x11,0);bt_hci_phone_poll();assert(!flush_writes && !media_flush.failed);
    bt_hci_airpods_flush_timeout(false);flush_ack(0x11,0x0c);bt_hci_phone_poll();
    assert(!flush_writes && !media_flush.failed && media_flush.confirmed_ms==200);
}
static void test_media_flush_reconnect_ack_cannot_confirm_new_epoch(void) {
    flush_fixture();flush_ack(0x11,0);flush_dirty_timeout();bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();
    uint32_t epoch=media_flush.epoch;
    uint8_t disconnected[]={HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0x11,0,0x13};
    hci_packet_handler(HCI_EVENT_PACKET,0,disconnected,sizeof(disconnected));
    bt_hci_airpods_flush_timeout(true);
    assert(media_flush.epoch!=epoch && media_flush.confirmed_ms==0);
    bt_hci_phone_poll();assert(flush_writes==1);
    flush_ack(0x11,0);assert(media_flush.confirmed_ms==0 && !media_flush_pending.active);
    bt_hci_phone_poll();assert(flush_writes==1);
    flush_ack(0x11,0);bt_hci_phone_poll(); // The new epoch learns only its SDK startup baseline.
    assert(flush_writes==1 && media_flush.confirmed_ms==200);
    uint8_t off[]={BTSTACK_EVENT_STATE,1,0};hci_packet_handler(HCI_EVENT_PACKET,0,off,sizeof(off));
    assert(!media_flush_pending.active && media_flush.handle==HCI_CON_HANDLE_INVALID);
}
static void test_media_flush_selected_peer_remembers_confirmed_value(void) {
    flush_fixture();memcpy(saved[1],second,6);memcpy(device_addr_list[1],second,6);
    credit_connections[2]=(hci_connection_t){.con_handle=0x33,.address_type=BD_ADDR_TYPE_ACL,.state=OPEN};
    memcpy(credit_connections[2].address,second,6);
    flush_ack(0x11,0);flush_ack(0x33,0);flush_dirty_timeout();
    bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();assert(flush_writes==1);
    memcpy(device_addr,second,6);bt_hci_airpods_flush_timeout(false);bt_hci_phone_poll();
    assert(flush_writes==1 && media_flush.handle==0x33 && media_flush.confirmed_ms==200);
    flush_ack(0x11,0);assert(media_flush.confirmed_ms==200); // ACK updates only its own ACL record.
    memcpy(device_addr,first,6);bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();
    assert(flush_writes==1 && media_flush.confirmed_ms==200);
    flush_dirty_timeout();bt_hci_phone_poll();
    assert(flush_writes==2 && flush_handle==0x11 && flush_slots==320 && media_flush.confirmed_ms==60);
    flush_ack(0x11,0);assert(media_flush.confirmed_ms==200);
}
static void flush_game_fixture(void) {
    flush_fixture();flush_ack(0x11,0);bt_hci_airpods_flush_timeout(true);flush_dirty_timeout();
    assert(media_flush.confirmed_ms==60 && !media_flush_pending.active);
}
static void test_media_flush_comparison_is_bounded_and_only_changes_timeout(void) {
    flush_fixture();assert(!bt_hci_airpods_flush_comparison(true));
    flush_ack(0x11,0);bt_hci_airpods_flush_timeout(true);
    assert(!bt_hci_airpods_flush_comparison(true)); // Normal baseline already matches the legacy diagnostic.
    flush_dirty_timeout();assert(bt_hci_airpods_flush_comparison(true));bt_hci_phone_poll();
    assert(media_flush.comparison && flush_handle==0x11 && flush_slots==320 && flush_writes==1);
    flush_ack(0x11,0);assert(media_flush.confirmed_ms==200 && bt_hci_airpods_flush_comparison(true));
    for(unsigned i=1;i<120;++i) {
        now=i*1000u;bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();
        assert(media_flush.desired_ms==200 && flush_writes==1);
    }
    now=119999;assert(bt_hci_airpods_flush_comparison(true));now=120000;bt_hci_phone_poll();
    assert(!media_flush.comparison && media_flush.desired_ms==200 && flush_writes==1);
    assert(inquiry_calls==0 && disconnect_calls==0 && flush_timeout==UINT16_MAX);

    flush_dirty_timeout();now=UINT32_MAX-59999u;
    assert(bt_hci_airpods_flush_comparison(true));bt_hci_phone_poll();flush_ack(0x11,0);
    now=59999u;bt_hci_phone_poll();assert(media_flush.comparison && media_flush.desired_ms==200);
    now=60000u;bt_hci_phone_poll();assert(!media_flush.comparison && media_flush.desired_ms==200);
}
static void test_media_flush_comparison_clears_on_cancel_scope_loss_and_stale_ack(void) {
    flush_game_fixture();assert(bt_hci_airpods_flush_comparison(true));bt_hci_phone_poll();
    assert(bt_hci_airpods_flush_comparison(false));bt_hci_phone_poll();
    assert(!media_flush.comparison && media_flush.desired_ms==200 && flush_writes==1 && flush_slots==320);
    flush_ack(0x11,0);assert(media_flush.confirmed_ms==200);

    flush_game_fixture();assert(bt_hci_airpods_flush_comparison(true));bt_hci_phone_poll();
    bt_hci_airpods_flush_timeout(false);
    assert(!media_flush.comparison && media_flush.desired_ms==200);
    flush_ack(0x11,0);bt_hci_phone_poll();assert(media_flush.confirmed_ms==200 && flush_writes==1);
    assert(!bt_hci_airpods_flush_comparison(true));

    flush_game_fixture();assert(bt_hci_airpods_flush_comparison(true));now=119500;bt_hci_phone_poll();
    now=120000;bt_hci_phone_poll();assert(!media_flush.comparison && media_flush_pending.active);
    flush_ack(0x11,0);bt_hci_phone_poll();assert(media_flush.confirmed_ms==200 && flush_writes==1);

    flush_game_fixture();assert(bt_hci_airpods_flush_comparison(true));bt_hci_phone_poll();
    uint8_t disconnected[]={HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0x11,0,0x13};
    hci_packet_handler(HCI_EVENT_PACKET,0,disconnected,sizeof(disconnected));
    assert(!media_flush.comparison && !media_flush.low_latency);flush_ack(0x11,0);assert(!media_flush.confirmed_ms);

    flush_game_fixture();assert(bt_hci_airpods_flush_comparison(true));
    memcpy(device_addr,second,6);bt_hci_airpods_flush_timeout(true);
    assert(!media_flush.comparison && media_flush.handle==HCI_CON_HANDLE_INVALID);

    flush_game_fixture();assert(bt_hci_airpods_flush_comparison(true));
    uint8_t off[]={BTSTACK_EVENT_STATE,1,0};hci_packet_handler(HCI_EVENT_PACKET,0,off,sizeof(off));
    assert(!media_flush.comparison && !media_flush.low_latency);
    flush_game_fixture();non_flushable=false;assert(!bt_hci_airpods_flush_comparison(true));
}

static void packet_fixture(void) {
    flush_fixture();packet_sync_complete=false;flush_ack(0x11,0);bt_hci_airpods_flush_timeout(false);bt_hci_phone_poll();
    assert(packet_writes==1 && packet_handle==0x11 && packet_mask==0xcc18 && media_packet_pending.active);
}
static void packet_normal_fixture(void) {
    packet_fixture();packet_status(0);packet_changed(0x11,0xcc18,0);bt_hci_phone_poll();
    assert(media_packets[0].baseline && media_packets[0].normal==0xcc18 && packet_writes==1);
}
static void packet_game_fixture(void) {
    packet_normal_fixture();bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();flush_ack(0x11,0);
    assert(packet_writes==2 && packet_mask==0xee1c);
    packet_status(0);packet_changed(0x11,0xee1c,0);
}
static void test_media_packet_baseline_game_and_normal_are_confirmed_per_acl(void) {
    flush_fixture();packet_sync_complete=false;flush_ack(0x11,0);command_ready=false;bt_hci_phone_poll();
    assert(!packet_writes && media_packets[0].normal==UINT16_MAX);
    command_ready=true;bt_hci_phone_poll();
    assert(packet_writes==1 && packet_handle==0x11 && packet_mask==0xcc18);
    for(unsigned i=0;i<10;++i)bt_hci_phone_poll();
    assert(packet_writes==1 && media_packet_pending.active);
    packet_changed(0x11,0xcc18,0); // A result cannot stand in for the missing Command Status.
    assert(!media_packets[0].baseline && media_packet_pending.active);
    packet_status(0);bt_hci_phone_poll();
    assert(media_packet_pending.active && !media_packets[0].baseline);
    packet_changed(0x22,0xcc18,0);assert(media_packet_pending.active);
    uint8_t short_result[]={HCI_EVENT_CONNECTION_PACKET_TYPE_CHANGED,4,0,0x11,0,0x18};
    hci_packet_handler(HCI_EVENT_PACKET,0,short_result,sizeof(short_result));
    assert(media_packet_pending.active);
    packet_changed(0x11,0xcc18,0);bt_hci_phone_poll();
    assert(media_packets[0].baseline && media_packets[0].normal==0xcc18 && !media_packet_pending.active && packet_writes==1);
    bt_hci_credit_diagnostics_t data;bt_hci_credit_diagnostics(&data);
    assert(data.media_packet_requested==0xcc18 && data.media_packet_normal==0xcc18 &&
        data.media_packet_confirmed==0xcc18 && !data.media_packet_failed && !data.media_packet_pending);

    bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();flush_ack(0x11,0);
    assert(packet_writes==2 && packet_handle==0x11 && packet_mask==0xee1c);
    packet_status(0);assert(media_packets[0].confirmed==0xcc18);
    packet_changed(0x11,0xee1c,0);
    for(unsigned i=0;i<10;++i){bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();}
    assert(packet_writes==2 && media_packets[0].confirmed==0xee1c);
    bt_hci_airpods_flush_timeout(false);bt_hci_phone_poll();
    assert(packet_writes==3 && packet_handle==0x11 && packet_mask==0xcc18);
    packet_status(0);packet_changed(0x11,0xcc18,0);bt_hci_phone_poll();
    assert(packet_writes==3 && media_packets[0].confirmed==0xcc18);
}
static void test_media_packet_baseline_clamp_polarity_dm1_and_reserved_bits(void) {
    packet_fixture();packet_status(0);packet_changed(0x11,0xc818,0);
    assert(media_packets[0].baseline && media_packets[0].normal==0xc818);
    bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();assert(packet_mask==0xea1c);
    packet_status(0);packet_changed(0x11,0xea1c,0);
    bt_hci_airpods_flush_timeout(false);bt_hci_phone_poll();assert(packet_mask==0xc818);
    packet_status(0);packet_changed(0x11,0xc818,0);bt_hci_phone_poll();assert(packet_writes==3);

    packet_fixture();packet_status(0);packet_changed(0x11,0xee1c,0); // A narrower EDR baseline is safe.
    bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();
    assert(media_packets[0].normal==0xee1c && packet_writes==1);

    flush_fixture();packet_sync_complete=false;usable_packet_mask=0xee1c;bt_hci_phone_poll();assert(packet_mask==0xee1c);
    packet_status(0);packet_changed(0x11,0xcc18,0); // Clearing negative EDR bits would broaden permissions.
    assert(media_packets[0].failed && !media_packets[0].baseline && media_packets[0].confirmed==0xcc18);
    for(unsigned i=0;i<10;++i)bt_hci_phone_poll();
    assert(packet_writes==1);

    packet_fixture();packet_status(0);packet_changed(0x11,0xcc19,0);
    assert(media_packets[0].failed && !media_packets[0].baseline);
    for(unsigned i=0;i<10;++i)bt_hci_phone_poll();
    assert(packet_writes==1);

    packet_fixture();packet_status(0);packet_changed(0x11,0xcc10,0);
    assert(media_packets[0].baseline && media_packets[0].normal==0xcc10);
    bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();assert(packet_mask==0xee14);
    packet_status(0);packet_changed(0x11,0xee1c,0);bt_hci_phone_poll();assert(packet_writes==2);
    bt_hci_airpods_flush_timeout(false);bt_hci_phone_poll();assert(packet_mask==0xcc10);
    packet_status(0);packet_changed(0x11,0xcc18,0);bt_hci_phone_poll();
    assert(packet_writes==3 && !media_packets[0].failed);
}
static void test_media_packet_command_credit_preserves_native_policy_order(void) {
    packet_normal_fixture();consume_command_credit=true;flush_dirty_timeout();
    bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();
    assert(packet_writes==2 && packet_mask==0xee1c && !command_ready && !flush_writes);
    for(unsigned i=0;i<10;++i)bt_hci_phone_poll();
    assert(packet_writes==2 && !flush_writes);
    packet_status(0);command_ready=true;bt_hci_phone_poll();
    assert(media_packet_pending.active && flush_writes==1 && flush_slots==320 && !command_ready);
    flush_ack(0x11,0);command_ready=true;packet_changed(0x11,0xee1c,0);bt_hci_phone_poll();
    assert(packet_writes==2 && flush_writes==1 && !media_packet_pending.active);
    flush_dirty_timeout();bt_hci_airpods_flush_timeout(false);bt_hci_phone_poll();
    assert(packet_writes==3 && packet_mask==0xcc18 && flush_writes==1 && !command_ready);
    packet_status(0);command_ready=true;bt_hci_phone_poll();
    assert(flush_writes==2 && flush_slots==320 && media_packet_pending.active);
    flush_ack(0x11,0);command_ready=true;packet_changed(0x11,0xcc18,0);bt_hci_phone_poll();
    assert(!media_packet_pending.active && packet_writes==3 && flush_writes==2);
}
static void test_media_packet_failures_are_bounded_and_status_is_not_completion(void) {
    flush_fixture();packet_sync_complete=false;packet_result=0x0c;bt_hci_phone_poll();
    assert(packet_writes==1 && !media_packet_pending.active && media_packets[0].failed);
    for(unsigned i=0;i<10;++i)bt_hci_phone_poll();
    assert(packet_writes==1);
    packet_fixture();packet_status(0x0c);
    assert(!media_packet_pending.active && media_packets[0].failed && media_packets[0].status==0x0c);
    packet_fixture();packet_status(0);packet_changed(0x11,0xcc18,0x0c);
    assert(!media_packet_pending.active && media_packets[0].failed && !media_packets[0].baseline);

    packet_normal_fixture();bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();
    packet_status(0);packet_changed(0x11,0xcc18,0); // Success with a different meaningful mask is not confirmation.
    assert(media_packets[0].failed && media_packets[0].confirmed==0xcc18);
    for(unsigned i=0;i<10;++i)bt_hci_phone_poll();
    assert(packet_writes==2);
    bt_hci_airpods_flush_timeout(false);bt_hci_phone_poll();
    assert(!media_packets[0].failed && packet_writes==2); // Already at the saved normal mask.

    packet_game_fixture();bt_hci_airpods_flush_timeout(false);bt_hci_phone_poll();
    packet_status(0);packet_changed(0x11,0xee1c,0);
    assert(media_packets[0].failed && media_packets[0].confirmed==0xee1c);
    for(unsigned i=0;i<10;++i)bt_hci_phone_poll();
    assert(packet_writes==3);

    flush_fixture();packet_sync_complete=false;now=UINT32_MAX-499u;bt_hci_phone_poll();
    now=499;bt_hci_phone_poll();assert(!media_packets[0].failed);
    now=500;bt_hci_phone_poll();assert(media_packets[0].failed && media_packet_pending.active);
    uint8_t short_status[]={HCI_EVENT_COMMAND_STATUS,3,0,1,0x0f};
    hci_packet_handler(HCI_EVENT_PACKET,0,short_status,sizeof(short_status));
    uint8_t unrelated[]={HCI_EVENT_COMMAND_STATUS,4,0,1,0x28,0x0c};
    hci_packet_handler(HCI_EVENT_PACKET,0,unrelated,sizeof(unrelated));
    assert(!media_packet_pending.accepted);
    packet_status(0);for(unsigned i=0;i<10;++i)bt_hci_phone_poll();
    assert(media_packet_pending.active && media_packets[0].failed && packet_writes==1);
    packet_changed(0x11,0xcc18,0);bt_hci_phone_poll();
    assert(!media_packet_pending.active && !media_packets[0].failed && media_packets[0].baseline && packet_writes==1);

    flush_fixture();packet_sync_complete=true;bt_hci_phone_poll();
    assert(!media_packet_pending.active && media_packets[0].baseline && packet_writes==1);
}
static void test_media_packet_old_peer_restore_phone_exclusion_and_handle_reuse(void) {
    packet_game_fixture();memcpy(saved[1],second,6);memcpy(device_addr_list[1],second,6);
    credit_connections[2]=(hci_connection_t){.con_handle=0x33,.address_type=BD_ADDR_TYPE_ACL,.state=OPEN};
    memcpy(credit_connections[2].address,second,6);memcpy(device_addr,second,6);
    bt_hci_airpods_flush_timeout(false);bt_hci_phone_poll();
    assert(packet_writes==3 && packet_handle==0x11 && packet_mask==0xcc18); // Restore the old live AirPods ACL.
    packet_status(0);packet_changed(0x11,0xcc18,0);bt_hci_phone_poll();
    assert(packet_writes==4 && packet_handle==0x33 && packet_mask==0xcc18);
    packet_status(0);packet_changed(0x33,0xcc18,0);bt_hci_phone_poll();assert(packet_writes==4);
    assert(media_packet_index(0x22)<0); // The independent phone ACL is never a policy target.

    packet_fixture();uint32_t old_epoch=media_packets[0].epoch;
    uint8_t disconnected[]={HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0x11,0,0x13};
    hci_packet_handler(HCI_EVENT_PACKET,0,disconnected,sizeof(disconnected));
    bt_hci_phone_poll(); // The fixture reuses the same open handle/address immediately.
    assert(media_packets[0].epoch!=old_epoch && !media_packets[0].baseline && packet_writes==1);
    packet_status(0);packet_changed(0x11,0xcc18,0);
    assert(!media_packet_pending.active && !media_packets[0].baseline && media_packets[0].confirmed==UINT16_MAX);
    bt_hci_phone_poll();assert(packet_writes==2 && media_packet_pending.active);
    packet_changed(0x11,0xcc18,0);assert(!media_packets[0].baseline);
    packet_status(0);packet_changed(0x11,0xcc18,0);assert(media_packets[0].baseline);

    packet_fixture();memcpy(device_addr,second,6);credit_connections[2]=(hci_connection_t){
        .con_handle=0x33,.address_type=BD_ADDR_TYPE_ACL,.state=OPEN};memcpy(credit_connections[2].address,second,6);
    bt_hci_phone_poll();assert(packet_writes==1 && media_packet_pending.active);
    packet_status(0x0c);bt_hci_phone_poll();assert(packet_writes==2 && packet_handle==0x33);
    uint8_t off[]={BTSTACK_EVENT_STATE,1,0};hci_packet_handler(HCI_EVENT_PACKET,0,off,sizeof(off));
    assert(!media_packet_pending.active && media_packets[0].handle==HCI_CON_HANDLE_INVALID &&
        media_packets[1].handle==HCI_CON_HANDLE_INVALID);
}

static void radio_reply(uint16_t opcode,uint16_t handle,uint8_t value,uint8_t status) {
    uint8_t p[19]={HCI_EVENT_COMMAND_COMPLETE,7,1,(uint8_t)opcode,(uint8_t)(opcode>>8),status,
        (uint8_t)handle,(uint8_t)(handle>>8),value};
    if(opcode==0x1406) {p[1]=17;memset(p+9,0xff,9);p[18]=0x7f;}
    hci_packet_handler(HCI_EVENT_PACKET,0,p,opcode==0x1406?19:9);
}
static void radio_fixture(void) {
    packet_normal_fixture();bt_hci_radio_active(true);bt_hci_phone_poll();
    assert(radio_writes==1 && radio_opcode==0x1405 && radio_handle==0x11);
}
static void test_radio_sampling_is_sparse_read_only_and_cached(void) {
    assert(sizeof(radio)+sizeof(radio_pending)+sizeof(radio_epoch)<=128);
    radio_fixture();radio_reply(radio_opcode,0x11,(uint8_t)-12,0);
    bt_hci_phone_poll();assert(radio_writes==2 && radio_opcode==0x1403);radio_reply(radio_opcode,0x11,201,0);
    bt_hci_phone_poll();assert(radio_writes==3 && radio_opcode==0x0c2d && !radio_power_type);radio_reply(radio_opcode,0x11,4,0);
    bt_hci_phone_poll();assert(radio_writes==4 && radio_opcode==0x0c2d && radio_power_type==1);radio_reply(radio_opcode,0x11,10,0);
    bt_hci_phone_poll();assert(radio_writes==5 && radio_opcode==0x1406);radio_reply(radio_opcode,0x11,1,0);
    bt_hci_radio_diagnostics_t out;assert(bt_hci_radio_diagnostics(&out));
    assert(out.valid==31 && !out.failed && !out.unavailable && out.active && !out.pending && out.rssi==-12 &&
        out.quality==201 && out.tx_power==4 && out.max_tx_power==10 && out.afh_mode==1 && out.afh_map[9]==0x7f);
    for(unsigned i=0;i<5;++i)assert(out.age_ms[i]==0);
    now=1999;
    for(unsigned i=0;i<10;++i){assert(bt_hci_radio_diagnostics(&out));bt_hci_phone_poll();}
    assert(radio_writes==5 && out.age_ms[0]==1999);
    now=2000;bt_hci_phone_poll();assert(radio_writes==6 && radio_opcode==0x1405);radio_reply(radio_opcode,0x11,0,0);
    bt_hci_phone_poll();assert(radio_writes==7 && radio_opcode==0x1403);radio_reply(radio_opcode,0x11,220,0);
    bt_hci_phone_poll();assert(radio_writes==7);
    now=10000;
    const uint16_t expected[]={0x0c2d,0x1406,0x1405,0x1403};
    for(unsigned i=0;i<4;++i){
        bt_hci_phone_poll();assert(radio_opcode==expected[i] && !radio_power_type);
        radio_reply(radio_opcode,0x11,radio_opcode==0x1406?0:7,0);
    }
    bt_hci_radio_diagnostics(&out);assert(out.afh_mode==0 && !out.afh_map[0] && out.max_tx_power==10);
    bt_hci_radio_active(false);now=100000;bt_hci_phone_poll();assert(radio_writes==11);
    assert(bt_hci_radio_diagnostics(&out) && !out.active && !out.pending);
    assert(packet_writes==1 && !flush_writes); // No radio sampling changes packet or flush policy.
}
static void test_radio_pressure_counts_only_the_selected_acl_and_reads_do_not_reset(void) {
    packet_normal_fixture();bt_hci_radio_diagnostics_t out;assert(bt_hci_radio_diagnostics(&out));
    uint32_t epoch=out.epoch;
    uint8_t flush[]={0x11,2,0x11,0};hci_packet_handler(HCI_EVENT_PACKET,0,flush,sizeof(flush));
    flush[2]=0x22;hci_packet_handler(HCI_EVENT_PACKET,0,flush,sizeof(flush));
    uint8_t completed[]={HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS,9,2,0x11,0,3,0,0x22,0,7,0};
    hci_packet_handler(HCI_EVENT_PACKET,0,completed,sizeof(completed));
    for(unsigned i=0;i<10;++i){
        assert(bt_hci_radio_diagnostics(&out) && out.epoch==epoch && out.flush_events==1 && out.completed_packets==3);
    }
    assert(!radio_writes && !out.valid);
    memcpy(device_addr,second,6);credit_connections[2]=(hci_connection_t){
        .con_handle=0x33,.address_type=BD_ADDR_TYPE_ACL,.state=OPEN};memcpy(credit_connections[2].address,second,6);
    assert(bt_hci_radio_diagnostics(&out) && out.epoch!=epoch && out.handle==0x33 && !out.flush_events && !out.completed_packets);
}
static void test_radio_errors_unsupported_and_timeouts_stop_without_retry_spam(void) {
    radio_fixture();
    uint8_t unsupported[]={HCI_EVENT_COMMAND_COMPLETE,4,1,0x05,0x14,1};
    hci_packet_handler(HCI_EVENT_PACKET,0,unsupported,sizeof(unsupported));
    bt_hci_phone_poll();assert(radio_opcode==0x1403);radio_reply(radio_opcode,0x11,200,0);
    bt_hci_phone_poll();assert(radio_opcode==0x0c2d && !radio_power_type);radio_reply(radio_opcode,0x11,0,0x0c);
    bt_hci_phone_poll();assert(radio_power_type==1);radio_reply(radio_opcode,0x11,0,0x11);
    bt_hci_phone_poll();assert(radio_opcode==0x1406);radio_reply(radio_opcode,0x11,2,0);
    bt_hci_radio_diagnostics_t out;bt_hci_radio_diagnostics(&out);
    assert(out.valid==BT_RADIO_QUALITY && out.unavailable==(BT_RADIO_RSSI|BT_RADIO_MAX_TX_POWER) &&
        out.failed==(BT_RADIO_TX_POWER|BT_RADIO_AFH_MAP) && out.age_ms[0]==UINT32_MAX);
    now=2000;bt_hci_phone_poll();assert(radio_writes==6 && radio_opcode==0x1403);
    radio_reply(radio_opcode,0x11,210,0);
    for(unsigned i=0;i<20;++i)bt_hci_phone_poll();
    assert(radio_writes==6);

    packet_normal_fixture();now=UINT32_MAX-499u;bt_hci_radio_active(true);bt_hci_phone_poll();
    now=499;bt_hci_phone_poll();assert(!radio.data.failed);
    now=500;for(unsigned i=0;i<20;++i)bt_hci_phone_poll();
    assert(radio_writes==1 && radio_pending.active && radio_pending.timed_out && radio.data.failed==BT_RADIO_RSSI);
    bt_hci_radio_active(false);bt_hci_radio_active(true);bt_hci_phone_poll();assert(radio_writes==1);
    radio_reply(0x1405,0x11,(uint8_t)-20,0);bt_hci_radio_diagnostics(&out);
    assert(!out.pending && out.valid==BT_RADIO_RSSI && !out.failed && out.rssi==-20 && !out.age_ms[0]);

    packet_normal_fixture();radio_result=0x0c;bt_hci_radio_active(true);bt_hci_phone_poll();
    assert(radio_writes==1 && !radio_pending.active && radio.data.failed==BT_RADIO_RSSI);
}
static void test_radio_stale_malformed_replies_and_policy_priority(void) {
    radio_fixture();uint32_t epoch=radio.data.epoch;
    uint8_t disconnected[]={HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0x11,0,0x13};
    hci_packet_handler(HCI_EVENT_PACKET,0,disconnected,sizeof(disconnected));
    bt_hci_radio_active(true);bt_hci_phone_poll();
    assert(radio.data.epoch!=epoch && radio_pending.active && radio_writes==1);
    radio_reply(0x1405,0x11,(uint8_t)-45,0);
    assert(!radio_pending.active && !radio.data.valid);
    bt_hci_phone_poll();assert(radio_writes==2);
    radio_reply(0x1403,0x11,200,0);radio_reply(0x1405,0x22,20,0);
    assert(radio_pending.active && !radio.data.valid);
    uint8_t short_reply[]={HCI_EVENT_COMMAND_COMPLETE,4,1,0x05,0x14,0};
    hci_packet_handler(HCI_EVENT_PACKET,0,short_reply,sizeof(short_reply));assert(radio_pending.active);
    radio_reply(0x1405,0x11,(uint8_t)-5,0);assert(!radio_pending.active && radio.data.rssi==-5);

    radio_fixture();credit_connections[0].state=0;
    hci_packet_handler(HCI_EVENT_PACKET,0,disconnected,sizeof(disconnected));
    radio_reply(0x1405,0x11,20,0);
    assert(!radio_pending.active && !radio.data.valid);
    bt_hci_radio_diagnostics_t out;assert(!bt_hci_radio_diagnostics(&out) && out.handle==HCI_CON_HANDLE_INVALID);
    uint8_t off[]={BTSTACK_EVENT_STATE,1,0};hci_packet_handler(HCI_EVENT_PACKET,0,off,sizeof(off));
    assert(!radio_pending.active && !radio.data.valid);

    packet_normal_fixture();consume_command_credit=true;bt_hci_radio_active(true);flush_dirty_timeout();
    bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();assert(packet_writes==2 && !flush_writes && !radio_writes);
    packet_status(0);command_ready=true;bt_hci_phone_poll();assert(flush_writes==1 && !radio_writes);
    flush_ack(0x11,0);packet_changed(0x11,0xee1c,0);command_ready=true;bt_hci_phone_poll();
    assert(radio_writes==1 && radio_opcode==0x1405 && !command_ready);
}

static void unanswered_command(unsigned kind,uint32_t start) {
    if(kind==0){
        flush_fixture();packet_sync_complete=false;now=start;bt_hci_phone_poll();packet_status(0);
        assert(media_packet_pending.active && media_packet_pending.accepted);
    }else if(kind==1){
        flush_fixture();packet_sync_complete=true;now=start;flush_ack(0x11,0);
        flush_dirty_timeout();bt_hci_airpods_flush_timeout(true);bt_hci_phone_poll();assert(media_flush_pending.active);
    }else{
        packet_normal_fixture();now=start;bt_hci_radio_active(true);bt_hci_phone_poll();
        assert(radio_pending.active);
    }
}
static void test_unanswered_controller_commands_recover_after_grace(void) {
    for(volatile unsigned kind=0;kind<3;++kind){
        unanswered_command(kind,0);now=34999;bt_hci_phone_poll();
        unsigned before=recovery_count;recovery_expected=true;now=35000;
        if(!setjmp(recovery)){bt_hci_phone_poll();assert(!"Missing command reply did not recover");}
        recovery_expected=false;assert(recovery_count==before+1);
    }
    unanswered_command(0,0);now=34000;
    uint8_t disconnected[]={HCI_EVENT_DISCONNECTION_COMPLETE,4,0,0x11,0,0x13};
    hci_packet_handler(HCI_EVENT_PACKET,0,disconnected,sizeof(disconnected));
    credit_connections[0].con_handle=0x44;bt_hci_phone_poll();
    assert(media_packet_pending.handle==0x11 && packet_writes==1);
    recovery_expected=true;now=35000;
    if(!setjmp(recovery)){bt_hci_phone_poll();assert(!"Reconnect extended old command's deadline");}
    recovery_expected=false;
    unanswered_command(0,UINT32_MAX-999u);now=33999;
    bt_hci_phone_poll();recovery_expected=true;now=34000;
    if(!setjmp(recovery)){bt_hci_phone_poll();assert(!"Clock wrap extended command's deadline");}
    recovery_expected=false;
    for(unsigned kind=0;kind<3;++kind){
        unanswered_command(kind,0);now=34999;
        if(kind==0)packet_changed(0x11,0xcc18,0);
        else if(kind==1)flush_ack(0x11,0);
        else radio_reply(0x1405,0x11,(uint8_t)-20,0);
        now=35001;bt_hci_phone_poll();
    }
    puts("Controller waits: finite recovery, late replies, reconnect and clock wrap passed");
}
int main(void) {
    test_unanswered_controller_commands_recover_after_grace();
    test_retired_pair_has_no_ram_target_but_keeps_authenticated_discovery_hint();
    test_unrelated_speaker_cannot_win_airpods_pairing();
    test_pairing_device_id_name_and_authenticated_identity_fallbacks();
    test_radio_sampling_is_sparse_read_only_and_cached();
    test_radio_pressure_counts_only_the_selected_acl_and_reads_do_not_reset();
    test_radio_errors_unsupported_and_timeouts_stop_without_retry_spam();
    test_radio_stale_malformed_replies_and_policy_priority();
    test_media_packet_baseline_game_and_normal_are_confirmed_per_acl();
    test_media_packet_baseline_clamp_polarity_dm1_and_reserved_bits();
    test_media_packet_command_credit_preserves_native_policy_order();
    test_media_packet_failures_are_bounded_and_status_is_not_completion();
    test_media_packet_old_peer_restore_phone_exclusion_and_handle_reuse();
    test_media_flush_game_retains_baseline_and_restores_dirty_timeout();
    test_media_flush_targets_only_airpods_and_confirms_ack();
    test_media_flush_failures_and_superseded_commands_are_bounded();
    test_media_flush_reconnect_ack_cannot_confirm_new_epoch();
    test_media_flush_selected_peer_remembers_confirmed_value();
    test_media_flush_comparison_is_bounded_and_only_changes_timeout();
    test_media_flush_comparison_clears_on_cancel_scope_loss_and_stale_ack();
    test_authenticated_phone_acl_session();
    test_current_link_roles();
    test_credit_diagnostics();
    test_selected_bond_stays_passive_at_boot();
    test_first_boot_can_pair_but_missing_selected_bond_cannot_fallback();
    test_only_live_pairing_inquiry_can_select_a_peer();
    test_short_events_and_unbounded_names_are_rejected();
    test_controller_off_and_failed_inquiry_release_ble_pause();
    test_controller_rejection_releases_ble_pause();
    test_failed_storage_cannot_replace_selected_peer();
    test_phone_pairing_is_bounded_and_keeps_airpods_slots();
    test_phone_pairing_expiry_authentication_and_inquiry_roles();
    test_phone_storage_rejects_unknown_and_failed_writes();
    test_phone_pairing_survives_idle_output_transitions();
    test_new_phone_pairing_requires_quiet_routes();
    test_phone_candidate_timeout_and_busy_poll_close_only_candidate();
    puts("HCI selected-peer ownership, per-AirPods media flush, quiet phone pairing/authentication/storage and BLE coordination passed");
    return 0;
}
