#include <assert.h>
#include <stdio.h>
#include "../../src/radio0076/shared_bridge.c"
probe_state_t cyw43_state;
static uint8_t ram[0x6000],patch[0x220];
static unsigned writes;
static int fault;
static void store(uint32_t a,uint32_t v) {
    if(a>=0x190d0000 && a+4<=0x190d6000)memcpy(ram+a-0x190d0000,&v,4);
    else {assert(a>=0x19310000 && a+4<=0x19310220);memcpy(patch+a-0x19310000,&v,4);}
}
static uint32_t next,mode[4];
int cyw43_ll_read_backplane_mem(void *p,uint32_t a,unsigned n,uint8_t *out) {
    (void)p;assert(n<=32);memset(out,0,n);
    if(a>=0x190d0000 && a+n<=0x190d6000){memcpy(out,ram+a-0x190d0000,n);return 0;}
    if(a>=0x19310000 && a+n<=0x19310220){memcpy(out,patch+a-0x19310000,n);return 0;}
    uint32_t v=0;
    if(a==0x19201f14)v=next;
    else if(a==0x192038d0)v=fault==1?0:2;
    else if(a==0x19202e7c){assert(n==16 || n==4);memcpy(out,mode,n);return 0;}
    else if(a==0x19202e80)v=mode[1];
    else if(a>=0x192063e4 && a<0x19206500)v=fault==4?1:0;
    else {
        for(unsigned i=0;i<sizeof shared_native_guards/sizeof *shared_native_guards;++i)
            if(a==0x19000000+shared_native_guards[i].address && n==8){memcpy(out,shared_native_guards[i].bytes,8);return 0;}
        bool found=false;
        for(unsigned i=0;i<sizeof shared_hooks/sizeof *shared_hooks;++i)
            if(a==0x19000000+shared_hooks[i].address){v=shared_hooks[i].expected;found=true;}
        assert(found);
    }
    assert(n==4);memcpy(out,&v,4);return 0;
}
int cyw43_ll_write_backplane_mem(void *p,uint32_t a,unsigned n,const uint8_t *in) {
    (void)p;++writes;if(fault==2 && writes==2)return -1;
    if(a>=0x190d3900 && a+n<=0x190d3900+sizeof shared_bridge_blob){memcpy(ram+a-0x190d0000,in,n);return 0;}
    assert(n==4);uint32_t v;memcpy(&v,in,4);
    if(a==0x19201f14){assert(v==45);next=v;return 0;}
    if(a==0x19202e80){mode[1]=v;return 0;}
    if(a==0x19202e7c){mode[0]=v;return 0;}
    if(a==0x19310208){assert(next==45 && v==0x1c001e7f);assert(!memcmp(ram+0x3900,shared_bridge_blob,sizeof shared_bridge_blob));}
    else {
        bool found=false;
        for(unsigned i=0;i<sizeof shared_hooks/sizeof *shared_hooks;++i){const struct shared_hook *h=&shared_hooks[i];
            if(a==0x19310000+4*h->slot){assert(v==h->address/4);found=true;}
            if(a==0x190d0000+4*h->slot){assert(v==h->patch);found=true;}
        }
        assert(found);
    }
    store(a,v);return 0;
}
static void reset(int f) {
    memset(ram,0,sizeof ram);memset(patch,0,sizeof patch);store(0x19310208,0x1c00007f);
    installed=configured=-1;writes=0;fault=f;next=39;
    mode[0]=0;mode[1]=0x66295;mode[2]=0x0fff0fff;mode[3]=0x0fff;
}
int main(void) {
    reset(0);assert(shared_bridge_install());assert(next==45);assert(shared_bridge_configure());
    assert(mode[0]==0x100 && mode[1]==0x662e1);
    uint32_t bank;memcpy(&bank,patch+0x208,4);assert(bank==0x1c001e7f && !(bank&0x180));
    reset(1);assert(!shared_bridge_install() && writes==0);
    reset(2);assert(!shared_bridge_install());memcpy(&bank,patch+0x208,4);assert(bank==0x1c00007f && next==39);
    reset(0);assert(shared_bridge_install());fault=4;unsigned before=writes;assert(!shared_bridge_configure() && writes==before);
    puts(".0076 native guards, publication-last, disabled diagnostic slots and idle-only configuration passed");
}
