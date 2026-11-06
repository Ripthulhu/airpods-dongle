#include "sco_outbound.h"
/* Actual qualified native functions; runtime prefix/overlay guards belong to the installer. */
typedef uint32_t (*map_fn)(uint32_t);
typedef void (*ready_fn)(void);
typedef uint32_t (*free_fn)(uint32_t,uint32_t);
typedef int (*write_fn)(uint32_t,const void *,uint32_t);
int sco_native_b2h_preflight(uint32_t length){
    volatile uint32_t *state=(volatile uint32_t *)(uintptr_t)0x2038ccu;
    if(length>258u || *(volatile uint8_t *)state!=1 || state[1]!=2 || state[3]!=0x6861cu || state[4]!=0x1000u || state[5]!=0x1000u)return 2;
    ((ready_fn)(uintptr_t)0x5f941u)();
    uint32_t raw=state[3]+state[4]+state[5];
    volatile uint32_t *meta=(volatile uint32_t *)(uintptr_t)((map_fn)(uintptr_t)0x7baedu)(raw);
    uint32_t writer=meta[2],reader=meta[3];
    if((writer|reader)&3u || writer>=state[5] || reader>=state[5])return 2;
    uint32_t free=((free_fn)(uintptr_t)0x5f925u)(reader,writer);
    /* .0076 signed5fcb4→d0348 requires payload+8 capacity, not payload+4 padding. */
    return free>=length+8u?0:1;
}
int sco_native_b2h_commit(const void *payload,uint32_t length){return ((write_fn)(uintptr_t)0x5fc55u)(3,payload,length);}
void sco_native_b2h_wake(void){__asm volatile("dmb" ::: "memory");((ready_fn)(uintptr_t)0x5f87bu)();}
