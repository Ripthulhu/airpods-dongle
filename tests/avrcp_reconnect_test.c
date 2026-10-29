// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/btstack/volume_sync.h"

#define ERROR_CODE_SUCCESS 0
#define HCI_EVENT_PACKET 4
#define HCI_EVENT_AVRCP_META 0xe1
#define AVRCP_SUBEVENT_CONNECTION_ESTABLISHED 1
#define AVRCP_SUBEVENT_CONNECTION_RELEASED 2
#define AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED 1
#define AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED 2
#define AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED 9
#define AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED 13
#define AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED 6
#define UNUSED(x) (void)(x)
typedef uint8_t bd_addr_t[6];
typedef unsigned avrcp_track_t;
static avrcp_track_t tracks[1];
static bool a2dp_accept_incoming, a2dp_is_connected_flag, profile_pending, phone_event;
static bd_addr_t cur_active_device, selected;
static const bd_addr_t peer={1,2,3,4,5,6}, other={6,5,4,3,2,1};
static struct {uint16_t avdtp_cid, avrcp_cid;} media_tracker;
static volume_sync_t sink_volume;
static uint32_t now;
static unsigned connects, disconnects, notifications, invalidations;
static uint16_t next_cid, last_disconnected;
static uint8_t connect_result;
static bool setup_already_active;
static uint32_t btstack_run_loop_get_time_ms(void) {return now;}
static const uint8_t *get_device_addr(void) {return selected;}
static uint8_t avrcp_connect(uint8_t *address,uint16_t *cid) {
    assert(!memcmp(address,peer,6));++connects;
    if (!connect_result && !setup_already_active) *cid=next_cid++;
    return connect_result;
}
static void avrcp_disconnect(uint16_t cid) {assert(cid);++disconnects;last_disconnected=cid;}
static bool phone_sink_handle_avrcp(uint8_t type,uint16_t channel,uint8_t *packet,uint16_t size) {
    (void)type;(void)channel;(void)packet;(void)size;return phone_event;
}
static uint8_t hci_event_packet_get_type(const uint8_t *p) {return p[0];}
static uint16_t avrcp_subevent_connection_established_get_avrcp_cid(const uint8_t *p) {return p[4]|p[5]<<8;}
static uint8_t avrcp_subevent_connection_established_get_status(const uint8_t *p) {return p[3];}
static void avrcp_subevent_connection_established_get_bd_addr(const uint8_t *p,bd_addr_t addr) {
    for (unsigned i=0;i<6;++i)addr[i]=p[11-i];
}
static uint16_t avrcp_subevent_connection_released_get_avrcp_cid(const uint8_t *p) {return p[3]|p[4]<<8;}
static void bt_volume_select_peer(void) {sink_volume.selected=true;memcpy(sink_volume.peer,selected,6);}
static void bt_volume_relay_reset(bool changed) {(void)changed;}
static void phone_sink_volume_invalidated(void) {++invalidations;}
static const char *bd_addr_to_str(const uint8_t *address) {(void)address;return "peer";}
static void avrcp_target_support_event(uint16_t cid,uint8_t event) {assert(cid);(void)event;}
static void avrcp_target_set_now_playing_info(uint16_t cid,void *info,size_t count) {assert(cid && !info && count==1);}
static void avrcp_controller_enable_notification(uint16_t cid,uint8_t event) {assert(cid);(void)event;++notifications;}
#include "avrcp_reconnect.inc"

static void opened(uint16_t cid,uint8_t status,const uint8_t *address) {
    uint8_t packet[14]={HCI_EVENT_AVRCP_META,12,AVRCP_SUBEVENT_CONNECTION_ESTABLISHED,status,
        (uint8_t)cid,(uint8_t)(cid>>8)};
    for (unsigned i=0;i<6;++i)packet[6+i]=address[5-i];
    avrcp_packet_handler(HCI_EVENT_PACKET,0,packet,sizeof(packet));
}
static void released(uint16_t cid) {
    uint8_t packet[]={HCI_EVENT_AVRCP_META,3,AVRCP_SUBEVENT_CONNECTION_RELEASED,(uint8_t)cid,(uint8_t)(cid>>8)};
    avrcp_packet_handler(HCI_EVENT_PACKET,0,packet,sizeof(packet));
}
static void fixture(void) {
    avrcp_reconnect_reset();memset(&sink_volume,0,sizeof(sink_volume));
    memcpy(cur_active_device,peer,6);memcpy(selected,peer,6);
    media_tracker.avdtp_cid=10;media_tracker.avrcp_cid=0;
    a2dp_accept_incoming=a2dp_is_connected_flag=true;profile_pending=phone_event=false;
    now=connects=disconnects=notifications=invalidations=0;
    next_cid=8;last_disconnected=connect_result=0;setup_already_active=false;
    opened(7,0,peer);
    assert(media_tracker.avrcp_cid==7 && sink_volume.cid==7 && notifications==2);
}
static void due(void) {now+=AVRCP_RETRY_MS;avrcp_reconnect_poll(now);}

