#ifndef TEST_AACP_BTSTACK_H
#define TEST_AACP_BTSTACK_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define UNUSED(x) ((void)(x))
#define ERROR_CODE_SUCCESS 0
#define HCI_EVENT_PACKET 4
#define L2CAP_DATA_PACKET 6
#define L2CAP_EVENT_CHANNEL_OPENED 0x70
#define L2CAP_EVENT_CHANNEL_CLOSED 0x71
#define L2CAP_EVENT_CAN_SEND_NOW 0x79
typedef uint8_t bd_addr_t[6];
typedef void (*btstack_packet_handler_t)(uint8_t, uint16_t, uint8_t *, uint16_t);
typedef struct btstack_timer_source {
    void (*handler)(struct btstack_timer_source *);
    uint32_t delay;
    bool active;
} btstack_timer_source_t;
void btstack_run_loop_remove_timer(btstack_timer_source_t *timer);
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *timer,
                                      void (*handler)(btstack_timer_source_t *));
void btstack_run_loop_set_timer(btstack_timer_source_t *timer, uint32_t delay);
void btstack_run_loop_add_timer(btstack_timer_source_t *timer);
uint32_t btstack_run_loop_get_time_ms(void);
uint8_t l2cap_create_channel(btstack_packet_handler_t handler, bd_addr_t addr,
                             uint16_t psm, uint16_t mtu, uint16_t *cid);
void l2cap_disconnect(uint16_t cid);
void l2cap_request_can_send_now_event(uint16_t cid);
uint8_t l2cap_send(uint16_t cid, uint8_t *data, uint16_t len);
static inline uint16_t little_endian_read_16(const uint8_t *p, unsigned off) {
    return (uint16_t)(p[off] | (uint16_t)p[off + 1] << 8);
}
static inline uint8_t hci_event_packet_get_type(const uint8_t *p) { return p[0]; }
static inline uint8_t l2cap_event_channel_opened_get_status(const uint8_t *p) { return p[2]; }
static inline uint16_t l2cap_event_channel_opened_get_local_cid(const uint8_t *p) {
    return little_endian_read_16(p, 13);
}
static inline void l2cap_event_channel_opened_get_address(const uint8_t *p, bd_addr_t addr) {
    for (unsigned i = 0; i < 6; ++i) addr[i] = p[8 - i];
}
static inline uint16_t l2cap_event_can_send_now_get_local_cid(const uint8_t *p) {
    return little_endian_read_16(p, 2);
}
static inline uint16_t l2cap_event_channel_closed_get_local_cid(const uint8_t *p) {
    return little_endian_read_16(p, 2);
}
#endif
