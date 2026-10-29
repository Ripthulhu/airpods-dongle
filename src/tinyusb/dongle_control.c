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
    uint8_t status[64], command[64];
    volatile bool available, pending;
    volatile uint32_t epoch;
    uint32_t seen_epoch, generation, last_sequence;
    uint8_t last_result, last_opcode;
    bool initialized;
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
    ++controls[client].epoch;
}
void dongle_control_available(bool on) { dongle_control_client_available(DONGLE_CLIENT_USB, on); }
bool dongle_control_client_get(uint8_t client, uint8_t report[64]) {
    if (client >= DONGLE_CLIENT_COUNT || !report || !controls[client].initialized) return false;
    memcpy(report, controls[client].status, 64);
    return true;
}
uint16_t dongle_control_get(uint8_t *out, uint16_t capacity) {
    if (!out || capacity < 63 || !controls[0].initialized) return 0;
    memcpy(out, controls[0].status + 1, 63);
    return 63;
}
bool dongle_control_client_set(uint8_t client, const uint8_t report[64]) {
    if (client >= DONGLE_CLIENT_COUNT || !report) return false;
    control_client_t *c = &controls[client];
    if (!c->available || c->pending) return false;
    memcpy(c->command, report, 64);
    c->pending = true;
    return true;
}
void dongle_control_set(const uint8_t *buffer, uint16_t size) {
    if (!buffer || size != 63) return;
    uint8_t report[64] = {2};
    memcpy(report + 1, buffer, 63);
    (void)dongle_control_client_set(DONGLE_CLIENT_USB, report);
}
static void execute(control_client_t *c, const uint8_t *p) {
    uint32_t sequence = dongle_u32(p + 18);
    if (c->last_sequence && (!sequence || sequence == c->last_sequence ||
        (uint32_t)(sequence - c->last_sequence) >= UINT32_C(0x80000000))) return;
    c->last_sequence = sequence;
    c->last_opcode = p[7] <= 4 ? p[7] : 0;
    c->last_result = DONGLE_ACCEPTED;
    if (!dongle_command_valid(p, 64)) c->last_result = DONGLE_MALFORMED;
    else if (dongle_u32(p + 8) != c->generation || memcmp(p + 12, selected, 6)) c->last_result = DONGLE_STALE;
    else if (dongle_rpc_busy()) c->last_result = DONGLE_BUSY;
    else if (p[7] == 1) {
        static const uint8_t empty[6];
        if (!memcmp(selected, empty, 6)) c->last_result = DONGLE_DISCONNECTED;
        else a2dp_source_reconnect();
    } else if (p[7] == 2) {
        if (usb_mic_requested() || phone_sink_connected() || get_usb_streaming()) c->last_result = DONGLE_BUSY;
        else if (!avdtp_disconnect_and_scan()) c->last_result = DONGLE_BUSY;
    } else if (p[7] == 3) {
        if (!aacp_control_ready()) c->last_result = DONGLE_DISCONNECTED;
        else if (!aacp_control_readback()->anc) c->last_result = DONGLE_UNSUPPORTED;
        else if (!aacp_control_anc(p[22])) c->last_result = DONGLE_BUSY;
    } else if (p[7] == 4) {
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
            if (!++c->generation) ++c->generation;
        }
        uint8_t request[64], next[64];
        uint32_t irq = save_and_disable_interrupts();
        bool take = c->pending && c->available;
        if (take) memcpy(request, c->command, 64);
        c->pending = false;
        restore_interrupts(irq);
        if (take) execute(c, request);
        dongle_header(next);
        next[7] = (get_a2dp_connected_flag() ? 1 : 0) | (aacp_control_ready() ? 2 : 0) |
            (check_is_streaming() ? 4 : 0) | (phone_sink_connected() ? 8 : 0) |
            (aacp_mic_active() ? 16 : 0) | (bt_hci_phone_pairing_active() ? 32 : 0) |
            (get_usb_streaming() ? 64 : 0);
        dongle_put32(next + 8, c->generation);
        memcpy(next + 12, selected, 6);
        dongle_readback_t observed=*aacp_control_readback();
        uint8_t present, case_source;
        dongle_case_battery_merge(&observed,aacp_control_ready(),&present,&case_source);
        const dongle_readback_t *readback=&observed;
        next[18] = readback->anc;
        memcpy(next + 19, readback->battery, 3);
        next[22] = readback->charging;
        next[23] = dongle_output_codec();
        for (unsigned i = 0; i < 3; ++i)
            dongle_put32(next + 24 + i * 4, dongle_readback_age(readback,i,now));
        dongle_put32(next + 36, c->last_sequence);
        next[40] = c->last_result; next[41] = c->last_opcode;
        next[42] = 1 | 4 | 8 | 16 | 32 | 64 | 128 | (readback->anc ? 2 : 0);
        next[44] = present;
        next[45] = case_source;
        irq = save_and_disable_interrupts();
        memcpy(c->status, next, 64);
        c->initialized = true;
        restore_interrupts(irq);
    }
}
