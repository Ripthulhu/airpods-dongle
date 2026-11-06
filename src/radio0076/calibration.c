#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "cyw43.h"
#include "cyw43_ll.h"
#include "readback_gate.h"
#include "calibration_words.h"

static int read_twice(uint32_t address, uint32_t mask, uint32_t *value) {
    uint32_t a=0,b=0;
    if(!rb_gate_time_available()) return -1;
    int r=cyw43_ll_read_backplane_mem(&cyw43_state.cyw43_ll,address,4,(uint8_t *)&a);
    if(!r) r=cyw43_ll_read_backplane_mem(&cyw43_state.cyw43_ll,address,4,(uint8_t *)&b);
    if(!r && ((a^b)&mask)) r=-2;
    *value=b;
    return r;
}

bool calibration_prepare(void) {
    uint8_t instructions[16] __attribute__((aligned(4)))={0};
    static const uint8_t expected[12]={0xf8,0xb5,0x15,0x46,0x0e,0x46,0xb3,0xf7,0x4f,0xfa,0xc0,0xb2};
    if(cyw43_ll_read_backplane_mem(&cyw43_state.cyw43_ll,0x1905e0f4u,16,instructions) ||
       memcmp(instructions+2,expected,12)) return false;
    uint32_t observed[CAL_WORD_COUNT],flags=0,secure=0;
    bool match[2]={true,true};
    for(unsigned i=0;i<CAL_WORD_COUNT;++i) {
        const calibration_word_t *w=&cal_words[i];
        if(read_twice(w->address,w->mask,&observed[i])) return false;
        for(unsigned j=0;j<2;++j)
            if((observed[i]^w->expected[j])&cal_structure_masks[i]) match[j]=false;
    }
    if(read_twice(0x19200680u,0xffffffu,&flags) || read_twice(0x19200684u,0xffu,&secure) ||
       ((flags>>8)&255)!=1 || ((flags>>16)&255)!=1 || (secure&255)!=1 ||
       (!match[0] && !match[1])) return false;
    unsigned selector=match[0]?0:1;
    for(unsigned i=0;i<CAL_WORD_COUNT;++i) {
        const calibration_word_t *w=&cal_words[i];
        uint32_t current=0;
        if(read_twice(w->address,w->mask,&current)) return false;
        if((current^w->expected[selector])&w->mask) {
            uint32_t value=(current&~w->mask)|(w->expected[selector]&w->mask);
            if(cyw43_ll_write_backplane_mem(&cyw43_state.cyw43_ll,w->address,4,(uint8_t *)&value)) return false;
        }
    }
    for(unsigned i=0;i<CAL_WORD_COUNT;++i) {
        uint32_t value=0;
        if(read_twice(cal_words[i].address,cal_words[i].mask,&value) ||
           ((value^cal_words[i].expected[selector])&cal_words[i].mask)) return false;
    }
    return true;
}
