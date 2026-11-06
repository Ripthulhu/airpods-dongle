#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdbool.h>
#include "../../src/radio0076/hci_gate.c"
static btstack_packet_callback_registration_t *first;
static jmp_buf stop;
static bool good;
static unsigned calls;
bool radio0076_after_hci(void) {++calls;return good;}
const char *radio0076_error(void) {return "test-failure";}
void hci_add_event_handler(btstack_packet_callback_registration_t *entry) {assert(!first);first=entry;}
void panic(const char *format,...) {(void)format;longjmp(stop,1);}
int main(void) {
    radio0076_register_gate();assert(first);
    uint8_t state[]={BTSTACK_EVENT_STATE,1,HCI_STATE_WORKING};good=true;
    first->callback(0,0,state,3);first->callback(HCI_EVENT_PACKET,0,state,2);assert(!calls);
    first->callback(HCI_EVENT_PACKET,0,state,3);assert(calls==1);
    good=false;
    if(!setjmp(stop)){first->callback(HCI_EVENT_PACKET,0,state,3);assert(!"Failed gate returned to dispatch");}
    assert(calls==2);puts(".0076 HCI-ready event validation and dispatch-stop failure passed");
}
