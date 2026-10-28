// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/btstack/volume_sync.h"
#include "../src/btstack/volume_scale.h"
#include "../src/tinyusb/media_control.h"

enum { AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED=1, AVRCP_SUBEVENT_SET_ABSOLUTE_VOLUME_RESPONSE=2,
       AVRCP_CTYPE_RESPONSE_INTERIM=15, AVRCP_CTYPE_RESPONSE_CHANGED_STABLE=13,
       AVRCP_CTYPE_RESPONSE_ACCEPTED=9, AVRCP_CTYPE_RESPONSE_REJECTED=10,
       AVRCP_CTYPE_RESPONSE_NOT_IMPLEMENTED=8, AVRCP_CONTROLLER=0, ERROR_CODE_SUCCESS=0 };
typedef struct {uint8_t transaction_id,controller_last_confirmed_transaction_id;} avrcp_connection_t;
static avrcp_connection_t connection;
static volume_sync_t sink_volume;
static bool _bt_sink_volume_changed, is_usb_streaming, host, mic, phone_owner, queue_full;
static uint32_t now,generation,revision,set_generation,sent;
static uint8_t peer[6]={1,2,3,4,5,6};
static unsigned cancels,keys,invalidations,absolute_sets;
static enum usb_media_command commands[64];
static struct {uint16_t avrcp_cid;} media_tracker;
static const uint8_t *get_device_addr(void) {return peer;}
static bool usb_host_active(void) {return host;}
static bool usb_mic_requested(void) {return mic;}
static bool aacp_mic_requested(void) {return mic;}
static bool phone_sink_owns_controls(void) {return phone_owner;}
static void phone_sink_volume_invalidated(void) {++invalidations;}
static void phone_sink_volume_changed(uint8_t level) {(void)level;}
static uint32_t btstack_run_loop_get_time_ms(void) {return now;}
static uint32_t usb_volume_set_revision(void) {return revision;}
uint32_t usb_media_generation(void) {return generation;}
uint32_t usb_media_volume_sent(void) {return sent;}
void usb_media_cancel_volume(void) {++cancels;}
bool usb_media_enqueue(enum usb_media_command command) {
    if(queue_full)return false;
    assert(keys<64);commands[keys++]=command;return true;
}
static uint16_t little_endian_read_16(const uint8_t *p,unsigned i) {return (uint16_t)(p[i]|(p[i+1]<<8));}
static uint8_t avrcp_controller_set_absolute_volume(uint16_t cid,uint8_t level) {
    assert(cid==sink_volume.cid && level<=127);++absolute_sets;
    ++connection.transaction_id;return 0;
}
static avrcp_connection_t *avrcp_get_connection_for_avrcp_cid_for_role(unsigned role,uint16_t cid) {
    assert(role==AVRCP_CONTROLLER);return cid==sink_volume.cid?&connection:NULL;
}
// Production relay, SET handling, AVRCP send/ACK and exact volume wire handler.
#include "playback_volume.inc"

