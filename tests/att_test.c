#include <assert.h>
#include <stdio.h>
#include "../src/btstack/btstack_att.c"
static uint32_t now, peer_session = 1;
static bool ready = true;
static uint8_t create_error, send_error;
static uint16_t next_cid = 0x40, remote_mtu = 512, last_closed;
static unsigned connects, closes, sends;
static const bd_addr_t selected = {1,2,3,4,5,6};
static uint8_t ear_packet[8], busy_routes;
static uint16_t ear_length;
static uint32_t ear_age;
uint32_t btstack_run_loop_get_time_ms(void) { return now; }
uint32_t aacp_control_session(void) { return peer_session; }
bool aacp_control_address(bd_addr_t out) { if (!ready) return false; memcpy(out, selected, 6); return true; }
bool get_usb_streaming(void) { return (busy_routes & 1) != 0; }
bool check_is_streaming(void) { return (busy_routes & 2) != 0; }
bool usb_mic_requested(void) { return (busy_routes & 4) != 0; }
bool aacp_mic_active(void) { return (busy_routes & 8) != 0; }
bool phone_sink_connected(void) { return (busy_routes & 16) != 0; }
uint16_t aacp_research_cached(uint8_t slot, uint8_t *out, uint16_t capacity) {
    assert(slot == 5 && capacity >= 16);
    if (!ear_length) return 0;
    dongle_put32(out, ear_age);
    dongle_put32(out + 4, 1);
    memcpy(out + 8, ear_packet, ear_length);
    return ear_length + 8;
}
uint8_t l2cap_create_channel(btstack_packet_handler_t handler, bd_addr_t address, uint16_t psm,
                             uint16_t mtu, uint16_t *cid) {
    assert(handler == packet_handler && psm == 0x1f && mtu == 512 && memcmp(address, selected, 6) == 0);
    connects++;
    if (!create_error) *cid = next_cid++;
    return create_error;
}
void l2cap_disconnect(uint16_t cid) { last_closed = cid; closes++; }
void l2cap_request_can_send_now_event(uint16_t cid) { assert(cid != 0); }
uint16_t l2cap_get_remote_mtu_for_local_cid(uint16_t cid) { assert(cid == att_cid); return remote_mtu; }
uint8_t l2cap_send(uint16_t cid, uint8_t *p, uint16_t n) {
    assert(cid == att_cid && n > 0 && p == tx); sends++; return send_error;
}
static void event(uint8_t event, uint16_t cid) {
    uint8_t p[] = {event, 2, (uint8_t)cid, (uint8_t)(cid >> 8)};
    packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}
static void open_event(uint16_t cid, uint8_t status, bool correct_peer) {
    uint8_t p[26] = {L2CAP_EVENT_CHANNEL_OPENED, 24, status};
    for (unsigned i = 0; i < 6; ++i) p[3 + i] = selected[5 - i];
    if (!correct_peer) p[3] ^= 1;
    p[13] = cid; p[14] = cid >> 8;
    packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}
