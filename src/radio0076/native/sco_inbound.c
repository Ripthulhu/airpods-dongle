#include "sco_inbound.h"
#include <stddef.h>

#define V8(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define V16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define V32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define ENTER() (((uint32_t (*)(void))(uintptr_t)0x45efu)())
#define LEAVE(key) (((void (*)(uint32_t))(uintptr_t)0x45f7u)(key))
#define LOOKUP(handle) (((uintptr_t (*)(uint32_t,uint32_t))(uintptr_t)0xf4a7u)(handle,2))
#define SUBMIT(endpoint,payload,length) (((void (*)(uintptr_t,const uint8_t *,uint32_t))(uintptr_t)0x390abu)(endpoint,payload,length))

volatile struct sco_inbound_counters sco_inbound_stats
    __attribute__((section(".sco_in_stats"), aligned(4)));
static uint8_t rx[258] __attribute__((section(".sco_rx_scratch"), aligned(4)));
static volatile uint32_t busy __attribute__((section(".sco_in_state"), aligned(4)));

static int ram_object(uintptr_t pointer, uint32_t bytes) {
    return !(pointer & 3u) && pointer >= 0x200000u &&
           pointer <= 0x220000u - bytes;
}

static int ring_span(const uint8_t *body, uint32_t length, uint32_t wrapped,
                     const uint8_t *base, const uint8_t *last) {
    uintptr_t begin=(uintptr_t)base, end=(uintptr_t)last;
    uintptr_t packet=(uintptr_t)body;
    return wrapped<=length && end>=begin && end-begin<0x10000u &&
           packet>=begin && packet<=end &&
           length-wrapped<=end-packet+1u && wrapped<=end-begin+1u &&
           (!wrapped || length-wrapped==end-packet+1u);
}

void sco_inbound_command(const uint8_t *body, uint32_t length,
                         uint32_t wrapped, const uint8_t *base,
                         const uint8_t *last) {
    if (length!=3 || !ring_span(body,length,wrapped,base,last)) return;
    uint8_t header[3];
    for (uint32_t i=0; i<3; ++i)
        header[i]=i<3-wrapped ? body[i] : base[i-(3-wrapped)];
    if (header[0]==3 && header[1]==12 && header[2]==0) {
        uint32_t key=ENTER();
        sco_bridge_reset();
        LEAVE(key);
    }
}

uint32_t sco_inbound_enable_cpu_mode(void) {
    uint32_t key = ENTER();
    /* ROM38ad4 does not reset the selector, and38da6 reads it at activation. */
    for (uint32_t i=0; i<3; ++i) {
        if (V8(0x2063e4u+40*i+0x24) || V8(0x20645cu+40*i+0x24)) {
            ++sco_inbound_stats.init_rejected;
            LEAVE(key);
            return 0;
        }
    }
    if (V8(0x202e7du)>1u) {
        ++sco_inbound_stats.init_rejected;
        LEAVE(key);
        return 0;
    }
    ((void (*)(uint32_t))(uintptr_t)0x38e8du)(1);
    uint32_t valid=V8(0x202e7du)==1 && V32(0x202e80u)==0x662e1u;
    if (valid) ++sco_inbound_stats.init_success;
    else ++sco_inbound_stats.init_rejected;
    LEAVE(key);
    return valid;
}

uint32_t sco_inbound_process(const uint8_t *body, uint32_t length,
                            uint32_t wrapped, const uint8_t *base,
                            const uint8_t *last) {
    uint32_t key=ENTER();
    if (busy) {
        ++sco_inbound_stats.busy_retries;
        LEAVE(key);
        return 0;
    }
    busy=1;
    ++sco_inbound_stats.frames;
    if (length<3 || length>sizeof rx || !ring_span(body,length,wrapped,base,last)) {
        ++sco_inbound_stats.malformed;
        goto consumed;
    }
    for (uint32_t i=0; i<length; ++i)
        rx[i]=i<length-wrapped ? body[i] : base[i-(length-wrapped)];
    uint32_t payload=rx[2];
    if (!payload || payload+3u!=length) {
        ++sco_inbound_stats.malformed;
        goto consumed;
    }
    uint32_t handle=(uint32_t)rx[0]|((uint32_t)rx[1]<<8);
    if (handle>=0xfffu) {
        ++sco_inbound_stats.invalid_handle;
        goto consumed;
    }
    uintptr_t connection=LOOKUP(handle);
    if (connection<0x2066ccu || connection>0x20696cu ||
        (connection-0x2066ccu)%0x150u) {
        ++sco_inbound_stats.invalid_handle;
        goto consumed;
    }
    uint32_t index=(V32(connection+0xe4u)>>18)&3u;
    if (index>=3) {
        ++sco_inbound_stats.invalid_handle;
        goto consumed;
    }
    uintptr_t endpoint=0x2063e4u+40u*index;
    uint32_t flags=V8(endpoint+0x1eu);
    uint32_t group=(flags>>2)&3u;
    if (group>=3 || V16(0x202e84u+2u*group)!=handle) {
        ++sco_inbound_stats.invalid_handle;
        goto consumed;
    }
    if (V8(endpoint+0x24u)!=1u) {
        /* Native390aa rejects this format without accepting or crediting it. */
        ++sco_inbound_stats.inactive;
        goto consumed;
    }
    uintptr_t pipe=V32(endpoint+0x14u);
    if (V8(0x202e7du)!=1u || V32(0x202e80u)!=0x662e1u) {
        ++sco_inbound_stats.cpu_mode_mismatch;
        goto consumed;
    }
    if (!pipe) {
        ++sco_inbound_stats.unavailable;
        SUBMIT(endpoint,rx+3,payload);
        goto consumed;
    }
    if (!ram_object(pipe,0x58u) || V32(pipe+0x18u)!=endpoint ||
        V32(endpoint)!=0x662e1u || !V16(pipe+0x4cu) ||
        V16(pipe+0x50u)<V16(pipe+0x4cu) ||
        V16(pipe+0x4eu)>V16(pipe+0x50u)) {
        ++sco_inbound_stats.unsafe_pipe;
        goto unavailable;
    }
    ++sco_inbound_stats.native_submissions;
    sco_inbound_stats.native_bytes+=payload;
    SUBMIT(endpoint,rx+3,payload);
    goto consumed;
unavailable: {
    /* Preserve native unavailable-pipe drop credits; never credit a success. */
    uint32_t absent[10];
    /* The guarded native NULL-pipe branch reads only these three fields. */
    absent[0x14/4]=0;
    absent[0x1c/4]=flags<<16;
    absent[0x24/4]=1;
    ++sco_inbound_stats.unavailable;
    SUBMIT((uintptr_t)absent,rx+3,payload);
}
consumed:
    busy=0;
    LEAVE(key);
    return 1;
}
