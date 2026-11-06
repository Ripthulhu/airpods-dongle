#include "sco_inbound.h"
#include "sco_outbound.h"
#define V32(a) (*(volatile uint32_t *)(uintptr_t)(a))
struct bridge_state {
    uint32_t epoch,reset_blocked,resets,reset_complete,drop_wakes,wake_failed;
};
volatile struct bridge_state sco_bridge_state __attribute__((section(".sco_bridge_state"),aligned(4)));
uint32_t sco_out_transport_context __attribute__((section(".sco_bridge_state"),aligned(4)));
struct sco_out_state sco_outbound_state __attribute__((section(".sco_bridge_state"),aligned(4)));
static uint8_t tx_scratch[260] __attribute__((section(".sco_tx_scratch"),aligned(4)));
static uint32_t lock(void){return ((uint32_t (*)(void))(uintptr_t)0x45efu)();}
static void unlock(uint32_t k){((void (*)(uint32_t))(uintptr_t)0x45f7u)(k);}
static int ram(uintptr_t p,uint32_t n){return !(p&3u) && p>=0x200000u && p<=0x220000u-n;}
static void drop_notify(void){
    uint32_t key=lock(),ctx=sco_out_transport_context;
    /* A stale SCO head must not strand a following disconnect/reset event. */
    if(ram(ctx,0x110) && ctx==V32(0x201f1cu) && ((uint32_t (*)(uintptr_t))(uintptr_t)0x440c3u)(ctx) && !V32(0x202f3cu)){
        ((void (*)(void))(uintptr_t)0x4402bu)();
        if(V32(0x202f3cu))++sco_bridge_state.drop_wakes;
        else ++sco_bridge_state.wake_failed;
    }
    unlock(key);
}
const struct sco_out_env sco_outbound_env={
    .endpoints=(uint8_t *)0x20645cu,.handles=(volatile uint16_t *)0x202e84u,
    .host_limit=(volatile uint8_t *)0x202e7cu,.stream_limit=(volatile uint8_t *)0x204482u,
    .epoch=&sco_bridge_state.epoch,.reset_blocked=&sco_bridge_state.reset_blocked,
    .scratch=tx_scratch,.state=&sco_outbound_state,.lock=lock,.unlock=unlock,
    .read_bytes=(void *)0x669ffu,.packet_flags=(void *)0x66717u,.rearm=(void *)0x66653u,
    .preflight=sco_native_b2h_preflight,.commit=sco_native_b2h_commit,
    .wake=sco_native_b2h_wake,.drop_notify=drop_notify
};
void sco_bridge_reset(void){
    sco_bridge_state.reset_blocked=1;
    ++sco_bridge_state.epoch;
    ++sco_bridge_state.resets;
    sco_outbound_cancel((struct sco_out_env *)&sco_outbound_env);
}
void sco_bridge_observe_control(const uint8_t *m){
    if(!ram((uintptr_t)m,14) || V32((uintptr_t)m+4)!=0x7041u)return;
    if(m[8]==0x0e && m[9]==4 && m[11]==3 && m[12]==12 && m[13]==0){
        sco_bridge_state.reset_blocked=0;
        ++sco_bridge_state.reset_complete;
    }
}
