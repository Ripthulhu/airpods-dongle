// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/aacp_routing.h"

static const uint8_t local[6]={0x10,0x20,0x30,0x40,0x50,0x60};
static const uint8_t other[6]={0x21,0x32,0x43,0x54,0x65,0x76};
static uint64_t fixture_hash(const uint8_t *p,size_t n) {
    uint64_t hash=UINT64_C(14695981039346656037);
    for(size_t i=0;i<n;++i) hash=(hash^p[i])*UINT64_C(1099511628211);
    return hash;
}
static void builders(void) {
    // Whole-packet golden receipts from independent ByteBuffer transcription of AACPManager.kt.
    const struct {aacp_route_kind_t kind;size_t size;uint64_t hash;} golden[]={
        {AACP_ROUTE_NEW_INFO,122,UINT64_C(0x16e37a024ba36948)},
        {AACP_ROUTE_TIPI,96,UINT64_C(0x175c28a58cd93abe)},
        {AACP_ROUTE_MEDIA,144,UINT64_C(0x7813d1c31c5338f8)},
        {AACP_ROUTE_HIJACK,112,UINT64_C(0xeebe80fea83941f6)},
        {AACP_ROUTE_REVERSE,103,UINT64_C(0x1f9bafe1c6348238)}
    };
    uint8_t out[AACP_ROUTE_MAX_PACKET+2],before[sizeof(out)];
    for(size_t i=0;i<sizeof(golden)/sizeof(golden[0]);++i) {
        memset(out,0xa5,sizeof(out));
        size_t n=aacp_route_build(out+1,AACP_ROUTE_MAX_PACKET,golden[i].kind,local,other);
        assert(n==golden[i].size && fixture_hash(out+1,n)==golden[i].hash);
        assert(out[0]==0xa5 && out[n+1]==0xa5);
        assert(aacp_route_header(out+1,n,0x10));
        assert((unsigned)(out[13]|(unsigned)out[14]<<8)==n-14);
        for(unsigned b=0;b<6;++b) assert(out[7+b]==other[5-b]);
        for(size_t cap=0;cap<n;++cap) {
            memset(out,0xa5,sizeof(out));memcpy(before,out,sizeof(out));
            assert(!aacp_route_build(out+1,cap,golden[i].kind,local,other));
            assert(!memcmp(out,before,sizeof(out)));
        }
        assert(!aacp_route_build(NULL,AACP_ROUTE_MAX_PACKET,golden[i].kind,local,other));
        assert(!aacp_route_build(out,AACP_ROUTE_MAX_PACKET,golden[i].kind,NULL,other));
        assert(!aacp_route_build(out,AACP_ROUTE_MAX_PACKET,golden[i].kind,local,NULL));
        assert(!aacp_route_build(out,AACP_ROUTE_MAX_PACKET,golden[i].kind,local,local));
    }
    const uint8_t own[]={4,0,4,0,9,0,6,1,0,0,0};bool owns=false;
    assert(aacp_route_build(out,sizeof(out),AACP_ROUTE_OWN,NULL,NULL)==sizeof(own));
    assert(!memcmp(out,own,sizeof(own)) && aacp_route_ownership(out,sizeof(own),&owns) && owns);
    assert(!aacp_route_build(out,sizeof(out),(aacp_route_kind_t)99,local,other));
    uint8_t invalid[6]={0};assert(!aacp_route_build(out,sizeof(out),AACP_ROUTE_MEDIA,invalid,other));
    memset(invalid,0xff,6);assert(!aacp_route_build(out,sizeof(out),AACP_ROUTE_MEDIA,local,invalid));
}
static void ownership_and_source(void) {
    uint8_t own[]={4,0,4,0,9,0,6,1,0,0,0};bool owns=false;
    for(size_t n=0;n<sizeof(own);++n) assert(!aacp_route_ownership(own,n,&owns));
    assert(aacp_route_ownership(own,sizeof(own),&owns) && owns);
    own[7]=0;assert(aacp_route_ownership(own,sizeof(own),&owns) && !owns);
    own[7]=2;assert(aacp_route_ownership(own,sizeof(own),&owns) && !owns);
    own[7]=3;owns=true;assert(!aacp_route_ownership(own,sizeof(own),&owns) && owns);
    own[7]=1;own[10]=1;assert(!aacp_route_ownership(own,sizeof(own),&owns));
    own[10]=0;own[6]=7;assert(!aacp_route_ownership(own,sizeof(own),&owns));
    assert(!aacp_route_ownership(NULL,11,&owns) && !aacp_route_ownership(own,11,NULL));
    uint8_t source[]={4,0,4,0,0x0e,0,0x76,0x65,0x54,0x43,0x32,0x21,2};
    uint8_t address[6],type=0;
    for(size_t n=0;n<sizeof(source);++n) assert(!aacp_route_source(source,n,address,&type));
    assert(aacp_route_source(source,sizeof(source),address,&type) && type==2 && !memcmp(address,other,6));
    source[12]=1;assert(aacp_route_source(source,sizeof(source),address,&type) && type==1);
    source[12]=3;assert(!aacp_route_source(source,sizeof(source),address,&type));
    memset(source+6,0,6);source[12]=1;assert(!aacp_route_source(source,sizeof(source),address,&type));
    source[12]=0;assert(aacp_route_source(source,sizeof(source),address,&type) && !type);
    assert(!aacp_route_source(NULL,13,address,&type) && !aacp_route_source(source,13,NULL,&type));
    assert(!aacp_route_source(source,13,address,NULL));
}
static void device_lists(void) {
    uint8_t p[33]={4,0,4,0,0x2e,0,0,0,0},found[6];bool sole=true;
    memset(found,0xa5,6);assert(aacp_route_devices(p,9,local,found,&sole) && !sole && !aacp_route_address_valid(found));
    p[8]=1;memcpy(p+9,local,6);p[15]=7;p[16]=0x80;
    assert(aacp_route_devices(p,17,local,found,&sole) && sole && !aacp_route_address_valid(found));
    memcpy(p+9,other,6);assert(aacp_route_devices(p,17,local,found,&sole) && !sole && !memcmp(found,other,6));
    p[8]=2;memcpy(p+17,local,6);p[23]=1;p[24]=0xff;
    assert(aacp_route_devices(p,25,local,found,&sole) && !sole && !memcmp(found,other,6));
    memcpy(p+9,local,6);memcpy(p+17,other,6);
    assert(aacp_route_devices(p,25,local,found,&sole) && !sole && !memcmp(found,other,6));
    for(size_t n=0;n<25;++n) assert(!aacp_route_devices(p,n,local,found,&sole));
    assert(!aacp_route_devices(p,26,local,found,&sole));
    memcpy(p+17,local,6);assert(!aacp_route_devices(p,25,local,found,&sole));
    memcpy(p+9,other,6);assert(aacp_route_devices(p,25,local,found,&sole));
    p[17]=0x99;assert(!aacp_route_devices(p,25,local,found,&sole)); // Two other hosts cannot fit the two-Pico route.
    p[8]=3;assert(!aacp_route_devices(p,33,local,found,&sole));
    p[8]=255;assert(!aacp_route_devices(p,sizeof(p),local,found,&sole));
    p[8]=1;memset(p+9,0,6);assert(!aacp_route_devices(p,17,local,found,&sole));
    memcpy(p+9,local,6);p[3]=1;assert(!aacp_route_devices(p,17,local,found,&sole));
    assert(!aacp_route_devices(NULL,25,local,found,&sole) && !aacp_route_devices(p,17,NULL,found,&sole));
}
static void relayed_ownership(void) {
    uint8_t p[AACP_ROUTE_MAX_PACKET+1];
    for(unsigned reverse=0;reverse<2;++reverse) {
        size_t n=aacp_route_build(p,AACP_ROUTE_MAX_PACKET,reverse?AACP_ROUTE_REVERSE:AACP_ROUTE_HIJACK,local,other);
        p[4]=0x11; // AirPods relay changes opcode and supplies sender in the same reversed-address slot.
        assert(aacp_route_hijack(p,n,other));
        for(size_t short_n=0;short_n<n;++short_n) assert(!aacp_route_hijack(p,short_n,other));
        assert(!aacp_route_hijack(p,n,local));
        assert(!aacp_route_hijack(p,n+1,other));
        p[13]=0xff;assert(!aacp_route_hijack(p,n,other));p[13]=0;
        p[12]--;assert(!aacp_route_hijack(p,n,other));p[12]++;
        // Never recognize a token copied into malformed/unknown dictionary bytes.
        for(size_t i=14;i<n;++i) {p[i]^=0x80;assert(!aacp_route_hijack(p,n,other));p[i]^=0x80;}
        p[4]=0x10;assert(!aacp_route_hijack(p,n,other));
    }
    assert(!aacp_route_hijack(NULL,112,other) && !aacp_route_hijack(p,112,NULL));
}
int main(void) {
    builders();ownership_and_source();device_lists();relayed_ownership();
    puts("AACP routing: exact LibrePods builders, address ordering, known relay schemas and bounded passive parsing passed");
}
