#ifndef AIRPODS_RUNTIME_WATCH_H
#define AIRPODS_RUNTIME_WATCH_H
#include <stdint.h>
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "hardware/structs/watchdog.h"
#include "hardware/timer.h"
#endif

#define RUNTIME_WATCH_MAGIC 0xa1d70000u
#define RUNTIME_WATCH_USB 0x100u
#define RUNTIME_WATCH_FAULT 0x200u
#define RUNTIME_WATCH_CORE1 0x400u
#define RUNTIME_WATCH_STAGE_MASK 0xffff00ffu
enum { WATCH_BOOT, WATCH_LOOP, WATCH_BUTTON, WATCH_LOCK, WATCH_HCI, WATCH_MIC,
       WATCH_PHONE, WATCH_CONTROL, WATCH_AACP, WATCH_RPC, WATCH_FEATURES,
       WATCH_PCM, WATCH_ENCODER, WATCH_PACKET, WATCH_UNLOCK, WATCH_IDLE,
       WATCH_BOOTSEL_ENTER, WATCH_BOOTSEL_READ, WATCH_BOOTSEL_EXIT,
       WATCH_BUTTON_ACTIONS, WATCH_BUTTON_ACTION_LOCK, WATCH_BUTTON_ACTION,
       WATCH_BUTTON_ACTION_UNLOCK };
static inline void runtime_watch_mark(volatile uint32_t *r,unsigned stage,uint32_t us) {
    if((r[0]&0xffff0000u)==RUNTIME_WATCH_MAGIC && (r[0]&RUNTIME_WATCH_FAULT))return;
    r[1]=us;
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
    // Core1 may set a fault while core0 marks its stage; preserve context bits.
    hw_write_masked(&r[0],RUNTIME_WATCH_MAGIC|(stage&255u),RUNTIME_WATCH_STAGE_MASK);
#else
    r[0]=(r[0]&~RUNTIME_WATCH_STAGE_MASK)|RUNTIME_WATCH_MAGIC|(stage&255u);
#endif
}
static inline void runtime_watch_fault(volatile uint32_t *r,uint32_t cfsr,uint32_t hfsr) {
    r[2]=cfsr;r[3]=hfsr;
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
    hw_set_bits(&r[0],RUNTIME_WATCH_FAULT);
#else
    r[0]|=RUNTIME_WATCH_FAULT;
#endif
}
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
static inline void runtime_watch_set(unsigned stage) {
    runtime_watch_mark(watchdog_hw->scratch,stage,time_us_32());
}
#else
static inline void runtime_watch_set(unsigned stage) {(void)stage;}
#endif
#endif
