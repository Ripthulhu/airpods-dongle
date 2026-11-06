#include <stdint.h>
#define HCI_EVENT_PACKET 4
#define BTSTACK_EVENT_STATE 0x60
#define HCI_STATE_WORKING 2
typedef struct { void (*callback)(uint8_t,uint16_t,uint8_t *,uint16_t); } btstack_packet_callback_registration_t;
void hci_add_event_handler(btstack_packet_callback_registration_t *);
static inline uint8_t btstack_event_state_get_state(const uint8_t *packet) {return packet[2];}