static void reply(const uint8_t *p, uint16_t n) { packet_handler(L2CAP_DATA_PACKET, att_cid, (uint8_t *)p, n); }
static void fixture(void) {
    dongle_att_cancel(); now = 0; peer_session++; ready = true;
    connects = closes = sends = 0; create_error = send_error = 0; remote_mtu = 512;
    memcpy(ear_packet, (uint8_t[]){4,0,4,0,6,0,2,2}, sizeof(ear_packet));
    ear_length = sizeof(ear_packet); ear_age = 0; busy_routes = 0;
}
static void begin(void) {
    const uint8_t read[] = {0x0a, 0x14, 0};
    assert(dongle_att_request(read, sizeof(read)));
    open_event(att_cid, 0, true);
    event(L2CAP_EVENT_CAN_SEND_NOW, att_cid);
}
int main(void) {
    uint8_t out[512]; uint16_t length;
    const uint8_t read[] = {0x0a, 0x14, 0}, response[] = {0x0b, 1, 2, 3};
    fixture(); ready = false;
    assert(!dongle_att_request(read, sizeof(read)) && connects == 0);
    ready = true;
    assert(!dongle_att_request((uint8_t[]){0x52,0,0,1}, 4));
    begin();
    assert(!dongle_att_request(read, sizeof(read)));
    reply((uint8_t[]){0x1b,1,0,8}, 4);
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_WAIT);
    reply(response, sizeof(response));
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_DONE && length == sizeof(response));
    assert(memcmp(out, response, length) == 0);
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_WAIT);
    assert(dongle_att_request((uint8_t[]){0x52,0x14,0,1},4));
    event(L2CAP_EVENT_CAN_SEND_NOW, att_cid);
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_DONE && length == 0 && connects == 1);
    event(L2CAP_EVENT_CHANNEL_CLOSED, att_cid);
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_WAIT);
    begin();
    reply(response, sizeof(response));
    assert(dongle_att_take(out, 1, &length) == DONGLE_ATT_FAILED);
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_WAIT);
    fixture(); begin();
    reply((uint8_t[]){1,0x0a,0x14,0,0x0a},5);
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_DONE && length == 5);
    fixture(); begin();
    reply((uint8_t[]){0x13},1);
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_FAILED && closes == 1);
    fixture(); begin();
    uint16_t old_cid = att_cid;
    dongle_att_cancel();
    packet_handler(L2CAP_DATA_PACKET, old_cid, (uint8_t *)response, sizeof(response));
    open_event(old_cid, 0, true);
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_WAIT && last_closed == old_cid);
    begin(); peer_session++;
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_FAILED);
    fixture(); begin(); now = 3000;
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_FAILED);
    fixture(); remote_mtu = 2; begin();
    assert(sends == 0 && dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_FAILED);
    fixture(); send_error = 1; begin(); now = 3000;
    assert(dongle_att_take(out, sizeof(out), &length) == DONGLE_ATT_FAILED);
    fixture(); create_error = 1;
    assert(dongle_att_request(read,sizeof(read)));
    assert(dongle_att_take(out,sizeof(out),&length) == DONGLE_ATT_FAILED);
    fixture(); assert(dongle_att_request(read,sizeof(read)));
    open_event(att_cid,0,false); now = 3000;
    assert(dongle_att_take(out,sizeof(out),&length) == DONGLE_ATT_FAILED);
    fixture(); begin();
    event(L2CAP_EVENT_CHANNEL_CLOSED,att_cid);
    assert(dongle_att_take(out,sizeof(out),&length) == DONGLE_ATT_FAILED);
    fixture(); begin();
    uint8_t large[513] = {0x0b}; reply(large,sizeof(large));
    assert(dongle_att_take(out,sizeof(out),&length) == DONGLE_ATT_FAILED);
    puts("ATT transport: bounded replies, cancellation, stale peers and failures passed");

    const uint8_t self_check[] = {0x12,0x14,0,'1'};
    fixture();
    assert(dongle_att_request(self_check, sizeof(self_check)));
    open_event(att_cid, 0, true);
    ear_packet[6] = 0; // Taken out after host validation, before radio send.
    event(L2CAP_EVENT_CAN_SEND_NOW, att_cid);
    assert(!sends && dongle_att_take(out,sizeof(out),&length) == DONGLE_ATT_FAILED);
    for (unsigned i = 0; i < 5; ++i) {
        fixture(); assert(dongle_att_request(self_check, sizeof(self_check)));
        open_event(att_cid, 0, true); busy_routes = (uint8_t)(1u << i);
        event(L2CAP_EVENT_CAN_SEND_NOW, att_cid);
        assert(!sends && dongle_att_take(out,sizeof(out),&length) == DONGLE_ATT_FAILED);
    }
    for (unsigned i = 0; i < 4; ++i) {
        fixture(); assert(dongle_att_request(self_check, sizeof(self_check)));
        open_event(att_cid, 0, true);
        if (i == 0) ear_length = 0;
        if (i == 1) ear_length = 7;
        if (i == 2) ear_packet[5] = 1;
        if (i == 3) ear_age = 30001;
        event(L2CAP_EVENT_CAN_SEND_NOW, att_cid);
        assert(!sends && dongle_att_take(out,sizeof(out),&length) == DONGLE_ATT_FAILED);
    }
    fixture(); assert(dongle_att_request(self_check, sizeof(self_check)));
    open_event(att_cid, 0, true); ear_age = 30000;
    event(L2CAP_EVENT_CAN_SEND_NOW, att_cid);
    assert(sends == 1); reply((uint8_t[]){0x13}, 1);
    assert(dongle_att_take(out,sizeof(out),&length) == DONGLE_ATT_DONE);
    fixture(); assert(dongle_att_request(self_check, sizeof(self_check)));
    open_event(att_cid, 0, true); send_error = 1;
    event(L2CAP_EVENT_CAN_SEND_NOW, att_cid);
    assert(sends == 1); send_error = 0; ear_packet[7] = 1;
    event(L2CAP_EVENT_CAN_SEND_NOW, att_cid);
    assert(sends == 1 && dongle_att_take(out,sizeof(out),&length) == DONGLE_ATT_FAILED);
    fixture(); busy_routes = 31; ear_length = 0;
    assert(dongle_att_request((uint8_t[]){0x12,0x14,0,'1',0}, 5));
    open_event(att_cid, 0, true); event(L2CAP_EVENT_CAN_SEND_NOW, att_cid);
    assert(sends == 1); reply((uint8_t[]){0x13}, 1);
    assert(dongle_att_take(out,sizeof(out),&length) == DONGLE_ATT_DONE);
    assert(dongle_att_request((uint8_t[]){0x52,0x14,0,'1'}, 4));
    event(L2CAP_EVENT_CAN_SEND_NOW, att_cid);
    assert(sends == 2 && dongle_att_take(out,sizeof(out),&length) == DONGLE_ATT_DONE);
    puts("ATT self-check: fresh case and idle routes required at every actual send attempt");
}
