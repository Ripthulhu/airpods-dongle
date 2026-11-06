#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "cyw43.h"
#include "cyw43_ll.h"
#include "pico/cyw43_arch.h"
#include "shared_bridge_blob.h"
#include "shared_bridge_guards.h"
static int installed=-1,configured=-1;
static uint32_t failure_address,failure_observed;
static int read_mem(uint32_t a,unsigned n,void *v){
    uint8_t *p=v;
    for(unsigned i=0;i<n;i+=32){unsigned k=n-i<32?n-i:32;int r=cyw43_ll_read_backplane_mem(&cyw43_state.cyw43_ll,a+i,k,p+i);if(r)return r;}
    return 0;
}
static bool equal(uint32_t a,unsigned n,const uint8_t *expected){
    uint8_t b[32];
    for(unsigned i=0;i<n;i+=32){unsigned k=n-i<32?n-i:32;
        if(read_mem(a+i,k,b))return false;
        for(unsigned j=0;j<k;++j)if(b[j]!=(expected?expected[i+j]:0)){
            failure_address=a+i+j;failure_observed=b[j];return false;
        }
    }
    return true;
}
static bool word(uint32_t a,uint32_t v){return equal(a,4,(uint8_t *)&v);}
static bool write(uint32_t a,unsigned n,const void *v){
    const uint8_t *p=v;
    for(unsigned i=0;i<n;i+=32){unsigned k=n-i<32?n-i:32;if(cyw43_ll_write_backplane_mem(&cyw43_state.cyw43_ll,a+i,k,p+i))return false;}
    return equal(a,n,p);
}
static bool put(uint32_t a,uint32_t v){return write(a,4,&v);}
bool shared_bridge_install(void){
    /* Slots 39/40 were diagnostic hooks; reserve them without enabling them. */
    installed=-2;
    if(!word(0x19310208u,0x1c00007fu) || !word(0x19201f14u,39) ||
       !word(0x192038d0u,2) || !equal(0x190d3900u,sizeof shared_bridge_blob,0))return false;
    for(unsigned i=0;i<sizeof shared_native_guards/sizeof *shared_native_guards;++i){
        const struct shared_guard *g=&shared_native_guards[i];
        if(!equal(0x19000000u+g->address,8,g->bytes))return false;
    }
    for(unsigned i=0;i<sizeof shared_hooks/sizeof *shared_hooks;++i){const struct shared_hook *h=&shared_hooks[i];
        if((h->address&3) || !word(0x19000000u+h->address,h->expected))return false;
    }
    installed=-3;
    if(!write(0x190d3900u,sizeof shared_bridge_blob,shared_bridge_blob))return false;
    for(unsigned i=0;i<sizeof shared_hooks/sizeof *shared_hooks;++i){const struct shared_hook *h=&shared_hooks[i];
        if(!put(0x19310000u+4*h->slot,h->address/4) || !put(0x190d0000u+4*h->slot,h->patch))return false;
    }
    /* Enable only after every byte and branch target has read back correctly. */
    if(!put(0x19201f14u,45) || !put(0x19310208u,0x1c001e7fu))return false;
    installed=0;return true;
}
bool shared_bridge_configure(void){
    uint32_t g[4]={0},state=0;
    configured=-2;
    CYW43_THREAD_ENTER;
    bool ok=installed==0 && word(0x19310208u,0x1c001e7fu) && word(0x19201f14u,45) && !read_mem(0x19202e7cu,sizeof g,g) && word(0x192038d0u,2);
    for(unsigned i=0;i<3 && ok;++i){
        ok=((g[2+i/2]>>(16*(i%2)))&0xffffu)==0xfff;
        for(unsigned d=0;d<2 && ok;++d){
            ok=!read_mem((d?0x1920645cu:0x192063e4u)+40*i+0x24,4,&state) && !(state&255);
        }
    }
    for(unsigned i=0;i<sizeof shared_hooks/sizeof *shared_hooks && ok;++i){
        const struct shared_hook *h=&shared_hooks[i];
        ok=word(0x19310000u+4*h->slot,h->address/4) && word(0x190d0000u+4*h->slot,h->patch);
    }
    uint32_t mode=(g[0]>>8)&255;
    ok=ok && ((mode==0 && g[1]==0x66295) || (mode==1 && g[1]==0x662e1));
    if(ok)ok=put(0x19202e80u,0x662e1) && put(0x19202e7cu,(g[0]&~0xff00u)|0x100u);
    if(ok)ok=word(0x19202e80u,0x662e1) && word(0x19202e7cu,(g[0]&~0xff00u)|0x100u);
    CYW43_THREAD_EXIT;
    configured=ok?0:-3;return ok;
}
