//
// Created by Sean on 8/10/23.
//

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "pico/stdlib.h"

#include "btstack_hci.h"
#include "peer_address.h"
#include "btstack_avdtp_source.h"
#include "btstack_ble_scan.h"
#include "btstack_aacp.h"
#include "phone_sink.h"
#include "phone_call.h"
#include "../tinyusb/uac.h"
#include "../pico_w_led.h"
#include "../slot_storage.h"
#include "btstack_tlv.h"


#define A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS 12

static btstack_packet_callback_registration_t hci_event_callback_registration;

static char device_addr_string[] = "00:00:00:00:00:00";
static bd_addr_t device_addr;
static bd_addr_t device_addr_list[2];
static bool device_addr_bonded[2];

static bool scan_active;
static bool selected_bonded;

#define PHONE_TAG 0x4150484eu // APHN, independent of APSL and BTstack bond records.
#define PHONE_PAIRING_MS 120000u
#define MEDIA_FLUSH_COMPARISON_MS 120000u
static bd_addr_t phone_address, phone_candidate;
static bool phone_bonded, phone_pairing, phone_storage_supported;
static uint32_t phone_pairing_since;
static uint32_t phone_session_generation;
static uint16_t phone_session_handle = HCI_CON_HANDLE_INVALID;
static bd_addr_t phone_session_address;
static uint16_t credit_raw_acl_count, credit_raw_acl_length;
static bool credit_buffer_known;
static struct {
    bt_hci_credit_role_t totals;
    uint32_t last_ms;
    uint16_t handle;
    bool armed;
} credit_roles[2];

static struct {
    bd_addr_t address;
    uint32_t epoch, since, comparison_since;
    uint16_t handle, desired_ms, confirmed_ms;
    uint8_t status;
    bool failed, low_latency, comparison;
} media_flush;
static struct {
    uint32_t epoch, since, link_epoch;
    uint16_t handle, milliseconds;
    bool active;
} media_flush_pending;
// Observe BTstack's initial 200-ms ACK before issuing another command on that ACL.
static struct { uint16_t handle, confirmed_ms; uint32_t epoch; } media_flush_links[2];
static uint32_t media_flush_link_epoch;
static struct {
    bd_addr_t address;
    uint32_t epoch;
    uint16_t handle, requested, normal, desired, confirmed;
    uint8_t status;
    bool baseline, failed;
} media_packets[2];
static struct {
    uint32_t epoch, since;
    uint16_t handle, mask;
    bool active, accepted, baseline;
} media_packet_pending;
static uint32_t media_packet_epoch;
static bool media_packet_game;
static struct {
    bt_hci_radio_diagnostics_t data;
    uint32_t sampled_at[5];
    bd_addr_t address;
    uint8_t next;
} radio;
static struct {
    uint32_t epoch, since;
    uint16_t handle;
    uint8_t field;
    bool active, timed_out;
} radio_pending;
static uint32_t radio_epoch;
// Keep late-reply fencing past the 30-second LMP response window, then recover.
static void controller_reply_wait(uint32_t since) {
    if ((uint32_t)(btstack_run_loop_get_time_ms() - since) >= 35000u)
        panic("Controller command reply stalled for 35000 ms");
}
static const uint16_t radio_opcodes[5]={HCI_OPCODE_HCI_READ_RSSI,HCI_OPCODE_HCI_READ_LINK_QUALITY,
    HCI_OPCODE_HCI_READ_TRANSMIT_POWER_LEVEL,HCI_OPCODE_HCI_READ_TRANSMIT_POWER_LEVEL,0x1406};
static const hci_cmd_t radio_read_afh_map={0x1406,"H"};

typedef struct {
    uint8_t version;
    uint8_t address[6];
} phone_record_t;
_Static_assert(sizeof(phone_record_t) == 7, "phone identity layout changed");

static bool airpods_address(const uint8_t address[6]) {
    return peer_address_valid(address) &&
        (memcmp(address, device_addr, 6) == 0 ||
         memcmp(address, device_addr_list[0], 6) == 0 ||
         memcmp(address, device_addr_list[1], 6) == 0);
}

