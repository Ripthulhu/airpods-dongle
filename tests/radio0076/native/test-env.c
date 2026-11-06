/* Synthetic emulator fixture only. Not a production address assignment. */
#include "sco_outbound.h"
struct sco_out_state out_state;
uint8_t out_scratch[SCO_OUT_SCRATCH];
volatile uint32_t out_epoch=1;
volatile uint32_t out_reset_blocked;
uint32_t sco_out_transport_context;
struct sco_out_env sco_outbound_env={
 .endpoints=(uint8_t *)0x20645c,.handles=(volatile uint16_t *)0x202e84,
 .host_limit=(volatile uint8_t *)0x204482,.stream_limit=(volatile uint8_t *)0x202e7c,
 .epoch=&out_epoch,.reset_blocked=&out_reset_blocked,.scratch=out_scratch,.state=&out_state,
 .lock=(uint32_t(*)(void))0x45ef,.unlock=(void(*)(uint32_t))0x45f7,
 .read_bytes=(uint32_t(*)(void *,void *,uint32_t))0x669ff,
 .packet_flags=(uint32_t(*)(void *,uint32_t))0x66717,
 .rearm=(void(*)(void *))0x66653,
 .preflight=sco_native_b2h_preflight,.commit=sco_native_b2h_commit,.wake=sco_native_b2h_wake,
 .drop_notify=(void(*)(void))0x88021
};

/* Production control observer is exercised separately by reset.py. */
void sco_bridge_observe_control(const uint8_t *message) {(void)message;}
