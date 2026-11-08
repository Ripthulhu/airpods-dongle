// SPDX-License-Identifier: GPL-3.0-only
#define AACP_RPC_INTEGRATION_TEST
#define main aacp_existing_main
#include "aacp_test.c"
#undef main
#include "../src/tinyusb/dongle_features.c"
#include "../src/tinyusb/dongle_rpc.c"

uint32_t save_and_disable_interrupts(void) {return 0;}
void restore_interrupts(uint32_t value) {(void)value;}
uint32_t dongle_control_client_generation(uint8_t client) {(void)client;return 1;}
bool dongle_att_request(const uint8_t *p,uint16_t n) {(void)p;(void)n;return false;}
void dongle_att_poll(void) {}
void dongle_att_cancel(void) {}
dongle_att_result_t dongle_att_take(uint8_t *p,uint16_t cap,uint16_t *n) {
    (void)p;(void)cap;*n=0;return DONGLE_ATT_WAIT;
}
uint8_t dongle_playback_configure(uint8_t c,uint8_t r,uint32_t b) {(void)c;(void)r;(void)b;return DONGLE_UNSUPPORTED;}
uint8_t dongle_playback_game_mode(bool e) {(void)e;return DONGLE_UNSUPPORTED;}
uint16_t dongle_playback_status(uint8_t *p,uint16_t n) {(void)p;(void)n;return 0;}
uint16_t dongle_routing_snapshot(uint8_t *p,uint16_t n) {(void)p;(void)n;return 0;}
uint8_t dongle_routing_request(uint8_t a,uint32_t r,uint32_t token) {(void)a;(void)r;(void)token;return DONGLE_UNSUPPORTED;}
uint8_t dongle_reconnect_policy(bool set,bool automatic,bool *current) {(void)set;(void)automatic;(void)current;return DONGLE_UNSUPPORTED;}
uint16_t dongle_playback_diagnostics(uint8_t *p,uint16_t n) {(void)p;(void)n;return 0;}
uint16_t phone_sink_diagnostics(uint8_t *p,uint16_t n) {(void)p;(void)n;return 0;}
uint16_t dongle_playback_trace(uint8_t *p,uint16_t n) {(void)p;(void)n;return 0;}
uint16_t dongle_playback_inspect(uint8_t *p,uint16_t n) {(void)p;(void)n;return 0;}
void dongle_playback_inspect_enable(bool active) {(void)active;}
void ble_scan_diagnostic_pause(bool paused) {(void)paused;}
bool bt_hci_airpods_flush_comparison(bool enabled) {return !enabled;}
bool bt_hci_radio_diagnostics(bt_hci_radio_diagnostics_t *out) {(void)out;return false;}
uint16_t dongle_playback_adaptive_snapshot(uint8_t *out,uint16_t cap) {(void)out;(void)cap;return 0;}
void dongle_playback_poll(void) {}
uint8_t dongle_finder_start(uint8_t c) {(void)c;return DONGLE_UNSUPPORTED;}
uint8_t dongle_finder_stop(uint8_t c) {(void)c;return DONGLE_ACCEPTED;}
uint16_t dongle_finder_snapshot(uint8_t c,uint8_t *p,uint16_t n) {(void)c;(void)p;(void)n;return 0;}
void dongle_finder_disconnect(uint8_t c) {(void)c;}
uint16_t dongle_volume_snapshot(uint8_t client,uint8_t *out,uint16_t capacity) {
    (void)client;(void)out;(void)capacity;return 0;
}
uint8_t dongle_volume_request(uint8_t client,const uint8_t *p,uint16_t size,
                             uint8_t *out,uint16_t capacity,uint16_t *length) {
    (void)client;(void)p;(void)size;(void)out;(void)capacity;
    *length=0;return DONGLE_UNSUPPORTED;
}
void dongle_volume_client_disconnect(uint8_t client) {(void)client;}
void dongle_finder_poll(void) {}
static void command(uint8_t client,uint8_t op,const uint8_t *p,uint8_t length) {
    uint8_t report[64]={3,'A','P','D','G',1,64};report[7]=op;
    dongle_put32(report+8,1);dongle_put32(report+12,1);
    report[16]=report[20]=length;if(length)memcpy(report+22,p,length);
    assert(dongle_rpc_client_set(client,report));dongle_rpc_poll();
}
static void queued(uint8_t client) {
    fixture();ready();memset(clients,0,sizeof(clients));transport_owner=lease_owner=-1;
    dongle_rpc_client_available(client,true);dongle_rpc_poll();
    const uint8_t setting[]={1,0x0d,2,0,0,0};
    command(client,7,setting,sizeof(setting));command(client,6,NULL,0);
    assert(clients[client].phase==RUNNING && transport_owner==client);
    assert(aacp_tx_count==1 && aacp_tx_queue[aacp_tx_head].data==setting_tx);
}
int main(void) {
    for(uint8_t client=0;client<DONGLE_CLIENT_COUNT;++client){
        for(unsigned poll=0;poll<2;++poll){
            queued(client);unsigned before=sends;
            dongle_rpc_client_available(client,false);
            if(poll)dongle_rpc_poll();
            event(L2CAP_EVENT_CAN_SEND_NOW,aacp_cid);dongle_rpc_poll();
            assert(sends==before && !aacp_tx_count && transport_owner<0);
        }
        queued(client);unsigned before=sends;command(client,5,NULL,0);
        event(L2CAP_EVENT_CAN_SEND_NOW,aacp_cid);
        assert(sends==before && !aacp_tx_count && clients[client].failure==CANCELLED);
        queued(client);before=sends;usb_capture_wanted=true;
        event(L2CAP_EVENT_CAN_SEND_NOW,aacp_cid);dongle_rpc_poll();
        assert(sends==before && clients[client].failure==TRANSPORT);
        queued(client);before=sends;now+=3000;
        event(L2CAP_EVENT_CAN_SEND_NOW,aacp_cid);dongle_rpc_poll();
        assert(sends==before && clients[client].failure==TRANSPORT);
        queued(client);before=sends;send_result=1;
        event(L2CAP_EVENT_CAN_SEND_NOW,aacp_cid);dongle_rpc_poll();
        assert(sends==before && clients[client].phase==RUNNING);
        send_result=0;event(L2CAP_EVENT_CAN_SEND_NOW,aacp_cid);dongle_rpc_poll();
        assert(sends==before+1 && clients[client].phase==COMPLETE && transport_owner<0);
        command(client,6,NULL,0);assert(sends==before+1); // No duplicate write.
    }
    puts("typed setting lifecycle: USB/phone cancellation, IRQ loss, microphone, deadline, retry and send-only completion passed");
}