static void media_flush_forget(void) {
    ++media_flush.epoch;
    media_flush.handle = HCI_CON_HANDLE_INVALID;
    media_flush.desired_ms = 200;
    media_flush.confirmed_ms = 0;
    media_flush.status = UINT8_MAX;
    media_flush.failed = false;
    media_flush.low_latency = media_flush.comparison = false;
    media_packet_game = false;
    media_flush.since = btstack_run_loop_get_time_ms();
    memset(media_flush.address, 0, sizeof(media_flush.address));
}
static uint16_t media_flush_known_timeout(uint16_t handle) {
    for (unsigned i = 0; i < 2; ++i)
        if (media_flush_links[i].handle == handle) return media_flush_links[i].confirmed_ms;
    return 0;
}
static hci_connection_t *media_flush_bind(void) {
    hci_connection_t *connection = peer_address_valid(device_addr) ?
        hci_connection_for_bd_addr_and_type(device_addr, BD_ADDR_TYPE_ACL) : NULL;
    if (connection == NULL || connection->address_type != BD_ADDR_TYPE_ACL || connection->state != OPEN ||
        memcmp(connection->address, device_addr, 6)) {
        if (media_flush.handle != HCI_CON_HANDLE_INVALID) media_flush_forget();
        return NULL;
    }
    if (media_flush.handle != connection->con_handle || memcmp(media_flush.address, device_addr, 6)) {
        media_flush_forget();
        media_flush.handle = connection->con_handle;
        memcpy(media_flush.address, device_addr, 6);
        media_flush.confirmed_ms = media_flush_known_timeout(connection->con_handle);
        if (media_flush.confirmed_ms) {
            media_flush.status = ERROR_CODE_SUCCESS;
        }
    }
    return connection;
}
static void media_flush_policy(void) {
    if (!media_flush.low_latency || (media_flush.comparison &&
        (uint32_t)(btstack_run_loop_get_time_ms() - media_flush.comparison_since) >= MEDIA_FLUSH_COMPARISON_MS))
        media_flush.comparison = false;
    // Retain the native-tested retransmission baseline in both Game Mode states.
    uint16_t desired = 200;
    if (desired != media_flush.desired_ms) {
        media_flush.desired_ms = desired;
        media_flush.failed = false;
    }
}
void bt_hci_airpods_flush_timeout(bool low_latency) {
    hci_connection_t *connection = media_flush_bind();
    media_packet_game = connection != NULL && low_latency;
    if (connection == NULL || !hci_non_flushable_packet_boundary_flag_supported()) return;
    media_flush.low_latency = low_latency;
    media_flush_policy();
}
bool bt_hci_airpods_flush_comparison(bool enabled) {
    hci_connection_t *connection = media_flush_bind();
    media_flush_policy();
    if (!enabled) {
        media_flush.comparison = false;
        media_flush_policy();
        return true;
    }
    if (connection == NULL || !hci_non_flushable_packet_boundary_flag_supported() ||
        !media_flush.low_latency || media_flush.failed) return false;
    if (media_flush.comparison) return true; // Repeated requests cannot extend the bounded comparison.
    if (media_flush_pending.active || media_flush.confirmed_ms != 60) return false;
    media_flush.comparison = true;
    media_flush.comparison_since = btstack_run_loop_get_time_ms();
    media_flush_policy();
    return true;
}
static void media_flush_poll(void) {
    hci_connection_t *connection = media_flush_bind();
    media_flush_policy();
    if (media_flush_pending.active) {
        controller_reply_wait(media_flush_pending.since);
        if ((uint32_t)(btstack_run_loop_get_time_ms() - media_flush_pending.since) >= 1000u &&
            media_flush_pending.epoch == media_flush.epoch) {
            media_flush.failed = true;
            media_flush.status = UINT8_MAX;
        }
        // A late ACK has no transaction ID; do not overlap another same-opcode command.
        return;
    }
    if (connection != NULL && !media_flush_known_timeout(connection->con_handle) &&
        hci_non_flushable_packet_boundary_flag_supported() &&
        (uint32_t)(btstack_run_loop_get_time_ms() - media_flush.since) >= 1000u) {
        media_flush.failed = true;
        media_flush.status = UINT8_MAX;
    }
    if (connection == NULL || !hci_non_flushable_packet_boundary_flag_supported() ||
        !media_flush_known_timeout(connection->con_handle) || media_flush.failed ||
        media_flush.desired_ms == media_flush.confirmed_ms || !hci_can_send_command_packet_now()) return;
    media_flush_pending.epoch = media_flush.epoch;
    media_flush_pending.handle = connection->con_handle;
    media_flush_pending.milliseconds = media_flush.desired_ms;
    media_flush_pending.since = btstack_run_loop_get_time_ms();
    for (unsigned i = 0; i < 2; ++i)
        if (media_flush_links[i].handle == connection->con_handle)
            media_flush_pending.link_epoch = media_flush_links[i].epoch;
    media_flush_pending.active = true;
    uint8_t status = hci_send_cmd(&hci_write_automatic_flush_timeout, connection->con_handle,
                                 media_flush.desired_ms * 8u / 5u);
    if (status != ERROR_CODE_SUCCESS && media_flush_pending.active &&
        media_flush_pending.epoch == media_flush.epoch) {
        media_flush_pending.active = false;
        media_flush.failed = true;
        media_flush.status = status;
    }
}
static void media_flush_event(const uint8_t *p, uint16_t size) {
    if (p[0] == BTSTACK_EVENT_STATE && size >= 3 && p[2] != HCI_STATE_WORKING) {
        media_flush_forget();
        media_flush_pending.active = false;
        media_flush_links[0].handle = media_flush_links[1].handle = HCI_CON_HANDLE_INVALID;
    } else if (p[0] == HCI_EVENT_DISCONNECTION_COMPLETE && size == 6 && p[2] == 0) {
        uint16_t handle = (uint16_t)p[3] | (uint16_t)p[4] << 8;
        for (unsigned i = 0; i < 2; ++i)
            if (media_flush_links[i].handle == handle) media_flush_links[i].handle = HCI_CON_HANDLE_INVALID;
        if (media_flush.handle == handle) media_flush_forget();
    } else if (p[0] == HCI_EVENT_COMMAND_COMPLETE && size == 8 &&
        ((uint16_t)p[3] | (uint16_t)p[4] << 8) == HCI_OPCODE_HCI_WRITE_AUTOMATIC_FLUSH_TIMEOUT) {
        uint16_t handle = (uint16_t)p[6] | (uint16_t)p[7] << 8;
        (void)media_flush_bind();
        if (media_flush_pending.active && media_flush_pending.handle == handle) {
            media_flush_pending.active = false;
            if (p[5] == ERROR_CODE_SUCCESS) {
                for (unsigned i = 0; i < 2; ++i)
                    if (media_flush_links[i].handle == handle &&
                        media_flush_links[i].epoch == media_flush_pending.link_epoch)
                        media_flush_links[i].confirmed_ms = media_flush_pending.milliseconds;
            }
            if (media_flush_pending.epoch != media_flush.epoch) return;
            media_flush.status = p[5];
            media_flush.failed = p[5] != ERROR_CODE_SUCCESS &&
                media_flush.desired_ms == media_flush_pending.milliseconds;
            if (p[5] == ERROR_CODE_SUCCESS) {
                media_flush.confirmed_ms = media_flush_pending.milliseconds;
            }
            return;
        }
        hci_connection_t *connection = hci_connection_for_handle(handle);
        if (connection == NULL || connection->address_type != BD_ADDR_TYPE_ACL ||
            connection->state != OPEN || !airpods_address(connection->address) ||
            media_flush_known_timeout(handle)) return;
        if (p[5] == ERROR_CODE_SUCCESS) {
            for (unsigned i = 0; i < 2; ++i) {
                if (media_flush_links[i].handle != HCI_CON_HANDLE_INVALID) continue;
                media_flush_links[i].handle = handle;
                media_flush_links[i].confirmed_ms = 200;
                if (++media_flush_link_epoch == 0) ++media_flush_link_epoch;
                media_flush_links[i].epoch = media_flush_link_epoch;
                break;
            }
        }
        if (media_flush.handle == handle) {
            media_flush.status = p[5];
            media_flush.failed = p[5] != ERROR_CODE_SUCCESS;
            if (p[5] == ERROR_CODE_SUCCESS) media_flush.confirmed_ms = 200;
        }
    }
}