static void fixture(void) {
    memset(&sink_volume,0,sizeof(sink_volume));memset(&usb_volume_relay,0,sizeof(usb_volume_relay));
    connection=(avrcp_connection_t){0};generation=3;revision=set_generation=sent=now=0;
    cancels=keys=invalidations=absolute_sets=0;queue_full=mic=phone_owner=false;
    host=is_usb_streaming=true;peer[0]=1;_bt_sink_volume_changed=false;
    bt_volume_select_peer();volume_sync_connection(&sink_volume,7,now);media_tracker.avrcp_cid=7;
    bt_volume_relay_reset(true);
}
static void event(uint8_t subtype,uint8_t ctype,uint8_t value) {
    uint8_t wire[]={0xc0,5,subtype,7,0,ctype,value};
    assert(bt_volume_controller_event(wire,sizeof(wire)));
}
static void changed(uint8_t value) {event(AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED,AVRCP_CTYPE_RESPONSE_CHANGED_STABLE,value);}
static void acknowledge(uint8_t value) {
    connection.controller_last_confirmed_transaction_id=connection.transaction_id;
    event(AVRCP_SUBEVENT_SET_ABSOLUTE_VOLUME_RESPONSE,AVRCP_CTYPE_RESPONSE_ACCEPTED,value);
}
static void host_set(uint8_t level) {
    ++revision;set_generation=generation;
    set_bt_volume_from_usb(bt_volume_to_usb(level),revision,generation,sent);
    bt_volume_poll();if(sink_volume.inflight)acknowledge(level);
}
static void ordinary_reversals_and_echoes(void) {
    fixture();host_set(81);
    event(AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED,AVRCP_CTYPE_RESPONSE_INTERIM,81);
    changed(81);assert(!keys);
    changed(73);assert(keys==1 && commands[0]==USB_MEDIA_VOLUME_DOWN && !bt_volume_usb_ready());
    ++sent;bt_volume_poll();host_set(73);assert(bt_volume_usb_ready());
    changed(73);assert(keys==1);
    changed(81);assert(keys==2 && commands[1]==USB_MEDIA_VOLUME_UP);
    ++sent;bt_volume_poll();host_set(81);changed(81);
    changed(73);assert(keys==3 && commands[2]==USB_MEDIA_VOLUME_DOWN);
    // Clamped own notifications arriving before an ACK cannot become gestures.
    ++revision;set_generation=generation;set_bt_volume_from_usb(bt_volume_to_usb(90),revision,generation,sent);
    bt_volume_poll();changed(89);assert(keys==3);acknowledge(89);changed(89);assert(keys==3);
}
static void rapid_queue_feedback(void) {
    fixture();host_set(64);changed(70);changed(60);changed(72);
    assert(keys==3 && commands[0]==USB_MEDIA_VOLUME_UP && commands[1]==USB_MEDIA_VOLUME_DOWN);
    unsigned before=cancels;++sent;bt_volume_poll();host_set(65);
    assert(cancels==before && !usb_volume_relay.waiting);
    ++sent;bt_volume_poll();assert(usb_volume_relay.waiting);
    host_set(64);++sent;bt_volume_poll();host_set(65);assert(cancels==before);
    changed(75);changed(80);++sent;bt_volume_poll();
    ++revision;uint32_t receipt=sent; // feedback SET for the latest transmitted key
    ++sent; // another queued gesture transmits before SET main-loop consumption
    set_bt_volume_from_usb(bt_volume_to_usb(66),revision,generation,receipt);
    assert(usb_volume_relay.waiting && !bt_volume_usb_ready());
    bt_volume_poll();acknowledge(66);assert(usb_volume_relay.waiting);
    host_set(67);assert(!usb_volume_relay.waiting && cancels==before);
}
static void proof_cold_start_and_fences(void) {
    fixture();changed(50);changed(60);assert(!keys);
    // Host SET precedes AirPods profile connection, as on ordinary cold USB boot.
    volume_sync_connection(&sink_volume,0,now);bt_volume_relay_reset(true);
    host_set(64);assert(usb_volume_relay.enabled);
    volume_sync_connection(&sink_volume,7,now);bt_volume_relay_reset(true);
    assert(usb_volume_relay.enabled);
    event(1,AVRCP_CTYPE_RESPONSE_INTERIM,64); // first native baseline is never a key
    sink_volume.pending=false;changed(70);assert(keys==1);
    unsigned before=cancels;is_usb_streaming=false;bt_volume_poll();
    assert(cancels>before && !usb_volume_relay.waiting);
    is_usb_streaming=true;mic=true;changed(75);assert(keys==2);
    peer[0]=9;bt_volume_select_peer();assert(!usb_volume_relay.waiting && sink_volume.cid==0);
    generation+=2;bt_volume_poll();assert(!usb_volume_relay.enabled);
    // A snapshot from the former USB session never writes the new AirPods state.
    set_bt_volume_from_usb(-5000,revision,generation-2,sent);assert(!sink_volume.pending);
}
static void malformed_and_unconsumed_set(void) {
    fixture();host_set(64);uint8_t wire[]={0xc0,5,1,7,0,13,72};
    assert(!bt_volume_controller_event(wire,6));wire[3]=8;
    assert(!bt_volume_controller_event(wire,7));wire[3]=7;wire[5]=0;
    assert(bt_volume_controller_event(wire,7));assert(!keys);
    wire[5]=13;wire[6]=128;assert(bt_volume_controller_event(wire,7));assert(!keys);
    ++revision;set_generation=generation;changed(70);assert(!keys && !bt_volume_usb_ready());
    // New SET IRQ wins against the old main-loop snapshot.
    set_bt_volume_from_usb(-5000,revision-1,generation,sent);assert(!sink_volume.pending);
    set_bt_volume_from_usb(-5000,revision,generation,sent);assert(sink_volume.pending);
}
static void timeout_and_queue_failure(void) {
    fixture();host_set(64);changed(72);++sent;now=100;bt_volume_poll();
    now=5099;bt_volume_poll();assert(usb_volume_relay.waiting);
    ++revision;set_generation=generation;now=5100;bt_volume_poll();
    assert(usb_volume_relay.waiting && !usb_volume_relay.disabled); // pending IRQ intent blocks stale fallback
    set_bt_volume_from_usb(bt_volume_to_usb(64),revision,generation,sent);bt_volume_poll();acknowledge(64);
    changed(73);now+=5000;bt_volume_poll();
    assert(usb_volume_relay.disabled && !usb_volume_relay.waiting && sink_volume.confirmed==73);
    unsigned before=absolute_sets;changed(74);host_set(74);changed(75);
    assert(keys==2 && absolute_sets==before); // No forced gain/rollback, disabled through same bus.
    generation+=2;bt_volume_poll();host_set(75);queue_full=true;changed(70);
    assert(usb_volume_relay.disabled && !usb_volume_relay.waiting);
}
int main(void) {
    ordinary_reversals_and_echoes();rapid_queue_feedback();proof_cold_start_and_fences();
    malformed_and_unconsumed_set();timeout_and_queue_failure();
    puts("USB stem volume: production controller/SET/timeout/route regressions passed");
}
