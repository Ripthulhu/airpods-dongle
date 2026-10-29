// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/volume_authority.h"

static const volume_authority_scope_t live={
    .usb_generation=3,.control_generation=7,.cid=9,.peer={1,2,3,4,5,6},.ready=true};

static void acquire_renew_set_release(void) {
    volume_authority_t s={0};
    assert(volume_authority_manage(&s,0,&live,0,0,0,100)==DONGLE_ACCEPTED);
    uint32_t first=s.token;
    assert(first && volume_authority_owner(&s,0,&live,100));
    assert(!volume_authority_owner(&s,1,&live,100));
    assert(volume_authority_remaining(&s,&live,100)==5000);
    assert(volume_authority_manage(&s,0,&live,0,0,0,200)==DONGLE_BUSY);
    assert(volume_authority_manage(&s,0,&live,1,first+1,0,200)==DONGLE_STALE);
    assert(volume_authority_manage(&s,0,&live,1,first,0,4000)==DONGLE_ACCEPTED);
    assert(volume_authority_remaining(&s,&live,5000)==4000);
    assert(volume_authority_manage(&s,0,&live,3,first,0,6000)==DONGLE_ACCEPTED);
    assert(volume_authority_manage(&s,0,&live,3,first,127,8000)==DONGLE_ACCEPTED);
    assert(volume_authority_manage(&s,0,&live,2,first,0,9000)==DONGLE_ACCEPTED);
    assert(!s.active && !s.token && !volume_authority_remaining(&s,&live,9000));
    assert(volume_authority_manage(&s,0,&live,0,0,0,9000)==DONGLE_ACCEPTED);
    assert(s.token!=first && volume_authority_manage(&s,0,&live,3,first,64,9000)==DONGLE_STALE);
}

static void malformed_and_foreign_requests_do_not_mutate(void) {
    volume_authority_t s={0};
    assert(volume_authority_manage(&s,0,&live,0,0,0,0)==DONGLE_ACCEPTED);
    volume_authority_t before=s;
    const struct {uint8_t action,level;uint32_t token;} bad[]={
        {4,0,1},{0,0,1},{0,1,0},{1,1,1},{2,1,1},{3,128,1},{1,0,0},{2,0,0},{3,64,0}};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        assert(volume_authority_manage(&s,0,&live,bad[i].action,bad[i].token,bad[i].level,1)==DONGLE_MALFORMED);
        assert(!memcmp(&s,&before,sizeof(s)));
    }
    assert(volume_authority_manage(&s,1,&live,1,s.token,0,1)==DONGLE_UNSUPPORTED);
    assert(!memcmp(&s,&before,sizeof(s)));
}

static void every_scope_change_revokes_once(void) {
    for (unsigned which=0;which<6;++which) {
        volume_authority_t s={0};volume_authority_scope_t scope=live;
        assert(volume_authority_manage(&s,0,&scope,0,0,0,10)==DONGLE_ACCEPTED);
        switch(which) {
            case 0:scope.usb_generation+=2;break;
            case 1:scope.control_generation++;break;
            case 2:scope.cid++;break;
            case 3:scope.peer[0]++;break;
            case 4:scope.ready=false;break;
            case 5:scope.usb_generation&=~1u;break;
        }
        assert(!volume_authority_current(&s,&scope,11));
        assert(volume_authority_poll(&s,&scope,11));
        assert(!s.active && !s.token && !volume_authority_poll(&s,&scope,11));
    }
    volume_authority_t s={0};
    assert(volume_authority_manage(&s,0,&live,0,0,0,0)==DONGLE_ACCEPTED);
    assert(!volume_authority_poll(&s,&live,4999)); // A pause leaves this scope intact.
    assert(!volume_authority_disconnect(&s,1) && s.active);
    assert(volume_authority_disconnect(&s,0) && !s.active && !s.token);
}

static void deadline_wrap_and_token_wrap(void) {
    volume_authority_t s={.serial=UINT32_MAX};
    uint32_t start=UINT32_MAX-2000u;
    assert(volume_authority_manage(&s,0,&live,0,0,0,start)==DONGLE_ACCEPTED);
    assert(s.token==1 && volume_authority_remaining(&s,&live,start)==5000);
    assert(volume_authority_current(&s,&live,start+4999u));
    assert(!volume_authority_current(&s,&live,start+5000u));
    assert(volume_authority_manage(&s,0,&live,1,s.token,0,start+5000u)==DONGLE_STALE);
    assert(volume_authority_poll(&s,&live,start+5000u) && !s.active);
    volume_authority_scope_t bad=live;memset(bad.peer,0,sizeof(bad.peer));
    assert(volume_authority_manage(&s,0,&bad,0,0,0,0)==DONGLE_DISCONNECTED);
    assert(!s.active);
}

int main(void) {
    acquire_renew_set_release();malformed_and_foreign_requests_do_not_mutate();
    every_scope_change_revokes_once();deadline_wrap_and_token_wrap();
    puts("volume authority: exact ownership, refresh/release, scope revocation, malformed immutability and wrap passed");
}