static int media_packet_index(uint16_t handle) {
    for (unsigned i = 0; i < 2; ++i) if (media_packets[i].handle == handle) return (int)i;
    return -1;
}
static bool media_packet_live(unsigned i) {
    hci_connection_t *connection = hci_connection_for_handle(media_packets[i].handle);
    return connection != NULL && connection->state == OPEN && connection->address_type == BD_ADDR_TYPE_ACL &&
        memcmp(connection->address, media_packets[i].address, 6) == 0;
}
static uint16_t media_packet_allowed(uint16_t mask) {
    // The wire EDR bits forbid types; DM1 is always allowed irrespective of bit 3.
    return ((mask ^ 0x3306u) & ACL_PACKET_TYPES_ALL) | ACL_PACKET_TYPES_DM1;
}
static bool media_packet_equal(uint16_t a, uint16_t b) {
    return (a & ~ACL_PACKET_TYPES_ALL) == 0 && media_packet_allowed(a) == media_packet_allowed(b);
}
static int media_packet_selected(void) {
    hci_connection_t *connection = peer_address_valid(device_addr) ?
        hci_connection_for_bd_addr_and_type(device_addr, BD_ADDR_TYPE_ACL) : NULL;
    if (connection == NULL || connection->state != OPEN || connection->address_type != BD_ADDR_TYPE_ACL ||
        memcmp(connection->address, device_addr, 6)) return -1;
    for (unsigned i = 0; i < 2; ++i)
        if (media_packets[i].handle != HCI_CON_HANDLE_INVALID && !media_packet_live(i))
            media_packets[i].handle = HCI_CON_HANDLE_INVALID;
    int index = media_packet_index(connection->con_handle);
    if (index >= 0) return index;
    for (unsigned i = 0; i < 2; ++i) {
        if (media_packets[i].handle != HCI_CON_HANDLE_INVALID) continue;
        memset(&media_packets[i], 0, sizeof(media_packets[i]));
        media_packets[i].handle = connection->con_handle;
        memcpy(media_packets[i].address, connection->address, 6);
        if (!++media_packet_epoch) ++media_packet_epoch;
        media_packets[i].epoch = media_packet_epoch;
        // This is a safe startup request, not a measurement of the controller's prior mask.
        media_packets[i].requested = hci_usable_acl_packet_types();
        media_packets[i].normal = media_packets[i].confirmed = UINT16_MAX;
        media_packets[i].desired = media_packets[i].requested;
        media_packets[i].status = UINT8_MAX;
        return (int)i;
    }
    return -1;
}
static void media_packet_poll(void) {
    int selected = media_packet_selected();
    for (unsigned i = 0; i < 2; ++i) {
        if (media_packets[i].handle == HCI_CON_HANDLE_INVALID) continue;
        uint16_t desired = media_packets[i].baseline ? media_packets[i].normal : media_packets[i].requested;
        if (media_packets[i].baseline && (int)i == selected && media_packet_game &&
            media_flush.handle == media_packets[i].handle && !memcmp(media_flush.address, media_packets[i].address, 6))
            desired |= ACL_PACKET_TYPES_EDR3;
        if (desired != media_packets[i].desired) {
            media_packets[i].desired = desired;
            media_packets[i].failed = false;
        }
    }
    if (media_packet_pending.active) {
        controller_reply_wait(media_packet_pending.since);
        int index = media_packet_index(media_packet_pending.handle);
        if (index >= 0 && media_packets[index].epoch == media_packet_pending.epoch &&
            (uint32_t)(btstack_run_loop_get_time_ms() - media_packet_pending.since) >= 1000u) {
            media_packets[index].failed = true;
            media_packets[index].status = UINT8_MAX;
        }
        return; // Status has no handle; quarantine this opcode until its terminal event or controller reset.
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (media_packets[i].handle == HCI_CON_HANDLE_INVALID || !media_packet_live(i) || media_packets[i].failed ||
            (!media_packets[i].baseline && (int)i != selected) ||
            (media_packets[i].baseline && media_packet_equal(media_packets[i].confirmed, media_packets[i].desired))) continue;
        if (!hci_can_send_command_packet_now()) return;
        media_packet_pending.epoch = media_packets[i].epoch;
        media_packet_pending.handle = media_packets[i].handle;
        media_packet_pending.mask = media_packets[i].desired;
        media_packet_pending.since = btstack_run_loop_get_time_ms();
        media_packet_pending.baseline = !media_packets[i].baseline;
        media_packet_pending.accepted = false;
        media_packet_pending.active = true;
        uint8_t status = hci_send_cmd(&hci_change_connection_packet_type, media_packet_pending.handle, media_packet_pending.mask);
        if (status != ERROR_CODE_SUCCESS && media_packet_pending.active && media_packet_pending.epoch == media_packets[i].epoch) {
            media_packet_pending.active = false;
            media_packets[i].failed = true;
            media_packets[i].status = status;
        }
        return;
    }
}
static void media_packet_event(const uint8_t *p, uint16_t size) {
    if (p[0] == BTSTACK_EVENT_STATE && size >= 3 && p[2] != HCI_STATE_WORKING) {
        media_packets[0].handle = media_packets[1].handle = HCI_CON_HANDLE_INVALID;
        media_packet_pending.active = false;
    } else if (p[0] == HCI_EVENT_DISCONNECTION_COMPLETE && size == 6 && p[2] == 0) {
        int index = media_packet_index((uint16_t)p[3] | (uint16_t)p[4] << 8);
        if (index >= 0) media_packets[index].handle = HCI_CON_HANDLE_INVALID;
    } else if (media_packet_pending.active && p[0] == HCI_EVENT_COMMAND_STATUS && size == 6 &&
        ((uint16_t)p[4] | (uint16_t)p[5] << 8) == HCI_OPCODE_HCI_CHANGE_CONNECTION_PACKET_TYPE) {
        media_packet_pending.accepted = p[2] == ERROR_CODE_SUCCESS;
        if (media_packet_pending.accepted) return;
        media_packet_pending.active = false;
        int index = media_packet_index(media_packet_pending.handle);
        if (index >= 0 && media_packets[index].epoch == media_packet_pending.epoch) {
            media_packets[index].status = p[2];
            media_packets[index].failed = media_packet_pending.baseline ||
                media_packet_equal(media_packets[index].desired, media_packet_pending.mask);
        }
    } else if (media_packet_pending.active && media_packet_pending.accepted &&
        p[0] == HCI_EVENT_CONNECTION_PACKET_TYPE_CHANGED && size == 7 &&
        ((uint16_t)p[3] | (uint16_t)p[4] << 8) == media_packet_pending.handle) {
        media_packet_pending.active = false;
        int index = media_packet_index(media_packet_pending.handle);
        if (index < 0 || media_packets[index].epoch != media_packet_pending.epoch || !media_packet_live((unsigned)index)) return;
        uint16_t mask = (uint16_t)p[5] | (uint16_t)p[6] << 8;
        bool match = p[2] == 0 && (mask & ~ACL_PACKET_TYPES_ALL) == 0;
        if (match && media_packet_pending.baseline)
            match = (media_packet_allowed(mask) & ~media_packet_allowed(media_packet_pending.mask)) == 0;
        else if (match) match = media_packet_equal(mask, media_packet_pending.mask);
        if (p[2] == 0) media_packets[index].confirmed = mask;
        media_packets[index].status = p[2];
        media_packets[index].failed = !match && (media_packet_pending.baseline ||
            media_packet_equal(media_packets[index].desired, media_packet_pending.mask));
        if (match && media_packet_pending.baseline) {
            media_packets[index].normal = mask;
            media_packets[index].baseline = true;
        }
    }
}

