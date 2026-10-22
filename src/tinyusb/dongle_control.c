// SPDX-License-Identifier: GPL-3.0-only
#include "dongle_control.h"
#include "dongle_protocol.h"
#include "dongle_rpc.h"
#include "hardware/sync.h"
#include "../btstack/btstack_hci.h"
#include "../btstack/btstack_avdtp_source.h"
#include "../btstack/btstack_aacp.h"
#include "../btstack/phone_sink.h"
#include "uac.h"

static uint8_t status[DONGLE_REPORT_SIZE], command[DONGLE_REPORT_SIZE];
static volatile bool initialized, available, pending;
static volatile uint32_t usb_session;
static uint32_t handled_usb_session;
static uint32_t generation = 1, last_session;
static uint8_t selected[6];
static uint32_t last_sequence;
static uint8_t last_result, last_opcode;
uint32_t dongle_control_generation(void) { return generation; }

void dongle_control_available(bool on) {
    available = on;
    pending = false;
    ++usb_session;
}
uint16_t dongle_control_get(uint8_t *buffer, uint16_t requested) {
    if (!initialized || !buffer || requested < DONGLE_REPORT_SIZE - 1) return 0;
    memcpy(buffer, status + 1, DONGLE_REPORT_SIZE - 1);
    return DONGLE_REPORT_SIZE - 1;
}
void dongle_control_set(const uint8_t *buffer, uint16_t size) {
    // Never overwrite an accepted command. A second request receives no matching
    // receipt and must time out; it cannot displace the first request.
    if (!available || pending || !buffer || size != DONGLE_REPORT_SIZE - 1) return;
    command[0] = DONGLE_REPORT_ID;
    memcpy(command + 1, buffer, size);
    pending = true;
}
void dongle_control_poll(void) {
    uint8_t next[DONGLE_REPORT_SIZE], request[DONGLE_REPORT_SIZE];
    uint32_t now = btstack_run_loop_get_time_ms();
    const uint8_t *address = (const uint8_t *)get_device_addr();
    const uint8_t empty[6] = {0};
    if (!address) address = empty;
    uint32_t session = aacp_control_session();
    if (handled_usb_session != usb_session) {
        handled_usb_session = usb_session;
        last_sequence = 0; last_result = last_opcode = 0;
        ++generation;
        if (!generation) ++generation;
    }
    if (memcmp(selected, address, 6) || session != last_session) {
        memcpy(selected, address, 6);
        last_session = session;
        ++generation;
        if (!generation) ++generation;
    }
    uint32_t irq = save_and_disable_interrupts();
    bool have_request = pending && available;
    if (have_request) memcpy(request, command, sizeof(request));
    pending = false;
    restore_interrupts(irq);
    if (have_request) {
        uint32_t seq = dongle_u32(request + 18);
        // Duplicate requests are idempotent even if readback changed meanwhile.
        if (last_sequence == 0 || (seq != 0 && seq != last_sequence &&
            (uint32_t)(seq - last_sequence) < UINT32_C(0x80000000))) {
            last_sequence = seq;
            last_opcode = request[7] <= 4 ? request[7] : 0;
            last_result = DONGLE_ACCEPTED;
            if (!dongle_command_valid(request, sizeof(request))) last_result = DONGLE_MALFORMED;
            else if (dongle_u32(request + 8) != generation || memcmp(request + 12, selected, 6))
                last_result = DONGLE_STALE;
            else if (dongle_rpc_busy()) last_result = DONGLE_BUSY;
            else if (request[7] == 1) {
                if (!memcmp(selected, empty, 6)) last_result = DONGLE_DISCONNECTED;
                else a2dp_source_reconnect();
            } else if (request[7] == 2) {
                // Pairing replaces the selected slot, and therefore must not
                // interrupt active USB capture or an active phone stream.
                if (usb_mic_requested() || phone_sink_connected() || get_usb_streaming()) last_result = DONGLE_BUSY;
                else if (!avdtp_disconnect_and_scan()) last_result = DONGLE_BUSY;
            } else if (request[7] == 3) {
                if (!aacp_control_ready()) last_result = DONGLE_DISCONNECTED;
                else if (!aacp_control_readback()->anc) last_result = DONGLE_UNSUPPORTED;
                else if (!aacp_control_anc(request[22])) last_result = DONGLE_BUSY;
            } else if (request[7] == 4 && !bt_hci_phone_pairing_start()) last_result = DONGLE_UNSUPPORTED;
        }
    }
    dongle_header(next);
    next[7] = (get_a2dp_connected_flag() ? 1 : 0) | (aacp_control_ready() ? 2 : 0) |
        (check_is_streaming() ? 4 : 0) | (phone_sink_connected() ? 8 : 0) |
        (aacp_mic_active() ? 16 : 0) | (bt_hci_phone_pairing_active() ? 32 : 0) |
        (get_usb_streaming() ? 64 : 0);
    dongle_put32(next + 8, generation);
    memcpy(next + 12, selected, 6);
    const dongle_readback_t *readback = aacp_control_readback();
    next[18] = readback->anc;
    memcpy(next + 19, readback->battery, 3);
    next[22] = readback->charging;
    next[23] = dongle_output_codec();
    for (unsigned i = 0; i < 3; ++i)
        dongle_put32(next + 24 + i * 4, readback->known[i] ? now - readback->updated[i] : UINT32_MAX);
    dongle_put32(next + 36, last_sequence);
    next[40] = last_result; next[41] = last_opcode;
    next[42] = 1 | 4 | 8 | (readback->anc ? 2 : 0);
    irq = save_and_disable_interrupts();
    memcpy(status, next, sizeof(status));
    initialized = true;
    restore_interrupts(irq);
}
