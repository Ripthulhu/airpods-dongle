// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/tinyusb/dongle_control.c"

static bd_addr_t test_address = {1,2,3,4,5,6};
static uint32_t test_session = 1;
static uint64_t now = 20;
static bool capture, playback, phone, ready = true;
static bool pairing_ok = true;
static bool pairing_busy;
static bool settings_busy;
static unsigned reconnects, pairs, phone_pairs, anc_writes;
static dongle_readback_t readback;
static bool case_fallback;
void dongle_case_battery_poll(void) {}
void dongle_case_battery_merge(dongle_readback_t *out, bool healthy, uint8_t *present, uint8_t *source) {
    (void)healthy;
    *present=*source=0;
    for (unsigned i=0;i<3;++i) if (out->known[i] && out->battery[i]<=100) *present|=1u<<i;
    if (case_fallback) {
        out->battery[0]=94; out->battery[1]=93; out->battery[2]=61;
        out->known[0]=out->known[1]=out->known[2]=true;
        out->updated[0]=out->updated[1]=out->updated[2]=10;
        out->charging=1; *present=5; *source=7;
    }
}
bool dongle_rpc_busy(void) { return settings_busy; }
uint32_t save_and_disable_interrupts(void) { return 0; }
void restore_interrupts(uint32_t state) { (void)state; }
uint32_t btstack_run_loop_get_time_ms(void) { return (uint32_t)now; }
uint64_t dongle_uptime_ms(void) { return now; }
bd_addr_t *get_device_addr(void) { return &test_address; }
uint32_t aacp_control_session(void) { return test_session; }
bool aacp_control_ready(void) { return ready; }
const dongle_readback_t *aacp_control_readback(void) { return &readback; }
bool aacp_control_anc(uint8_t mode) { assert(mode == 2 || mode == 3); anc_writes++; return true; }
void a2dp_source_reconnect(void) { reconnects++; }
bool avdtp_disconnect_and_scan(void) { if (pairing_ok) { pairs++; test_address[0]++; test_session++; } return pairing_ok; }
bool bt_hci_phone_pairing_start(void) { if (pairing_ok) phone_pairs++; return pairing_ok; }
bool bt_hci_phone_pairing_busy(void) { return pairing_busy; }
bool bt_hci_phone_pairing_active(void) { return false; }
bool usb_mic_requested(void) { return capture; }
bool phone_sink_connected(void) { return phone; }
bool get_usb_streaming(void) { return playback; }
bool get_a2dp_connected_flag(void) { return ready; }
bool check_is_streaming(void) { return playback; }
bool aacp_mic_active(void) { return capture; }
uint8_t dongle_output_codec(void) { return 3; }
static void request(uint8_t *r, uint8_t op, uint32_t seq) {
    dongle_header(r); r[7] = op; dongle_put32(r + 8, controls[0].generation);
    memcpy(r + 12, test_address, 6); dongle_put32(r + 18, seq);
}
static void send(uint8_t *r) { dongle_control_set(r + 1, 63); dongle_control_poll(); }
int main(void) {
    uint8_t r[64], out[63];
    dongle_readback_clear(&readback);
    assert(!dongle_control_get(out, 63));
    dongle_control_available(true); dongle_control_poll();
    assert(dongle_control_get(out, 62) == 0 && dongle_control_get(out, 63) == 63);
    assert(!memcmp(out, "APDG", 4));
    assert(controls[0].status[42] & 32); // Whole-workflow settings lease support.
    assert(controls[0].status[42] & 64); // Presence and case provenance extension.
    assert(controls[0].status[42] & 128); // Explicit USB host volume authority.
    uint32_t same_generation=controls[0].generation;
    case_fallback=true; dongle_control_poll();
    assert(controls[0].generation==same_generation);
    assert(controls[0].status[19]==94 && controls[0].status[20]==93 && controls[0].status[21]==61);
    assert(controls[0].status[22]==1 && dongle_u32(controls[0].status+24)==10);
    assert(controls[0].status[44]==5 && controls[0].status[45]==7);
    assert(controls[0].status[43]==0 && controls[0].status[46]==0);
    now+=(UINT64_C(1)<<32);
    dongle_control_poll();
    assert(dongle_u32(controls[0].status+24)==UINT32_MAX-1u);
    assert(controls[0].status[19]==94 && controls[0].generation==same_generation);
    now=20;
    case_fallback=false;
    for (unsigned i = 0; i < 10; ++i) dongle_control_poll();
    assert(!reconnects && !pairs && !anc_writes); // discovery is read-only
    request(r, 1, 1); send(r); assert(reconnects == 1 && controls[0].last_result == DONGLE_ACCEPTED);
    send(r); assert(reconnects == 1); // duplicate
    request(r, 1, 2); r[12]++; send(r); assert(reconnects == 1 && controls[0].last_result == DONGLE_STALE);
    request(r, 1, 3); dongle_control_set(r + 1, 63);
    dongle_control_available(false); dongle_control_poll(); assert(reconnects == 1);
    dongle_control_available(true); dongle_control_poll();
    request(r, 1, 1); dongle_control_set(r + 1, 63); test_session++;
    dongle_control_poll(); assert(controls[0].last_result == DONGLE_STALE && reconnects == 1);
    request(r, 2, 2); playback = true; send(r); assert(controls[0].last_result == DONGLE_BUSY && !pairs);
    playback = false; request(r, 2, 3); send(r); assert(pairs == 1);
    dongle_control_poll(); assert(controls[0].last_sequence == 3 && controls[0].last_result == DONGLE_ACCEPTED);
    request(r, 3, 4); r[22] = 2; send(r); assert(controls[0].last_result == DONGLE_UNSUPPORTED);
    readback.anc = 3; request(r, 3, 5); r[22] = 2; send(r);
    assert(anc_writes == 1 && readback.anc == 3); // receipt does not fabricate readback
    request(r, 3, 4); r[22] = 2; send(r); assert(anc_writes == 1); // older replay
    request(r, 4, 6); dongle_control_set(r + 1, 63);
    request(r, 1, 7); dongle_control_set(r + 1, 63); dongle_control_poll();
    assert(phone_pairs == 1 && reconnects == 1); // no overwrite
    request(r, 1, 8); r[63] = 1; send(r); assert(controls[0].last_result == DONGLE_MALFORMED && reconnects == 1);
    request(r, 255, 9); send(r);
    assert(controls[0].last_result == DONGLE_MALFORMED && controls[0].status[41] == 0);
    assert(dongle_control_get(out, 63) == 63); // malformed commands cannot poison controls[0].status
    request(r, 1, 10); send(r); assert(reconnects == 2 && controls[0].last_result == DONGLE_ACCEPTED);
    pairing_ok = false;
    request(r, 2, 11); send(r); assert(controls[0].last_result == DONGLE_BUSY && pairs == 1);
    request(r, 4, 12); send(r); assert(controls[0].last_result == DONGLE_UNSUPPORTED && phone_pairs == 1);
    settings_busy = true;
    request(r, 1, 13); send(r); assert(controls[0].last_result == DONGLE_BUSY && reconnects == 2);
    request(r, 3, 14); r[22] = 2; send(r); assert(controls[0].last_result == DONGLE_BUSY && anc_writes == 1);
    settings_busy=false;
    pairing_busy = true;
    request(r, 4, 15); send(r);
    assert(controls[0].last_result == DONGLE_BUSY && phone_pairs == 1);
    pairing_busy = false;
    request(r, 4, 16); send(r);
    assert(controls[0].last_result == DONGLE_UNSUPPORTED && phone_pairs == 1);
    dongle_control_client_available(1,true);dongle_control_poll();
    uint8_t phone_report[64];assert(dongle_control_client_get(1,phone_report));
    request(r,1,1);dongle_put32(r+8,dongle_control_client_generation(1));
    assert(dongle_control_client_set(1,r));dongle_control_poll();assert(reconnects==3);
    assert(controls[1].last_sequence==1 && controls[0].last_sequence==16);
    assert(dongle_control_client_set(1,r));dongle_control_poll();assert(reconnects==3);
    uint32_t phone_generation=dongle_control_client_generation(1);
    dongle_control_available(false);dongle_control_poll();
    assert(dongle_control_client_generation(1)==phone_generation && controls[1].last_sequence==1);
    dongle_control_client_available(1,false);dongle_control_poll();
    assert(!controls[1].last_sequence && dongle_control_client_generation(1)!=phone_generation);
    puts("dongle controls: silent discovery, target generations, duplicate commands, backpressure and USB loss passed");
}