static void radio_reset(void) {
    memset(&radio,0,sizeof(radio));
    if (!++radio_epoch) ++radio_epoch;
    radio.data.epoch=radio_epoch;
    radio.data.handle=HCI_CON_HANDLE_INVALID;
}
static bool radio_bind(void) {
    hci_connection_t *connection=peer_address_valid(device_addr) ?
        hci_connection_for_bd_addr_and_type(device_addr,BD_ADDR_TYPE_ACL) : NULL;
    if (!connection || connection->state!=OPEN || connection->address_type!=BD_ADDR_TYPE_ACL ||
        memcmp(connection->address,device_addr,6)) {
        if (radio.data.handle!=HCI_CON_HANDLE_INVALID) radio_reset();
        return false;
    }
    if (radio.data.handle!=connection->con_handle || memcmp(radio.address,connection->address,6)) {
        radio_reset();radio.data.handle=connection->con_handle;
        memcpy(radio.address,connection->address,6);
    }
    return true;
}
void bt_hci_radio_active(bool active) {radio.data.active=radio_bind() && active;}
bool bt_hci_radio_diagnostics(bt_hci_radio_diagnostics_t *out) {
    if (!out) return false;
    bool connected=radio_bind();
    *out=radio.data;out->pending=radio_pending.active;
    uint32_t now=btstack_run_loop_get_time_ms();
    for (unsigned i=0;i<5;++i)
        out->age_ms[i]=(out->valid&(1u<<i)) ? now-radio.sampled_at[i] : UINT32_MAX;
    return connected;
}
static void radio_error(uint8_t status) {
    if (radio_pending.epoch!=radio.data.epoch) return;
    uint8_t bit=1u<<radio_pending.field;
    radio.data.valid&=~bit;
    if (status==0x01 || status==0x11) radio.data.unavailable|=bit;
    else radio.data.failed|=bit;
}
static void radio_poll(void) {
    bool connected=radio_bind();
    uint32_t now=btstack_run_loop_get_time_ms();
    if (radio_pending.active) {
        controller_reply_wait(radio_pending.since);
        if (!radio_pending.timed_out && (uint32_t)(now-radio_pending.since)>=1000u) {
            radio_error(UINT8_MAX);radio_pending.timed_out=true;
        }
        return; // A late same-opcode response must not populate a replacement ACL or another power query.
    }
    if (!connected || !radio.data.active || !hci_can_send_command_packet_now()) return;
    for (unsigned offset=0;offset<5;++offset) {
        unsigned field=(radio.next+offset)%5;
        uint8_t bit=1u<<field;
        if ((radio.data.failed|radio.data.unavailable)&bit) continue;
        if (radio.data.valid&bit) {
            if (field==3 || (uint32_t)(now-radio.sampled_at[field])<(field<2?2000u:10000u)) continue;
        }
        radio_pending.epoch=radio.data.epoch;radio_pending.handle=radio.data.handle;
        radio_pending.field=(uint8_t)field;radio_pending.since=now;radio_pending.active=true;
        radio_pending.timed_out=false;
        radio.next=(field+1)%5;
        uint8_t status;
        if (field<2) status=hci_send_cmd(field?&hci_read_link_quality:&hci_read_rssi,radio_pending.handle);
        else if (field<4) status=hci_send_cmd(&hci_read_transmit_power_level,radio_pending.handle,field==3?1:0);
        else status=hci_send_cmd(&radio_read_afh_map,radio_pending.handle);
        if (status && radio_pending.active && radio_pending.epoch==radio.data.epoch) {
            radio_error(status);radio_pending.active=false;
        }
        return;
    }
}
static void radio_event(const uint8_t *p,uint16_t size) {
    if (p[0]==BTSTACK_EVENT_STATE && size>=3 && p[2]!=HCI_STATE_WORKING) {
        radio_reset();radio_pending.active=false;return;
    }
    if (p[0]==HCI_EVENT_DISCONNECTION_COMPLETE && size==6 && p[2]==0 &&
        ((uint16_t)p[3]|(uint16_t)p[4]<<8)==radio.data.handle) {radio_reset();return;}
    bool connected=radio_bind();
    if (connected && p[0]==0x11 && size==4 && ((uint16_t)p[2]|(uint16_t)p[3]<<8)==radio.data.handle)
        ++radio.data.flush_events;
    if (connected && p[0]==HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS && size>=3 && size==3u+4u*p[2])
        for (unsigned i=0;i<p[2];++i)
            if ((((uint16_t)p[3+4*i]|(uint16_t)p[4+4*i]<<8)&0x0fffu)==radio.data.handle)
                radio.data.completed_packets+=(uint16_t)p[5+4*i]|(uint16_t)p[6+4*i]<<8;
    if (!radio_pending.active || p[0]!=HCI_EVENT_COMMAND_COMPLETE || size<6 ||
        ((uint16_t)p[3]|(uint16_t)p[4]<<8)!=radio_opcodes[radio_pending.field]) return;
    if (p[5]) {radio_error(p[5]);radio_pending.active=false;return;}
    uint16_t expected=radio_pending.field==4?19:9;
    if (size!=expected || ((uint16_t)p[6]|(uint16_t)p[7]<<8)!=radio_pending.handle) return;
    radio_pending.active=false;
    if (radio_pending.epoch!=radio.data.epoch) return;
    unsigned field=radio_pending.field;
    if (field==4 && (p[8]>1 || (p[8] && (p[18]&0x80)))) {radio_error(UINT8_MAX);return;}
    if (field==0) radio.data.rssi=(int8_t)p[8];
    else if (field==1) radio.data.quality=p[8];
    else if (field==2) radio.data.tx_power=(int8_t)p[8];
    else if (field==3) radio.data.max_tx_power=(int8_t)p[8];
    else {
        radio.data.afh_mode=p[8];memset(radio.data.afh_map,0,10);
        if (p[8]) memcpy(radio.data.afh_map,p+9,10);
    }
    radio.sampled_at[field]=btstack_run_loop_get_time_ms();
    radio.data.failed&=~(1u<<field);radio.data.valid|=1u<<field;
}

