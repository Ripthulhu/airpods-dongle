// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PLAYBACK_TRACE_H
#define AIRPODS_PLAYBACK_TRACE_H
#include <stdint.h>
#include <string.h>

enum {
    TRACE_USB_PACKETS, TRACE_USB_FRAMES, TRACE_USB_GAP_US, TRACE_USB_BAD,
    TRACE_USB_SHORT, TRACE_STARTUP_DROP, TRACE_ACTIVE_DROP, TRACE_STARTUP_ZERO,
    TRACE_ACTIVE_ZERO, TRACE_TIMER_LATE_US, TRACE_ENCODER_US, TRACE_SEND_WAIT_US,
    TRACE_SEND_GAP_US, TRACE_PACKET_AGE_US, TRACE_FLUSHES, TRACE_COMPLETED,
    TRACE_DEBT_DROP, TRACE_RECOVERIES, TRACE_PCM_MIN, TRACE_PCM_MAX,
    TRACE_CLOCK_Q16, TRACE_BLE_SCANNING, TRACE_USB_FORMAT, TRACE_UNIT_US,
    TRACE_STAT_COUNT
};
enum { TRACE_DROP=1, TRACE_ZERO, TRACE_LATE, TRACE_SEND_WAIT, TRACE_RECOVERY,
       TRACE_FLUSH, TRACE_USB_ERROR, TRACE_FORMAT, TRACE_GAME };
#define PLAYBACK_TRACE_EVENTS 24u
#define PLAYBACK_TRACE_SIZE 512u
typedef struct { uint32_t us, kind, a, b; } playback_trace_event_t;
typedef struct {
    uint32_t stats[TRACE_STAT_COUNT], serial;
    uint8_t head,count;
    playback_trace_event_t events[PLAYBACK_TRACE_EVENTS];
} playback_trace_t;

// Core 0 and its USB IRQ serialize access; callers exclude the IRQ briefly.
static inline void playback_trace_record(playback_trace_t *s, uint32_t us,
                                          uint32_t kind, uint32_t a, uint32_t b) {
    s->events[s->head] = (playback_trace_event_t){us,kind,a,b};
    s->head=(uint8_t)((s->head+1)%PLAYBACK_TRACE_EVENTS);
    if(s->count<PLAYBACK_TRACE_EVENTS)++s->count;
    ++s->serial;
}
static inline void playback_trace_put32(uint8_t *p, uint32_t v) {
    for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i));
}
static inline uint16_t playback_trace_pack(const playback_trace_t *s, uint8_t *out,
    uint16_t capacity, uint32_t us, uint32_t used, uint32_t debt, uint32_t game, uint32_t rtp) {
    if (!out || capacity<PLAYBACK_TRACE_SIZE) return 0;
    memset(out,0,PLAYBACK_TRACE_SIZE);
    out[0]=1; out[1]=16;
    unsigned count=s->count;
    out[2]=(uint8_t)count;
    const uint32_t header[]={us,s->serial,used,debt,game,rtp,0};
    for(unsigned i=0;i<7;++i)playback_trace_put32(out+4+4*i,header[i]);
    for(unsigned i=0;i<TRACE_STAT_COUNT;++i)playback_trace_put32(out+32+4*i,s->stats[i]);
    for(unsigned i=0;i<count;++i) {
        const playback_trace_event_t *e=&s->events[(s->head+PLAYBACK_TRACE_EVENTS-count+i)%PLAYBACK_TRACE_EVENTS];
        const uint32_t values[]={e->us,e->kind,e->a,e->b};
        for(unsigned j=0;j<4;++j)playback_trace_put32(out+128+16*i+4*j,values[j]);
    }
    return PLAYBACK_TRACE_SIZE;
}
#endif
