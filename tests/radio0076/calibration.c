#include <assert.h>
#include <stdio.h>
#include "../../src/radio0076/calibration.c"
probe_state_t cyw43_state;
static uint32_t memory[CAL_WORD_COUNT];
static int fault;
static unsigned writes;
bool rb_gate_time_available(void) {return fault!=6;}
int cyw43_ll_read_backplane_mem(void *p,uint32_t a,unsigned n,uint8_t *out) {
    (void)p;
    if(a==0x1905e0f4u) {
        const uint8_t bytes[16]={0xf0,0x8f,0xf8,0xb5,0x15,0x46,0x0e,0x46,0xb3,0xf7,0x4f,0xfa,0xc0,0xb2,0xa3,0xf7};
        assert(n==16);memcpy(out,bytes,16);if(fault==1)out[2]^=1;return 0;
    }
    assert(n==4);uint32_t value=0;bool found=false;
    if(a==0x19200680u) {value=fault==3?0:0x010100;found=true;}
    if(a==0x19200684u) {value=1;found=true;}
    for(unsigned i=0;i<CAL_WORD_COUNT;++i)if(a==cal_words[i].address){value=memory[i];found=true;}
    assert(found);if(fault==2)return -1;memcpy(out,&value,4);return 0;
}
int cyw43_ll_write_backplane_mem(void *p,uint32_t a,unsigned n,const uint8_t *in) {
    (void)p;assert(n==4);++writes;if(fault==5)return -1;
    for(unsigned i=0;i<CAL_WORD_COUNT;++i)if(a==cal_words[i].address) {
        uint32_t value;memcpy(&value,in,4);assert(!((value^memory[i])&~cal_words[i].mask));memory[i]=value;return 0;
    }
    assert(0);return -1;
}
static void reset(unsigned profile,int f) {
    fault=f;writes=0;
    for(unsigned i=0;i<CAL_WORD_COUNT;++i)memory[i]=(cal_words[i].expected[profile]&cal_words[i].mask)|(0xa5a5a5a5&~cal_words[i].mask);
}
int main(void) {
    reset(0,0);assert(calibration_prepare() && writes==0);
    reset(2,0);assert(calibration_prepare() && writes>0);
    for(unsigned i=0;i<CAL_WORD_COUNT;++i)assert(!((memory[i]^cal_words[i].expected[0])&cal_words[i].mask));
    for(int f=1;f<=3;++f){reset(2,f);assert(!calibration_prepare() && writes==0);}
    reset(2,0);memory[0]^=cal_structure_masks[0];assert(!calibration_prepare() && writes==0);
    reset(2,5);assert(!calibration_prepare() && writes==1);
    reset(2,6);assert(!calibration_prepare() && writes==0);
    puts(".0076 calibration shape/signature/ROM/deadline guards and masked writes passed");
}
