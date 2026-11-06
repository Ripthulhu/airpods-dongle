#include <stdbool.h>
#include <stdint.h>
#include "cyw43.h"
#include "cyw43_ll.h"
#include "readback_gate.h"
#include "preboot_patch.h"
int __real_cyw43_ll_write_backplane_mem(cyw43_ll_t *,uint32_t,uint32_t,const uint8_t *);
static int installed=-1,checked=-1;
static unsigned writes;
static uint32_t before,after,target,value,next,marker,bank0,boot_marker,register_marker,rf_values[8],entry_base,entry_flags,requested_length;
static const uint32_t rf_expected[]={0,6,12,17,22,26,31,35};
static bool read_word(cyw43_ll_t *ll,uint32_t address,uint32_t *out) {
    uint32_t a=0,b=0;
    if(!rb_gate_time_available()) return false;
    if(cyw43_ll_read_backplane_mem(ll,address,4,(uint8_t *)&a) || cyw43_ll_read_backplane_mem(ll,address,4,(uint8_t *)&b) || a!=b) return false;
    *out=b;return true;
}
static bool write_word(cyw43_ll_t *ll,uint32_t address,uint32_t value) {
    uint32_t actual=0;
    ++writes;
    return !__real_cyw43_ll_write_backplane_mem(ll,address,4,(const uint8_t *)&value) && read_word(ll,address,&actual) && actual==value;
}
bool preboot_patch_install(cyw43_ll_t *ll) {
#ifdef EXTERNAL_PI_TRANSPORT
    uint32_t rom=0;
    installed=-2;
    if(!read_word(ll,0x1905de48u,&rom) || rom!=0x462a2302u || !read_word(ll,0x19310208u,&before) || before!=0 || !read_word(ll,0x190d30fcu,&marker) || marker!=0 || !read_word(ll,0x190d31fcu,&boot_marker) || boot_marker!=0 || !read_word(ll,0x19004908u,&rom) || rom!=0x486e2100u || !read_word(ll,0x19005394u,&rom) || rom!=0x43fee92du || !read_word(ll,0x190d33fcu,&register_marker) || register_marker!=0) return false;
    if(!read_word(ll,0x1905ac1cu,&rom) || rom!=0x4dc7b570u)return false;
    if(!read_word(ll,0x1905ac44u,&rom) || rom!=0xd0f32800u)return false;
    if(!read_word(ll,0x1905ac64u,&rom) || rom!=0xf813f7f9u)return false;
    // Slot 62 bootstraps after the ROM resets patches; TCA slot 63 retires itself.
    if(!write_word(ll,0x193100f8u,0x1242u) || !write_word(ll,0x190d00f8u,0xbbfaf0ceu) || !write_word(ll,0x19310208u,0x40000000u)) {installed=-3;return false;}
    installed=0;return true;
#else
    (void)ll;installed=0;return true;
#endif
}
bool preboot_patch_capture(void) {
#ifdef EXTERNAL_PI_TRANSPORT
    cyw43_ll_t *ll=&cyw43_state.cyw43_ll;
    checked=-1;
    if(installed || !read_word(ll,0x19310208u,&after) || !read_word(ll,0x193100fcu,&target) || !read_word(ll,0x190d00fcu,&value) || !read_word(ll,0x19201f14u,&next) || !read_word(ll,0x190d30fcu,&marker) || !read_word(ll,0x19310204u,&bank0) || !read_word(ll,0x190d31fcu,&boot_marker) || !read_word(ll,0x190d33fcu,&register_marker)) return false;
    for(unsigned i=0;i<8;++i) if(!read_word(ll,0x190d3380u+i*4u,&rf_values[i]) || (rf_values[i]&255u)!=rf_expected[i]) return false;
    if(!read_word(ll,0x190d33c0u,&entry_base) || !read_word(ll,0x190d33c4u,&entry_flags) || !read_word(ll,0x190d33c8u,&requested_length)) return false;
    checked=after==0x1c00007fu && bank0==0xfffffffeu && next==39 && target==0x17792u && value==0xb8daf075u && marker==3 && boot_marker==1 && register_marker==1 && entry_base==0x219000u && ((entry_flags>>8)&255u)==1 && requested_length==144 ? 0:-2;
    return checked==0;
#else
    checked=0;return true;
#endif
}
bool preboot_patch_good(void) {return installed==0 && checked==0;}
