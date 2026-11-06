#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/radio0076/preboot_patch.c"
probe_state_t cyw43_state;
static uint32_t mock_bank,mock_target,mock_value,mock_marker,mock_bank0,mock_boot_marker,host_target,host_value,mock_register_marker;
static int fault;static unsigned io_writes;
bool rb_gate_time_available(void) {return fault!=5;}
int cyw43_ll_read_backplane_mem(void *p,uint32_t a,unsigned n,uint8_t *out) {
 (void)p;assert(n==4);uint32_t v;
 switch(a) {
 case 0x1905ac1c:v=fault==11?0:0x4dc7b570;break;
 case 0x1905ac44:v=fault==11?0:0xd0f32800;break;
 case 0x1905ac64:v=fault==11?0:0xf813f7f9;break;

 case 0x190d33c8:v=fault==10?672:144;break;
 case 0x190d33c0:v=0x219000;break;
 case 0x190d33c4:v=0x100;break;
 case 0x19005394:v=fault==8?0:0x43fee92d;break;
 case 0x190d33fc:v=mock_register_marker;break;
 case 0x19004908:v=fault==7?0:0x486e2100;break;
 case 0x190d31fc:v=mock_boot_marker;break;
 case 0x1905de48:v=fault==1?0:0x462a2302;break;
 case 0x19310208:v=mock_bank;break;
 case 0x193100fc:v=mock_target;break;
 case 0x190d00fc:v=mock_value;break;
 case 0x193100f8:v=host_target;break;
 case 0x190d00f8:v=fault==4?0:host_value;break;
 case 0x19201f14:v=39;break;
 case 0x190d30fc:v=mock_marker;break;
 case 0x19310204:v=mock_bank0;break;
 default:if(a>=0x190d3380 && a<0x190d33a0) {v=fault==9?255:rf_expected[(a-0x190d3380)/4];break;}assert(!"Unexpected read");return -1;
 } memcpy(out,&v,4);return 0;
}
int __real_cyw43_ll_write_backplane_mem(cyw43_ll_t *p,uint32_t a,uint32_t n,const uint8_t *in) {
 (void)p;assert(n==4);uint32_t v;memcpy(&v,in,4);++io_writes;
 if(fault==3 && io_writes==2)return -1;
 switch(a) {
 case 0x193100f8:assert(io_writes==1 && v==0x1242);host_target=v;break;
 case 0x190d00f8:assert(io_writes==2 && v==0xbbfaf0ce);host_value=v;break;
 case 0x19310208:assert(io_writes==3 && host_target==0x1242 && host_value==0xbbfaf0ce && v==0x40000000u);mock_bank=v;break;
 default:assert(!"Unexpected write");return -1;
 } return 0;
}
static void reset(int f) {fault=f;mock_bank=f==2?1:0;mock_target=mock_value=host_target=host_value=mock_boot_marker=mock_register_marker=0;mock_marker=f==6?1:0;mock_bank0=0xfffffffe;io_writes=writes=0;installed=checked=-1;}
int main(void) {
 reset(0);preboot_patch_install(&cyw43_state.cyw43_ll);assert(installed==0 && writes==3);
 mock_bank=0x1c00007f;mock_marker=3;mock_boot_marker=1;mock_register_marker=1;mock_target=0x17792;mock_value=0xb8daf075;assert(preboot_patch_capture() && preboot_patch_good());
 fault=10;assert(!preboot_patch_capture());fault=0;
 mock_register_marker=0;assert(!preboot_patch_capture());mock_register_marker=1;fault=9;assert(!preboot_patch_capture());fault=0;
 mock_boot_marker=0;assert(!preboot_patch_capture());mock_boot_marker=1;
 mock_marker=0;assert(!preboot_patch_capture());mock_marker=0x80000003;assert(!preboot_patch_capture());mock_marker=3;
 mock_bank|=0x80000000;assert(!preboot_patch_capture());mock_bank=0x1c00007f;mock_bank0=0;assert(!preboot_patch_capture());
 for(int f=1;f<=8;++f) {reset(f);preboot_patch_install(&cyw43_state.cyw43_ll);assert(installed<0 && !(mock_bank&0x40000000u));if(f==1 || f==2 || f==5 || f==6 || f==7 || f==8)assert(io_writes==0);}
 reset(11);preboot_patch_install(&cyw43_state.cyw43_ll);assert(installed<0 && io_writes==0);
 puts("Early TCA guards, publication order, marker and retirement checks passed");
}