static void phone_load(void) {
    const btstack_tlv_t *tlv;
    void *context;
    btstack_tlv_get_instance(&tlv, &context);
    phone_record_t record;
    memset(phone_address, 0, sizeof(phone_address));
    memset(phone_candidate, 0, sizeof(phone_candidate));
    phone_bonded = phone_pairing = false;
    phone_storage_supported = tlv != NULL;
    if (tlv == NULL) return;
    int length = tlv->get_tag(context, PHONE_TAG, NULL, 0);
    if (length == 0) return;
    phone_storage_supported = length == (int)sizeof(record) &&
        tlv->get_tag(context, PHONE_TAG, (uint8_t *)&record, sizeof(record)) == (int)sizeof(record) &&
        record.version == 1 && peer_address_valid(record.address);
    if (!phone_storage_supported || airpods_address(record.address)) return;
    memcpy(phone_address, record.address, 6);
    link_key_t key;
    link_key_type_t type;
    phone_bonded = gap_get_link_key_for_bd_addr(phone_address, key, &type);
}

static bool phone_pairing_audio_busy(void) {
    return get_usb_streaming() || usb_mic_requested() || aacp_mic_active() || phone_sink_active() || phone_call_active();
}

static uint16_t credit_u16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t credit_add(uint32_t value, uint32_t added) {
    return value > UINT32_MAX - added ? UINT32_MAX : value + added;
}
static void credit_event(const uint8_t *p, uint16_t size) {
    if (p[0] == HCI_EVENT_COMMAND_COMPLETE && size >= 13 &&
        credit_u16(p + 3) == HCI_OPCODE_HCI_READ_BUFFER_SIZE && p[5] == 0) {
        credit_raw_acl_length = credit_u16(p + 6);
        credit_raw_acl_count = credit_u16(p + 9);
        credit_buffer_known = true;
    } else if (p[0] == HCI_EVENT_DISCONNECTION_COMPLETE && size == 6 && p[2] == 0) {
        uint16_t handle = credit_u16(p + 3) & 0x0fffu;
        for (unsigned i = 0; i < 2; ++i)
            if (credit_roles[i].handle == handle) credit_roles[i].armed = false;
    } else if (p[0] == BTSTACK_EVENT_STATE && size >= 3 && p[2] != HCI_STATE_WORKING) {
        credit_buffer_known = false;
        credit_roles[0].armed = credit_roles[1].armed = false;
    } else if (p[0] == HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS && size >= 3 && size == 3u + p[2] * 4u) {
        uint32_t now = btstack_run_loop_get_time_ms();
        bool active = phone_pairing_audio_busy();
        for (unsigned offset = 3; offset < size; offset += 4) {
            uint16_t handle = credit_u16(p + offset) & 0x0fffu;
            hci_connection_t *connection = hci_connection_for_handle(handle);
            if (connection == NULL || connection->address_type != BD_ADDR_TYPE_ACL) continue;
            int role = memcmp(connection->address, device_addr, 6) == 0 ? 0 :
                ((peer_address_valid(phone_address) && memcmp(connection->address, phone_address, 6) == 0) ||
                 (peer_address_valid(phone_candidate) && memcmp(connection->address, phone_candidate, 6) == 0)) ? 1 : -1;
            if (role < 0) continue;
            uint16_t completed = credit_u16(p + offset + 2);
            credit_roles[role].totals.events = credit_add(credit_roles[role].totals.events, 1);
            credit_roles[role].totals.packets = credit_add(credit_roles[role].totals.packets, completed);
            if (!completed) continue;
            if (active && credit_roles[role].armed && credit_roles[role].handle == handle) {
                uint32_t gap = now - credit_roles[role].last_ms;
                if (gap > credit_roles[role].totals.max_active_gap_ms)
                    credit_roles[role].totals.max_active_gap_ms = gap;
            }
            credit_roles[role].handle = handle;
            credit_roles[role].last_ms = now;
            credit_roles[role].armed = active;
        }
    }
}

static void credit_link(const uint8_t address[6], uint16_t *handle, uint8_t *role) {
    *handle = HCI_CON_HANDLE_INVALID;
    *role = UINT8_MAX;
    if (!peer_address_valid(address)) return;
    hci_connection_t *connection = hci_connection_for_bd_addr_and_type(address, BD_ADDR_TYPE_ACL);
    if (connection == NULL || connection->state != OPEN) return;
    *handle = connection->con_handle;
    if (connection->role == HCI_ROLE_MASTER || connection->role == HCI_ROLE_SLAVE)
        *role = (uint8_t)connection->role;
}

