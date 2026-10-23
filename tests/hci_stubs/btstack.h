#ifndef TEST_HCI_BTSTACK_H
#define TEST_HCI_BTSTACK_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define UNUSED(x) ((void)(x))
#define ERROR_CODE_SUCCESS 0
#define HCI_EVENT_PACKET 4
#define BTSTACK_EVENT_STATE 0x60
#define HCI_STATE_WORKING 2
#define HCI_EVENT_PIN_CODE_REQUEST 0x16
#define HCI_EVENT_USER_CONFIRMATION_REQUEST 0x33
#define HCI_EVENT_USER_PASSKEY_REQUEST 0x34
#define HCI_EVENT_COMMAND_STATUS 0x0f
#define HCI_OPCODE_HCI_INQUIRY 0x0401
#define GAP_EVENT_INQUIRY_RESULT 0xdc
#define GAP_EVENT_INQUIRY_COMPLETE 0xdd
#define INQUIRY_MODE_RSSI_AND_EIR 2
#define HCI_LINK_TYPE_ACL 1
#define BD_ADDR_TYPE_ACL 0xff
#define SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT 3
#define SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING 4
#define LEVEL_2 2
typedef uint8_t hci_link_type_t;
typedef struct { uint16_t con_handle; } hci_connection_t;
typedef uint8_t bd_addr_t[6];
typedef uint8_t link_key_t[16];
typedef uint8_t link_key_type_t;
typedef struct {
    void (*callback)(uint8_t, uint16_t, uint8_t *, uint16_t);
} btstack_packet_callback_registration_t;

uint8_t gap_inquiry_start(uint8_t duration);
void gap_inquiry_stop(void);
void gap_enable_link_watchdog(uint16_t timeout);
bool hci_non_flushable_packet_boundary_flag_supported(void);
void gap_pin_code_response(bd_addr_t address, const char *pin);
bool gap_get_link_key_for_bd_addr(bd_addr_t address, link_key_t key, link_key_type_t *type);
void gap_local_bd_addr(bd_addr_t address);
void hci_add_event_handler(btstack_packet_callback_registration_t *registration);
void hci_set_master_slave_policy(uint8_t policy);
void hci_set_inquiry_mode(uint8_t mode);
void gap_set_local_name(const char *name);
void gap_discoverable_control(uint8_t discoverable);
void gap_set_class_of_device(uint32_t cod);
void gap_connectable_control(uint8_t connectable);
void gap_ssp_set_io_capability(int capability);
void gap_ssp_set_authentication_requirement(int requirement);
void gap_ssp_set_auto_accept(int auto_accept);
void gap_register_classic_connection_filter(int (*callback)(bd_addr_t, hci_link_type_t));
int gap_pin_code_negative(bd_addr_t address);
int gap_ssp_confirmation_response(const uint8_t address[6]);
int gap_ssp_confirmation_negative(const uint8_t address[6]);
int gap_ssp_passkey_negative(const uint8_t address[6]);
uint32_t btstack_run_loop_get_time_ms(void);
hci_connection_t *hci_connection_for_bd_addr_and_type(const uint8_t address[6], uint8_t type);
uint8_t gap_security_level(uint16_t handle);
uint8_t gap_disconnect(uint16_t handle);
const char *bd_addr_to_str(const uint8_t *address);

static inline uint8_t hci_event_packet_get_type(const uint8_t *p) { return p[0]; }
static inline uint8_t btstack_event_state_get_state(const uint8_t *p) { return p[2]; }
static inline uint8_t hci_event_command_status_get_status(const uint8_t *p) { return p[2]; }
static inline uint16_t hci_event_command_status_get_command_opcode(const uint8_t *p) {
    return (uint16_t)p[4] | (uint16_t)p[5] << 8;
}
static inline void hci_event_pin_code_request_get_bd_addr(const uint8_t *p, bd_addr_t address) {
    for (unsigned i = 0; i < 6; ++i) address[i] = p[7 - i];
}
static inline void hci_event_user_confirmation_request_get_bd_addr(const uint8_t *p, bd_addr_t address) {
    hci_event_pin_code_request_get_bd_addr(p, address);
}
static inline void hci_event_user_passkey_request_get_bd_addr(const uint8_t *p, bd_addr_t address) {
    hci_event_pin_code_request_get_bd_addr(p, address);
}
static inline void gap_event_inquiry_result_get_bd_addr(const uint8_t *p, bd_addr_t address) {
    for (unsigned i = 0; i < 6; ++i) address[i] = p[7 - i];
}
static inline uint32_t gap_event_inquiry_result_get_class_of_device(const uint8_t *p) {
    return (uint32_t)p[9] | (uint32_t)p[10] << 8 | (uint32_t)p[11] << 16;
}
static inline uint8_t gap_event_inquiry_result_get_rssi_available(const uint8_t *p) { return p[14]; }
static inline uint8_t gap_event_inquiry_result_get_rssi(const uint8_t *p) { return p[15]; }
static inline uint8_t gap_event_inquiry_result_get_name_available(const uint8_t *p) { return p[25]; }
static inline uint8_t gap_event_inquiry_result_get_name_len(const uint8_t *p) { return p[26]; }
static inline const uint8_t *gap_event_inquiry_result_get_name(const uint8_t *p) { return p + 27; }
#endif
