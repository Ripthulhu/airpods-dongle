#include <assert.h>
#include <stdio.h>
#include "hci_stubs/btstack.h"

// The transport header has unrelated static encoder declarations.
#define PICOW_USB_BT_AUDIO_SSP_COUNTER_H
void avdtp_source_establish_stream(void);
#include "../src/btstack/btstack_hci.c"

static bd_addr_t saved[2];
static bool has_key[2], non_flushable;
static uint8_t slot, inquiry_result;
static unsigned connect_calls, inquiry_calls, inquiry_stops, writes, pin_calls;
static unsigned registered;
static uint16_t flush_timeout;
static bool ble_ready;
static bool fail_storage;
static uint32_t now;
static unsigned pin_rejections, confirmations, confirmation_rejections, passkey_rejections;
static uint8_t discoverable, security_level;
static bool phone_has_key, missing_acl, tlv_missing, tlv_fail, tlv_corrupt;
static unsigned phone_writes;
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
bool hci_non_flushable_packet_boundary_flag_supported(void) { return non_flushable; }
void gap_pin_code_response(bd_addr_t address, const char *pin) {
    (void)address;
    assert(strcmp(pin, "0000") == 0);
    pin_calls++;
}
bool gap_get_link_key_for_bd_addr(bd_addr_t address, link_key_t key, link_key_type_t *type) {
    if (phone_has_key && memcmp(address, phone_key_address, 6) == 0) {
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
    static hci_connection_t connection = {.con_handle = 1};
    return missing_acl ? NULL : &connection;
}
uint8_t gap_security_level(uint16_t handle) { assert(handle == 1); return security_level; }
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
void set_led_mode_off(void) {}
void set_led_mode_pairing(void) {}
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
    has_key[0] = has_key[1] = false;
    scan_active = selected_bonded = false;
    non_flushable = true;
    slot = 1;
    inquiry_result = 0;
    connect_calls = inquiry_calls = inquiry_stops = writes = pin_calls = registered = 0;
    flush_timeout = UINT16_MAX;
    ble_ready = false;
    fail_storage = false;
    ble_pauses = 0;
    now = 0;
    pin_rejections = confirmations = confirmation_rejections = passkey_rejections = 0;
    discoverable = 0;
    security_level = LEVEL_2;
    phone_has_key = missing_acl = tlv_missing = tlv_fail = tlv_corrupt = false;
    phone_writes = 0;
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
}

static void test_selected_bond_auto_connects(void) {
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
    assert(connect_calls == 1 && inquiry_calls == 0 && flush_timeout == 200);
    assert(ble_ready && ble_pauses == 0);
    assert(writes == 0);

    fixture();
    memcpy(saved[0], first, 6);
    has_key[0] = true;
    non_flushable = false;
    bt_hci_init();
    working();
    assert(connect_calls == 1 && inquiry_calls == 0 && flush_timeout == 0);
}

static void test_first_boot_can_pair_but_missing_selected_bond_cannot_fallback(void) {
    fixture();
    bt_hci_init();
    working();
    assert(scan_active && inquiry_calls == 1 && connect_calls == 0);
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
    assert(scan_active && ble_ready && (ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
    inquiry_result = 1;
    unsigned calls = inquiry_calls;
    gap_start_scanning(); // A busy duplicate must not cancel ownership of the live inquiry.
    assert(inquiry_calls == calls && scan_active && (ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
    complete();
    assert(!scan_active && !(ble_pauses & BLE_SCAN_PAUSE_INQUIRY));
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

    bt_hci_phone_pairing_start();
    assert(bt_hci_phone_pairing_active() && discoverable == 1);
    assert(!allow(second, HCI_LINK_TYPE_ACL)); // A saved AirPods slot is never a phone.
    assert(!allow(nearby, 0)); // No SCO/HFP support is advertised by this firmware.
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

int main(void) {
    test_selected_bond_auto_connects();
    test_first_boot_can_pair_but_missing_selected_bond_cannot_fallback();
    test_only_live_pairing_inquiry_can_select_a_peer();
    test_short_events_and_unbounded_names_are_rejected();
    test_controller_off_and_failed_inquiry_release_ble_pause();
    test_controller_rejection_releases_ble_pause();
    test_failed_storage_cannot_replace_selected_peer();
    test_phone_pairing_is_bounded_and_keeps_airpods_slots();
    test_phone_pairing_expiry_authentication_and_inquiry_roles();
    test_phone_storage_rejects_unknown_and_failed_writes();
    puts("HCI selected-peer ownership, phone pairing/authentication/storage and BLE coordination: 10 groups passed");
    return 0;
}
