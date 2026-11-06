#include "btstack.h"
#include "pico/stdlib.h"
#include "radio0076.h"

static btstack_packet_callback_registration_t registration;
static void packet_handler(uint8_t type,uint16_t channel,uint8_t *packet,uint16_t size) {
    (void)channel;
    if(type!=HCI_EVENT_PACKET || size!=3 || packet[0]!=BTSTACK_EVENT_STATE || packet[1]!=1 ||
       btstack_event_state_get_state(packet)!=HCI_STATE_WORKING) return;
    // Registered before all profiles: a failed gate cannot dispatch profile-ready handlers.
    if(!radio0076_after_hci()) panic("Experimental .0076 startup failed: %s",radio0076_error());
}
void radio0076_register_gate(void) {
    registration.callback=packet_handler;
    hci_add_event_handler(&registration);
}