static void test_loss_recovers_without_media_reconnect(void) {
    fixture();
    for (unsigned i=0;i<6;++i) {
        uint16_t previous=media_tracker.avrcp_cid;
        released(previous);
        assert(!sink_volume.cid && !media_tracker.avrcp_cid && avrcp_retry.media_cid==10);
        now+=AVRCP_RETRY_MS-1;avrcp_reconnect_poll(now);assert(connects==i);
        avrcp_reconnect_poll(++now);assert(connects==i+1 && avrcp_retry.pending);
        uint16_t current=media_tracker.avrcp_cid;
        opened(current,0,peer);
        assert(sink_volume.cid==current && !avrcp_retry.media_cid);
        released(previous);assert(media_tracker.avrcp_cid==current);
        assert(a2dp_is_connected_flag && media_tracker.avdtp_cid==10 && !disconnects);
    }
    assert(notifications==14 && invalidations==13);
}

static void test_failures_are_bounded(void) {
    fixture();released(7);connect_result=0x0c;
    for (unsigned i=1;i<=AVRCP_RETRY_ATTEMPTS;++i) {due();assert(connects==i && !avrcp_retry.pending);}
    now+=60000;avrcp_reconnect_poll(now);assert(connects==AVRCP_RETRY_ATTEMPTS);

    fixture();released(7);
    for (unsigned i=1;i<=AVRCP_RETRY_ATTEMPTS;++i) {
        due();assert(connects==i && avrcp_retry.pending);
        uint16_t cid=media_tracker.avrcp_cid;
        opened(cid,0x08,peer);assert(!media_tracker.avrcp_cid && !avrcp_retry.pending);
    }
    due();assert(connects==AVRCP_RETRY_ATTEMPTS);
    opened(20,0,peer);released(20);due();assert(connects==AVRCP_RETRY_ATTEMPTS+1);
}

static void test_session_and_stale_event_fences(void) {
    for (unsigned reason=0;reason<5;++reason) {
        fixture();released(7);
        switch(reason) {
            case 0:a2dp_is_connected_flag=false;break;
            case 1:a2dp_accept_incoming=false;break;
            case 2:++media_tracker.avdtp_cid;break;
            case 3:memcpy(selected,other,6);break;
            case 4:memcpy(cur_active_device,other,6);break;
        }
        due();assert(!connects && !avrcp_retry.media_cid);
    }
    fixture();released(7);avrcp_reconnect_reset();due();assert(!connects); // Actual A2DP release cancels even reused media IDs.
    fixture();released(7);due();uint16_t cid=media_tracker.avrcp_cid;
    opened(cid,0x08,other);opened(cid+1,0x08,peer);released(cid+1);
    assert(avrcp_retry.pending && media_tracker.avrcp_cid==cid);
    opened(cid+1,0,other);assert(disconnects==1 && last_disconnected==cid+1);
    assert(avrcp_retry.pending && media_tracker.avrcp_cid==cid);
    phone_event=true;released(cid);opened(cid,0x08,peer);phone_event=false;
    assert(avrcp_retry.pending && media_tracker.avrcp_cid==cid);
    opened(cid,0,peer);assert(!avrcp_retry.media_cid && sink_volume.cid==cid);
}

static void test_sdk_setup_is_not_restarted(void) {
    fixture();media_tracker.avrcp_cid=0;volume_sync_connection(&sink_volume,0,now);
    setup_already_active=true;
    avrcp_reconnect_schedule(true);avrcp_reconnect_poll(now); // Initial A2DP media-open path, not a previous AVRCP loss.
    assert(connects==1 && avrcp_retry.pending && !media_tracker.avrcp_cid);
    opened(40,0x08,peer);setup_already_active=false;due();
    assert(connects==2 && avrcp_retry.pending && media_tracker.avrcp_cid==8);
    opened(8,0,peer);assert(sink_volume.cid==8);
    fixture();released(7);setup_already_active=true;due();
    assert(connects==1 && avrcp_retry.pending && !media_tracker.avrcp_cid);
    for (unsigned i=0;i<60;++i)due();
    assert(connects==1 && !disconnects); // No invalid disconnect of the SDP-stage L2CAP CID zero.
    opened(40,0x08,peer);setup_already_active=false;due();
    assert(connects==2 && avrcp_retry.pending && media_tracker.avrcp_cid==8);
    opened(8,0,peer);assert(sink_volume.cid==8);
    fixture();now=UINT32_MAX-500u;released(7);due();assert(connects==1);
}

int main(void) {
    test_loss_recovers_without_media_reconnect();test_failures_are_bounded();
    test_session_and_stale_event_fences();test_sdk_setup_is_not_restarted();
    puts("AVRCP-only recovery: bounded retries, current-peer/media session fences and preserved audio passed");
}