bool bt_hci_credit_diagnostics(bt_hci_credit_diagnostics_t *out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    out->raw_acl_count = credit_raw_acl_count;
    out->raw_acl_length = credit_raw_acl_length;
    out->airpods = credit_roles[0].totals;
    out->phone = credit_roles[1].totals;
    out->media_flush_desired_ms = media_flush.desired_ms;
    out->media_flush_confirmed_ms = media_flush.confirmed_ms;
    out->media_flush_status = media_flush.status;
    out->media_flush_pending = media_flush_pending.active;
    out->media_flush_failed = media_flush.failed;
    credit_link(device_addr, &out->airpods_handle, &out->airpods_local_role);
    credit_link(phone_address, &out->phone_handle, &out->phone_local_role);
    if (out->phone_handle == HCI_CON_HANDLE_INVALID)
        credit_link(phone_candidate, &out->phone_handle, &out->phone_local_role);
    out->media_packet_requested = out->media_packet_normal = out->media_packet_desired =
        out->media_packet_confirmed = UINT16_MAX;
    out->media_packet_status = UINT8_MAX;
    out->media_packet_pending = media_packet_pending.active;
    int index = out->airpods_handle != HCI_CON_HANDLE_INVALID ? media_packet_index(out->airpods_handle) : -1;
    if (index >= 0 && media_packet_live((unsigned)index)) {
        out->media_packet_requested = media_packets[index].requested;
        out->media_packet_normal = media_packets[index].normal;
        out->media_packet_desired = media_packets[index].desired;
        out->media_packet_confirmed = media_packets[index].confirmed;
        out->media_packet_status = media_packets[index].status;
        out->media_packet_failed = media_packets[index].failed;
    }
    if (credit_buffer_known) {
        out->effective_acl_limit = credit_raw_acl_count < MAX_NR_CONTROLLER_ACL_BUFFERS ?
            credit_raw_acl_count : MAX_NR_CONTROLLER_ACL_BUFFERS;
        out->free_acl_slots = hci_number_free_acl_slots_for_connection_type(BD_ADDR_TYPE_ACL);
        out->outstanding_acl_packets = out->free_acl_slots <= out->effective_acl_limit ?
            out->effective_acl_limit - out->free_acl_slots : 0;
        out->packet_buffer_reserved = hci_is_packet_buffer_reserved();
    }
    return credit_buffer_known;
}

bool bt_hci_phone_pairing_busy(void) {
    return phone_pairing_audio_busy() || dongle_playback_transitioning();
}

bool bt_hci_phone_pairing_active(void) {
    return phone_pairing &&
        // An idle suspend/start must not revoke an already opened pairing window.
        !phone_pairing_audio_busy() &&
        (uint32_t)(btstack_run_loop_get_time_ms() - phone_pairing_since) < PHONE_PAIRING_MS;
}

static void phone_pairing_stop(void) {
    bd_addr_t candidate;
    memcpy(candidate, phone_candidate, sizeof(candidate));
    phone_pairing = false;
    memset(phone_candidate, 0, sizeof(phone_candidate));
    gap_discoverable_control(0);
    // Close only the uncommitted candidate; an existing phone bond remains usable.
    if (peer_address_valid(candidate) &&
        !(phone_bonded && memcmp(candidate, phone_address, 6) == 0)) {
        hci_connection_t *connection = hci_connection_for_bd_addr_and_type(candidate, BD_ADDR_TYPE_ACL);
        if (connection != NULL) gap_disconnect(connection->con_handle);
    }
}

void bt_hci_phone_poll(void) {
    media_packet_poll();
    media_flush_poll();
    radio_poll();
    if (!phone_pairing_audio_busy()) credit_roles[0].armed = credit_roles[1].armed = false;
    if (phone_pairing && !bt_hci_phone_pairing_active()) phone_pairing_stop();
}

bool bt_hci_phone_pairing_start(void) {
    if (bt_hci_phone_pairing_busy()) {
        bt_hci_phone_poll();
        printf("Pause audio before pairing a new phone\n");
        return false;
    }
    if (!phone_storage_supported) {
        printf("Phone identity storage unavailable or unsupported\n");
        return false;
    }
    if (phone_pairing) phone_pairing_stop();
    phone_pairing_since = btstack_run_loop_get_time_ms();
    phone_pairing = true;
    gap_discoverable_control(1);
    printf("Phone pairing available for 120 seconds\n");
    return true;
}

bool bt_hci_phone_peer_allowed(const uint8_t address[6]) {
    if (address == NULL || !peer_address_valid(address) || airpods_address(address)) return false;
    if (phone_bonded && memcmp(address, phone_address, 6) == 0) return true;
    bt_hci_phone_poll();
    return bt_hci_phone_pairing_active() && memcmp(address, phone_candidate, 6) == 0;
}

bool bt_hci_phone_peer_connected(const uint8_t address[6]) {
    if (!bt_hci_phone_peer_allowed(address)) return false;
    hci_connection_t *connection = hci_connection_for_bd_addr_and_type(address, BD_ADDR_TYPE_ACL);
    if (connection == NULL || gap_security_level(connection->con_handle) < LEVEL_2) return false;
    link_key_t key;
    link_key_type_t type;
    if (!gap_get_link_key_for_bd_addr((uint8_t *)address, key, &type)) return false;
    if (phone_bonded && memcmp(address, phone_address, 6) == 0) return true;
    const btstack_tlv_t *tlv;
    void *context;
    btstack_tlv_get_instance(&tlv, &context);
    if (!phone_storage_supported || tlv == NULL) return false;
    phone_record_t record = {.version = 1}, verify;
    memcpy(record.address, address, 6);
    if (!bt_hci_phone_pairing_active()) {
        bt_hci_phone_poll();
        return false;
    }
    // shortcut: BTstack may already have stored an in-flight link-key notification
    // before our callback; coordinating that boundary needs a separate SDK change.
    if (tlv->store_tag(context, PHONE_TAG, (const uint8_t *)&record, sizeof(record)) != 0 ||
        tlv->get_tag(context, PHONE_TAG, NULL, 0) != (int)sizeof(verify) ||
        tlv->get_tag(context, PHONE_TAG, (uint8_t *)&verify, sizeof(verify)) != (int)sizeof(verify) ||
        memcmp(&record, &verify, sizeof(record)) != 0) return false;
    memcpy(phone_address, address, 6);
    phone_bonded = true;
    phone_pairing = false;
    memset(phone_candidate, 0, sizeof(phone_candidate));
    gap_discoverable_control(0);
    return true;
}

bool bt_hci_phone_address(uint8_t address[6]) {
    if (address == NULL || !phone_bonded || !peer_address_valid(phone_address)) return false;
    memcpy(address, phone_address, 6);
    return true;
}

uint32_t bt_hci_phone_session(const uint8_t address[6]) {
    if (!bt_hci_phone_peer_connected(address)) return 0;
    hci_connection_t *connection = hci_connection_for_bd_addr_and_type(address, BD_ADDR_TYPE_ACL);
    if (connection == NULL || connection->state != OPEN) return 0;
    if (phone_session_handle != connection->con_handle || memcmp(phone_session_address, address, 6)) {
        if (++phone_session_generation == 0) ++phone_session_generation;
        phone_session_handle = connection->con_handle;
        memcpy(phone_session_address, address, 6);
    }
    return phone_session_generation;
}

