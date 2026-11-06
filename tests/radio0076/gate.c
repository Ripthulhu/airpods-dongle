#include <assert.h>
#include <stdio.h>
#include "../../src/radio0076/radio0076.c"
#include "../../src/radio0076/cyw43_btfw_43439.h"

probe_state_t cyw43_state;
uint64_t mock_now;
static int fault,step,forwarded,writes;
static unsigned span_reads;

bool preboot_patch_install(cyw43_ll_t *ll) {
    assert(ll==&cyw43_state.cyw43_ll && step==0 && span_reads>100);
    step=1;return fault!=4;
}
bool preboot_patch_capture(void) {assert(step==1);step=2;return fault!=5;}
bool preboot_patch_good(void) {return step>=2;}
bool calibration_prepare(void) {assert(step==2);step=3;return fault!=6;}
bool shared_bridge_install(void) {assert(step==3);step=4;return fault!=7;}
bool shared_bridge_configure(void) {assert(step==5);step=6;return fault!=11;}
cybt_result_t __real_cybt_set_host_ready(void) {assert(step==4);step=5;++forwarded;return fault==8?1:0;}
int __real_cyw43_ll_write_backplane_mem(cyw43_ll_t *ll,uint32_t address,uint32_t n,const uint8_t *p) {
    (void)ll;(void)address;(void)n;(void)p;++writes;return fault==9?-9:0;
}
int cyw43_ll_read_backplane_mem(void *ll,uint32_t address,unsigned n,uint8_t *out) {
    assert(ll==&cyw43_state.cyw43_ll);
    if(address==0x1907baec) {
        const uint8_t bytes[]={0x03,0x49,0x09,0x78,0x89,0x07,0x00,0xd5,0xc6,0xe7,0x70,0x47};
        assert(n==sizeof bytes);memcpy(out,bytes,n);return 0;
    }
    if(address==0x18000d6c || address==0x192038d0) {
        uint32_t value=address==0x192038d0?2:0;
        assert(n==4);if(fault==10)value^=1;memcpy(out,&value,4);return 0;
    }
    assert(rb_read_allowed(rb_spans,RB_SPAN_COUNT,address,n));
    for(unsigned i=0;i<RB_SPAN_COUNT;++i) {
        const rb_span_t *s=&rb_spans[i];
        if(address>=s->address && address+n<=s->address+((s->length+3)&~3u)) {
            memset(out,0,n);
            unsigned offset=address-s->address,defined=s->length-offset;
            if(defined>n)defined=n;
            memcpy(out,s->expected+offset,defined);++span_reads;
            if(fault==3)out[0]^=1;
            return 0;
        }
    }
    assert(0);return -1;
}
cybt_result_t __real_cybt_fw_download(const uint8_t *firmware,uint32_t n,uint8_t *a,uint8_t *b) {
    (void)a;(void)b;assert(firmware==cyw43_btfw_43439 && n==sizeof cyw43_btfw_43439);
    uint8_t callback[]={0xed,0xf7,0x05,0x00};if(fault==2)callback[0]^=1;
    return __wrap_cyw43_ll_write_backplane_mem(&cyw43_state.cyw43_ll,0x19204164,4,callback)?1:0;
}
int cyw43_bluetooth_hci_init(void) {
    cybt_result_t result=__wrap_cybt_fw_download(cyw43_btfw_43439,sizeof cyw43_btfw_43439,NULL,NULL);
    return result?result:__wrap_cybt_set_host_ready();
}
static void reset(int f) {mock_now=0;fault=f;step=forwarded=writes=0;span_reads=0;radio0076_begin();}
int main(void) {
    reset(0);assert(radio0076_start() && step==5 && forwarded==1 && writes==1);
    assert(radio0076_after_hci() && step==6);assert(radio0076_after_hci() && step==6);
    assert(!strcmp(radio0076_error(),"none"));
    for(int f=2;f<=10;++f) {
        reset(f);assert(!radio0076_start() && strcmp(radio0076_error(),"none"));
        assert(!radio0076_after_hci());if(f!=8)assert(forwarded==0);
    }
    reset(11);assert(radio0076_start());assert(!radio0076_after_hci());
    reset(0);mock_now=6000000;assert(!radio0076_start() && step==0 && forwarded==0 && writes==0);
    reset(0);assert(__wrap_cybt_fw_download(NULL,0,NULL,NULL)==CYBT_ERR_HCI_INIT_FAILED && !writes);
    puts(".0076 identity/readback/bootstrap/calibration/ready ordering and fail-closed cases passed");
}
