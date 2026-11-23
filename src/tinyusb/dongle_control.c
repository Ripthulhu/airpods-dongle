// SPDX-License-Identifier: GPL-3.0-only
#include "dongle_control.h"
#include "../btstack/dongle_case_battery.h"
#include "../btstack/btstack_ble_scan.h"
#include "dongle_protocol.h"
#include "dongle_rpc.h"
#include "hardware/sync.h"
#include "../btstack/btstack_hci.h"
#include "../btstack/btstack_avdtp_source.h"
#include "../btstack/btstack_aacp.h"
#include "../btstack/phone_sink.h"
#include "uac.h"

typedef struct {
    uint8_t status[DONGLE_REPORT_SIZE], command[DONGLE_REPORT_SIZE];
    volatile bool available, pending, initialized;
    volatile uint32_t epoch;
    uint32_t seen_epoch, generation, last_sequence;
    uint8_t last_result, last_opcode;
} control_client_t;
static control_client_t controls[DONGLE_CLIENT_COUNT];
static uint32_t last_session;
static uint8_t selected[6];

uint32_t dongle_control_client_generation(uint8_t client) {
    return client < DONGLE_CLIENT_COUNT ? controls[client].generation : 0;
}
uint32_t dongle_control_generation(void) { return dongle_control_client_generation(DONGLE_CLIENT_USB); }
void dongle_control_client_available(uint8_t client, bool on) {
    if (client >= DONGLE_CLIENT_COUNT) return;
    controls[client].available = on;
    controls[client].pending = false;
    controls[client].initialized = false;
    ++controls[client].epoch;
}
void dongle_control_available(bool on) { dongle_control_client_available(DONGLE_CLIENT_USB, on); }
bool dongle_control_client_get(uint8_t client, uint8_t report[64]) {
    if (client >= DONGLE_CLIENT_COUNT || !report || !controls[client].initialized) return false;
    memcpy(report, controls[client].status, DONGLE_REPORT_SIZE);
    return true;
}
uint16_t dongle_control_get(uint8_t *out, uint16_t capacity) {
    if (!out || capacity < DONGLE_REPORT_SIZE - 1 || !controls[0].initialized) return 0;
    memcpy(out, controls[0].status + 1, DONGLE_REPORT_SIZE - 1);
    return DONGLE_REPORT_SIZE - 1;
}
bool dongle_control_client_set(uint8_t client, const uint8_t report[64]) {
    if (client >= DONGLE_CLIENT_COUNT || !report) return false;
    control_client_t *c = &controls[client];
    if (!c->available || c->pending) return false;
    memcpy(c->command, report, DONGLE_REPORT_SIZE);
    c->pending = true;
    return true;
}
void dongle_control_set(const uint8_t *buffer, uint16_t size) {
    if (!buffer || size != DONGLE_REPORT_SIZE - 1) return;
    uint8_t report[DONGLE_REPORT_SIZE] = {DONGLE_REPORT_ID};
    memcpy(report + 1, buffer, DONGLE_REPORT_SIZE - 1);
    (void)dongle_control_client_set(DONGLE_CLIENT_USB, report);
}
static void execute(control_client_t *c, const uint8_t *p) {
    uint32_t sequence = dongle_u32(p + DONGLE_COMMAND_OFFSET_COMMAND_SEQUENCE);
    uint8_t opcode = p[DONGLE_COMMAND_OFFSET_OPCODE];
    if (c->last_sequence && (!sequence || sequence == c->last_sequence ||
        (uint32_t)(sequence - c->last_sequence) >= UINT32_C(0x80000000))) return;
    c->last_sequence = sequence;
    c->last_opcode = opcode <= DONGLE_COMMAND_OPEN_PHONE_PAIRING ? opcode : 0;
    c->last_result = DONGLE_ACCEPTED;
    if (!dongle_command_valid(p, DONGLE_REPORT_SIZE)) c->last_result = DONGLE_MALFORMED;
    else if (dongle_u32(p + DONGLE_COMMAND_OFFSET_CONNECTION_GENERATION) != c->generation ||
             memcmp(p + DONGLE_COMMAND_OFFSET_SELECTED_ADDRESS, selected, DONGLE_COMMAND_SIZE_SELECTED_ADDRESS)) c->last_result = DONGLE_STALE;
    else if (dongle_rpc_busy()) c->last_result = DONGLE_BUSY;
    else if (opcode == DONGLE_COMMAND_RECONNECT_AIRPODS) {
        static const uint8_t empty[6];
        if (!memcmp(selected, empty, 6)) c->last_result = DONGLE_DISCONNECTED;
        else if (!aacp_takeover_request(selected)) c->last_result = DONGLE_BUSY;
        else a2dp_source_reconnect();
    } else if (opcode == DONGLE_COMMAND_PAIR_AIRPODS) {
        if (usb_mic_requested() || aacp_mic_requested() || aacp_mic_active() ||
            phone_sink_connected() || get_usb_streaming()) c->last_result = DONGLE_BUSY;
        else if (!avdtp_disconnect_and_scan()) c->last_result = DONGLE_BUSY;
    } else if (opcode == DONGLE_COMMAND_CHANGE_LISTENING_MODE) {
        if (!aacp_control_ready()) c->last_result = DONGLE_DISCONNECTED;
        else if (!aacp_control_readback()->anc) c->last_result = DONGLE_UNSUPPORTED;
        else if (!aacp_control_anc(p[DONGLE_COMMAND_OFFSET_ARGUMENT])) c->last_result = DONGLE_BUSY;
    } else if (opcode == DONGLE_COMMAND_OPEN_PHONE_PAIRING) {
        if (bt_hci_phone_pairing_busy()) c->last_result = DONGLE_BUSY;
        else if (!bt_hci_phone_pairing_start())
            c->last_result = bt_hci_phone_pairing_busy() ? DONGLE_BUSY : DONGLE_UNSUPPORTED;
    }
}
void dongle_control_poll(void) {
    uint64_t now = dongle_uptime_ms();
    dongle_case_battery_poll();
    const uint8_t empty[6] = {0};
    const uint8_t *address = (const uint8_t *)get_device_addr();
    if (!address) address = empty;
    uint32_t session = aacp_control_session();
    bool changed = memcmp(selected, address, 6) || session != last_session;
    memcpy(selected, address, 6);
    last_session = session;
    for (unsigned client = 0; client < DONGLE_CLIENT_COUNT; ++client) {
        control_client_t *c = &controls[client];
        bool reset = c->seen_epoch != c->epoch;
        if (reset) {
            c->seen_epoch = c->epoch;
            c->last_sequence = c->last_result = c->last_opcode = 0;
        }
        if (changed || reset || !c->generation) {
            c->initialized = false;
            if (!++c->generation) ++c->generation;
        }
        if (!c->available) continue;
        uint8_t request[DONGLE_REPORT_SIZE], next[DONGLE_REPORT_SIZE];
        uint32_t irq = save_and_disable_interrupts();
        bool take = c->pending && c->available;
        if (take) memcpy(request, c->command, DONGLE_REPORT_SIZE);
        c->pending = false;
        restore_interrupts(irq);
        if (take) execute(c, request);
        dongle_header(next);
        next[DONGLE_STATUS_OFFSET_FLAGS] = (get_a2dp_connected_flag() ? DONGLE_STATUS_FLAG_A2DP_CONNECTED : 0) |
            (aacp_control_ready() ? DONGLE_STATUS_FLAG_AACP_READY : 0) |
            (check_is_streaming() ? DONGLE_STATUS_FLAG_STREAMING : 0) |
            (phone_sink_connected() ? DONGLE_STATUS_FLAG_PHONE_CONNECTED : 0) |
            (aacp_mic_active() ? DONGLE_STATUS_FLAG_MICROPHONE_ACTIVE : 0) |
            (bt_hci_phone_pairing_active() ? DONGLE_STATUS_FLAG_PHONE_PAIRING : 0) |
            (get_usb_streaming() ? DONGLE_STATUS_FLAG_USB_PLAYBACK : 0);
        dongle_put32(next + DONGLE_STATUS_OFFSET_CONNECTION_GENERATION, c->generation);
        memcpy(next + DONGLE_STATUS_OFFSET_SELECTED_ADDRESS, selected, DONGLE_STATUS_SIZE_SELECTED_ADDRESS);
        dongle_readback_t observed=*aacp_control_readback();
        uint8_t present, case_source;
        dongle_case_battery_merge(&observed,aacp_control_ready(),&present,&case_source);
        const dongle_readback_t *readback=&observed;
        next[DONGLE_STATUS_OFFSET_LISTENING_MODE] = readback->anc;
        memcpy(next + DONGLE_STATUS_OFFSET_BATTERY_PERCENT, readback->battery, DONGLE_STATUS_SIZE_BATTERY_PERCENT);
        next[DONGLE_STATUS_OFFSET_CHARGING_COMPONENTS] = readback->charging;
        next[DONGLE_STATUS_OFFSET_OUTPUT_CODEC] = dongle_output_codec();
        for (unsigned i = 0; i < 3; ++i)
            dongle_put32(next + DONGLE_STATUS_OFFSET_BATTERY_AGES_MILLISECONDS + i * 4, dongle_readback_age(readback,i,now));
        dongle_put32(next + DONGLE_STATUS_OFFSET_LAST_COMMAND_SEQUENCE, c->last_sequence);
        next[DONGLE_STATUS_OFFSET_LAST_COMMAND_RESULT] = c->last_result;
        next[DONGLE_STATUS_OFFSET_LAST_COMMAND_OPCODE] = c->last_opcode;
        // Capability 256: BOOTSEL during audio; 512: confirmed routing handover.
        uint16_t capabilities = DONGLE_CAPABILITY_PAIR_AND_RECONNECT | DONGLE_CAPABILITY_PHONE_PAIRING |
            DONGLE_CAPABILITY_SETTINGS_TRANSPORT | DONGLE_CAPABILITY_TYPED_FEATURES | DONGLE_CAPABILITY_SETTINGS_WORKFLOW_LEASE |
            DONGLE_CAPABILITY_BATTERY_OBSERVATION_FLAGS | DONGLE_CAPABILITY_USB_HOST_VOLUME_AUTHORITY |
            DONGLE_CAPABILITY_ACTIVE_AUDIO_RUNTIME_UPDATE | DONGLE_CAPABILITY_EXPLICIT_ROUTING_HANDOVER |
            (readback->anc ? DONGLE_CAPABILITY_CONFIRMED_NOISE_CONTROL : 0);
        next[DONGLE_STATUS_OFFSET_CAPABILITIES] = (uint8_t)capabilities;
        next[DONGLE_STATUS_OFFSET_CAPABILITIES + 1] = (uint8_t)(capabilities >> 8);
        next[DONGLE_STATUS_OFFSET_PRESENT_COMPONENTS] = present;
        next[DONGLE_STATUS_OFFSET_CASE_OBSERVED_COMPONENTS] = case_source;
        irq = save_and_disable_interrupts();
        memcpy(c->status, next, DONGLE_REPORT_SIZE);
        c->initialized = c->seen_epoch == c->epoch;
        restore_interrupts(irq);
    }
}
