#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "cyw43.h"
#include "cyw43_ll.h"
#include "cybt_shared_bus_driver.h"
#include "radio0076.h"
#include "readback_gate.h"
#include "preboot_patch.h"
#include "readback.h"
#include "expected.h"

bool calibration_prepare(void);
bool shared_bridge_install(void);
bool shared_bridge_configure(void);
cybt_result_t __real_cybt_fw_download(const uint8_t *,uint32_t,uint8_t *,uint8_t *);
cybt_result_t __real_cybt_set_host_ready(void);
int __real_cyw43_ll_write_backplane_mem(cyw43_ll_t *,uint32_t,uint32_t,const uint8_t *);

static absolute_time_t deadline;
static bool armed,in_download,callback_good,host_ready,configured;
static unsigned downloads,callbacks;
static const char *failure;

static bool fail(const char *reason) { if(!failure) failure=reason; return false; }
const char *radio0076_error(void) { return failure?failure:"none"; }
bool rb_gate_time_available(void) { return armed && !time_reached(deadline); }
void radio0076_begin(void) {
    armed=true;in_download=callback_good=host_ready=configured=false;
    downloads=callbacks=0;failure=NULL;deadline=make_timeout_time_ms(6000);
}

static int read_owned(void *context,uint32_t address,uint32_t length,uint8_t *data) {
    if(!rb_read_allowed(rb_spans,RB_SPAN_COUNT,address,length)) return RB_INVALID;
    if(!rb_gate_time_available()) return RB_DEADLINE;
    return cyw43_ll_read_backplane_mem(context,address,length,data);
}

cybt_result_t __wrap_cybt_fw_download(const uint8_t *firmware,uint32_t length,uint8_t *write_buf,uint8_t *hex_buf) {
    if(!rb_gate_time_available() || ++downloads!=1 || !firmware || length!=RB_CONTAINER_LENGTH ||
       rb_crc32(firmware,length)!=RB_CONTAINER_CRC32) {
        fail("firmware-identity"); return CYBT_ERR_HCI_INIT_FAILED;
    }
    in_download=true;
    cybt_result_t result=__real_cybt_fw_download(firmware,length,write_buf,hex_buf);
    in_download=false;
    if(result || !callback_good) { fail("firmware-download");return CYBT_ERR_HCI_INIT_FAILED; }
    return result;
}

int __wrap_cyw43_ll_write_backplane_mem(cyw43_ll_t *self,uint32_t address,uint32_t length,const uint8_t *data) {
    if(in_download && address==0x19204164u) {
        static const uint8_t expected[4]={0xed,0xf7,0x05,0x00};
        if(++callbacks!=1 || length!=4 || !data || memcmp(data,expected,4)) {
            fail("loader-callback-identity");return -1;
        }
        for(unsigned i=0;i<RB_SPAN_COUNT;++i) {
            rb_result_t result;
            if(rb_compare(&rb_spans[i],&result,read_owned,self) || result.mismatches ||
               result.compared!=rb_spans[i].length || result.observed_crc32!=rb_spans[i].crc32) {
                fail("firmware-readback");return -1;
            }
        }
        if(!preboot_patch_install(self)) { fail("early-configuration");return -1; }
        callback_good=true;
    }
    int result=__real_cyw43_ll_write_backplane_mem(self,address,length,data);
    if(in_download && result) fail("firmware-write");
    return result;
}

cybt_result_t __wrap_cybt_set_host_ready(void) {
    uint8_t observed[12] __attribute__((aligned(4)))={0};
    static const uint8_t expected[12]={0x03,0x49,0x09,0x78,0x89,0x07,0x00,0xd5,0xc6,0xe7,0x70,0x47};
    uint32_t host=0,mode=0;
    /* SDK already holds the CYW43 lock; never reenter it at this boundary. */
    if(!rb_gate_time_available() || failure || !callback_good || host_ready ||
       cyw43_ll_read_backplane_mem(&cyw43_state.cyw43_ll,0x1907baecu,12,observed) || memcmp(observed,expected,12) ||
       cyw43_ll_read_backplane_mem(&cyw43_state.cyw43_ll,0x18000d6cu,4,(uint8_t *)&host) || (host&(1u<<24)) ||
       cyw43_ll_read_backplane_mem(&cyw43_state.cyw43_ll,0x192038d0u,4,(uint8_t *)&mode) || mode!=2) {
        fail("host-ready-identity");return CYBT_ERR_HCI_INIT_FAILED;
    }
    if(!preboot_patch_capture()) { fail("early-configuration-readback");return CYBT_ERR_HCI_INIT_FAILED; }
    if(!calibration_prepare()) { fail("calibration");return CYBT_ERR_HCI_INIT_FAILED; }
    if(!shared_bridge_install()) { fail("shared-sco-install");return CYBT_ERR_HCI_INIT_FAILED; }
    cybt_result_t result=__real_cybt_set_host_ready();
    if(result) fail("host-ready");
    host_ready=!result;
    return result;
}

bool radio0076_start(void) {
    int result=cyw43_bluetooth_hci_init();
    if(result || failure || !host_ready || !callback_good || !preboot_patch_good()) return fail("bluetooth-init");
    return true;
}

bool radio0076_after_hci(void) {
    if(configured) return !failure;
    if(failure || !host_ready || !shared_bridge_configure()) return fail("shared-sco-configuration");
    configured=true;
    return true;
}