static int accept_classic_peer(bd_addr_t address, hci_link_type_t link_type) {
    if (!peer_address_valid(address)) return 0;
    if (link_type != HCI_LINK_TYPE_ACL)
        return (link_type == HCI_LINK_TYPE_SCO || link_type == HCI_LINK_TYPE_ESCO) &&
            phone_call_sco_allowed(address) && bt_hci_phone_peer_connected(address);
    if (peer_address_valid(device_addr) && memcmp(address, device_addr, 6) == 0) return 1;
    if (bt_hci_phone_peer_allowed(address)) return 1;
    if (!bt_hci_phone_pairing_active() || airpods_address(address) ||
        peer_address_valid(phone_candidate)) return 0;
    memcpy(phone_candidate, address, 6);
    return 1;
}

static bool pairing_peer_allowed(const uint8_t address[6]) {
    bt_hci_phone_poll();
    return (peer_address_valid(device_addr) && memcmp(address, device_addr, 6) == 0) ||
        (bt_hci_phone_pairing_active() && bt_hci_phone_peer_allowed(address));
}

static bool inquiry_name_word(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9');
}

static bool inquiry_airpods(const uint8_t *packet, uint16_t size, const uint8_t address[6]) {
    if (size < 27 || !peer_address_valid(address)) return false;
    if (gap_event_inquiry_result_get_device_id_available(packet)) {
        uint16_t source = gap_event_inquiry_result_get_device_id_vendor_id_source(packet);
        uint16_t vendor = gap_event_inquiry_result_get_device_id_vendor_id(packet);
        return (source == 1 && vendor == 0x004c) || (source == 2 && vendor == 0x05ac);
    }
    // Keep renamed, previously authenticated peers eligible when EIR omits Device ID.
    for (unsigned i = 0; i < 2; ++i)
        if (device_addr_bonded[i] && memcmp(address, device_addr_list[i], 6) == 0) return true;
    if (!gap_event_inquiry_result_get_name_available(packet)) return false;
    const uint8_t *name = gap_event_inquiry_result_get_name(packet);
    unsigned length = gap_event_inquiry_result_get_name_len(packet);
    if (length > size - 27u) return false;
    static const char airpods[] = "airpods";
    for (unsigned i = 0; i + sizeof(airpods) - 1u <= length; ++i) {
        if ((i && inquiry_name_word(name[i - 1])) ||
            (i + sizeof(airpods) - 1u < length && inquiry_name_word(name[i + sizeof(airpods) - 1u]))) continue;
        unsigned j;
        for (j = 0; j < sizeof(airpods) - 1u; ++j) {
            uint8_t c = name[i + j];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != (uint8_t)airpods[j]) break;
        }
        if (j == sizeof(airpods) - 1u) return true;
    }
    return false;
}


const char * get_device_addr_string(){
    return device_addr_string;
}

bd_addr_t * get_device_addr(){
    return &device_addr;
}

void bt_hci_retire_selected_peer(void) {
    // Reusing an encrypted ACL after deleting its stored key would lose the bond at reboot.
    hci_connection_t *connection = peer_address_valid(device_addr) ?
        hci_connection_for_bd_addr_and_type(device_addr, BD_ADDR_TYPE_ACL) : NULL;
    if (connection && connection->state == OPEN && connection->address_type == BD_ADDR_TYPE_ACL &&
        !memcmp(connection->address, device_addr, 6))
        gap_disconnect(connection->con_handle);
    memset(device_addr, 0, sizeof(device_addr));
    snprintf(device_addr_string, sizeof(device_addr_string), "%s", bd_addr_to_str(device_addr));
    selected_bonded = false;
    // Keep authenticated discovery hints for a renamed peer being explicitly repaired.
}

bd_addr_t * get_device_addr_from_list(uint8_t i){
    return i < 2 ? &device_addr_list[i] : NULL;
}

bool gap_start_scanning(void){
    if (scan_active) return true;
    aacp_routing_override();
    printf("Start scanning...\n");
    ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, true);
    scan_active = gap_inquiry_start(A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS) == ERROR_CODE_SUCCESS;
    if (!scan_active) ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, false);
    set_led_mode_off();
    if (scan_active) set_led_mode_pairing();
    return scan_active;
}


static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    if (packet_type != HCI_EVENT_PACKET || size < 2 || (unsigned)packet[1] + 2 != size) return;
    credit_event(packet, size);
    media_flush_event(packet, size);
    media_packet_event(packet, size);
    radio_event(packet,size);
    dongle_playback_controller_event(packet,size);
    uint8_t status;
    UNUSED(status);

    bd_addr_t address;
    uint32_t cod;

    // Service Class: Rendering | Audio, Major Device Class: Audio
    const uint32_t bluetooth_speaker_cod = 0x200000 | 0x040000 | 0x000400;

    switch (hci_event_packet_get_type(packet)){
        case  BTSTACK_EVENT_STATE:
            if (size < 3) return;
            ble_scan_controller_ready(btstack_event_state_get_state(packet) == HCI_STATE_WORKING);
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) {
                phone_session_handle = HCI_CON_HANDLE_INVALID;
                scan_active = false;
                phone_pairing = false;
                memset(phone_candidate, 0, sizeof(phone_candidate));
                gap_discoverable_control(0);
                ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, false);
                return;
            }
            // Control must remain reliable when only media is allowed to expire.
            gap_enable_link_watchdog(hci_non_flushable_packet_boundary_flag_supported() ? 200 : 0);
            // Startup is passive; the saved AirPods may initiate their own return.
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (size < 6) return;
            if (packet[2] == ERROR_CODE_SUCCESS &&
                (credit_u16(packet + 3) & 0x0fffu) == phone_session_handle)
                phone_session_handle = HCI_CON_HANDLE_INVALID;
            printf("[HCI] disconnect: status=0x%02x reason=0x%02x\n", packet[2], packet[5]);
            break;
        case HCI_EVENT_COMMAND_STATUS:
            if (size < 6 || !scan_active ||
                hci_event_command_status_get_command_opcode(packet) != HCI_OPCODE_HCI_INQUIRY ||
                hci_event_command_status_get_status(packet) == ERROR_CODE_SUCCESS) break;
            scan_active = false;
            ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, false);
            set_led_mode_off();
            printf("Pairing inquiry rejected: 0x%02x\n", hci_event_command_status_get_status(packet));
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            if (size < 8) return;
            hci_event_pin_code_request_get_bd_addr(packet, address);
            if (peer_address_valid(device_addr) && memcmp(address, device_addr, 6) == 0)
                gap_pin_code_response(address, "0000");
            else gap_pin_code_negative(address);
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            if (size < 12) return;
            hci_event_user_confirmation_request_get_bd_addr(packet, address);
            if (pairing_peer_allowed(address)) gap_ssp_confirmation_response(address);
            else gap_ssp_confirmation_negative(address);
            break;
        case HCI_EVENT_USER_PASSKEY_REQUEST:
            if (size < 8) return;
            hci_event_user_passkey_request_get_bd_addr(packet, address);
            gap_ssp_passkey_negative(address); // There is no local passkey input.
            break;
        case GAP_EVENT_INQUIRY_RESULT:
            if (size < 27) return;
            gap_event_inquiry_result_get_bd_addr(packet, address);
            // print info
            printf("Device found: %s ",  bd_addr_to_str(address));
            cod = gap_event_inquiry_result_get_class_of_device(packet);
            printf("with COD: %06" PRIx32, cod);
            if (gap_event_inquiry_result_get_rssi_available(packet)){
                printf(", rssi %d dBm", (int8_t) gap_event_inquiry_result_get_rssi(packet));
            }
            if (gap_event_inquiry_result_get_name_available(packet)){
                char name_buffer[240];
                size_t name_len = gap_event_inquiry_result_get_name_len(packet);
                if (name_len > size - 27u) break;
                if (name_len >= sizeof(name_buffer)) name_len = sizeof(name_buffer) - 1;
                memcpy(name_buffer, gap_event_inquiry_result_get_name(packet), name_len);
                name_buffer[name_len] = 0;
                printf(", name '%s'", name_buffer);
            }
            printf("\n");
            if (scan_active && (cod & bluetooth_speaker_cod) == bluetooth_speaker_cod &&
                memcmp(address, phone_address, 6) != 0 && memcmp(address, phone_candidate, 6) != 0 &&
                inquiry_airpods(packet, size, address)){
                scan_active = false;
                gap_inquiry_stop();
                ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, false);

                uint8_t currect_slot = read_uint8_last_flash();
                bool saved = (currect_slot == 1 && write_slot1_mac(address)) ||
                             (currect_slot == 2 && write_slot2_mac(address));
                if (!saved) {
                    set_led_mode_off();
                    printf("Pairing address could not be saved\n");
                    break;
                }
                memcpy(device_addr, address, 6);
                snprintf(device_addr_string, sizeof(device_addr_string), "%s", bd_addr_to_str(address));
                printf("Bluetooth speaker detected, trying to connect to %s...\n", bd_addr_to_str(device_addr));
                if (aacp_takeover_request(device_addr)) avdtp_source_establish_stream();
            }
            break;
        case GAP_EVENT_INQUIRY_COMPLETE:
            if (scan_active){
                printf("No Bluetooth speakers found, scanning again...\n");
                scan_active = gap_inquiry_start(A2DP_SOURCE_DEMO_INQUIRY_DURATION_1280MS) == ERROR_CODE_SUCCESS;
                if (!scan_active) {
                    ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, false);
                    set_led_mode_off();
                }
            }
            break;
        default:
            break;
    }
}


void get_link_keys(void){
    read_slot1_mac(device_addr_list[0]);
    read_slot2_mac(device_addr_list[1]);
    memset(device_addr_bonded, 0, sizeof(device_addr_bonded));
    link_key_t link_key;
    link_key_type_t type;
    for (unsigned i = 0; i < 2; ++i) {
        if (peer_address_valid(device_addr_list[i])) {
            device_addr_bonded[i] = gap_get_link_key_for_bd_addr(device_addr_list[i], link_key, &type);
        }
    }
    // Slot identity is authoritative; key iteration order cannot identify a peer.
    selected_bonded = peer_select(device_addr_list, device_addr_bonded, read_uint8_last_flash(), device_addr);
    snprintf(device_addr_string, sizeof(device_addr_string), "%s", bd_addr_to_str(device_addr));
}


static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    bd_addr_t local_addr;
    if (packet_type != HCI_EVENT_PACKET || size < 2 || (unsigned)packet[1] + 2 != size) return;
    switch(hci_event_packet_get_type(packet)){
        case BTSTACK_EVENT_STATE:
            if (size < 3) return;
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
            gap_local_bd_addr(local_addr);
            printf("BTstack up and running on %s.\n", bd_addr_to_str(local_addr));
            break;
        default:
            break;
    }
}


// separate registration for the state logger — reusing hci_event_callback_registration
// for both handlers means the second hci_add_event_handler() call is rejected as a
// duplicate list item and only the callback assigned last would ever run
static btstack_packet_callback_registration_t hci_state_callback_registration;

void bt_hci_init(void){
    media_flush_forget();
    media_flush_pending.active = false;
    media_flush_links[0].handle = media_flush_links[1].handle = HCI_CON_HANDLE_INVALID;
    media_packets[0].handle = media_packets[1].handle = HCI_CON_HANDLE_INVALID;
    media_packet_pending.active = false;
    radio_reset();radio_pending.active=false;
    phone_session_generation = 0;
    phone_session_handle = HCI_CON_HANDLE_INVALID;
    memset(phone_session_address, 0, sizeof(phone_session_address));
    credit_buffer_known = false;
    credit_raw_acl_count = credit_raw_acl_length = 0;
    memset(credit_roles, 0, sizeof(credit_roles));

    // inform about BTstack state
    hci_state_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_state_callback_registration);

    // Request role change on reconnecting headset to always use them in slave mode
    hci_set_master_slave_policy(0);
    // enabled EIR
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    // Set local name with a template Bluetooth address, that will be automatically
    // replaced with a actual address once it is available, i.e. when BTstack boots
    // up and starts talking to a Bluetooth module.
    gap_set_local_name("AirPods Dongle");
    gap_discoverable_control(0);
    gap_set_class_of_device(0x240408);
    gap_connectable_control(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(0);
    gap_register_classic_connection_filter(accept_classic_peer);

    /* Register for HCI events */
    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    get_link_keys();
    phone_load();

}
