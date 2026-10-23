// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/dongle_settings.h"

static unsigned metadata(uint8_t *out, const char *model, const char *firmware, const char *packed) {
    const char *fields[]={"AirPods",model,"Apple Inc.","EXAMPLE",firmware,"","1","com.apple.accessory.updater","LEFT","RIGHT",packed};
    const uint8_t header[]={4,0,4,0,0x1d,0,2,0,0,4,0};
    memcpy(out,header,sizeof(header)); unsigned size=sizeof(header);
    for (unsigned i=0;i<11;++i) {
        unsigned n=(unsigned)strlen(fields[i])+1; memcpy(out+size,fields[i],n); size+=n;
    }
    return size;
}
static void reported(dongle_settings_t *s, uint8_t id, uint32_t value, uint32_t now) {
    uint8_t p[]={4,0,4,0,9,0,id,0,0,0,0}; dongle_put32(p+7,value);
    settings_packet(s,p,sizeof(p),now);
}
static void identify(dongle_settings_t *s, const char *model, const char *version, const char *packed) {
    uint8_t packet[384]; unsigned n=metadata(packet,model,version,packed);
    settings_packet(s,packet,n,0);
}
static void test_values(void) {
    const struct { uint8_t id; uint32_t good,bad; } cases[]={
        {1,2,3},{0x0a,2,0},{0x0d,4,5},{0x16,0x0501,0x01050501},
        {0x17,2,3},{0x18,2,3},{0x1a,6,16},{0x1b,1,3},{0x1c,2,3},
        {0x1f,0x5064,0x5065},{0x20,1,3},{0x23,0,3},{0x24,0x0300,3},
        {0x25,1,3},{0x26,2,0},{0x28,1,0x0101},{0x2e,100,101},
        {0x31,1,0},{0x34,2,3},{0x35,1,3},{0x36,1,3},{0x3b,1,3},{0x40,100,101},
    };
    for (unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        assert(setting_value_valid(cases[i].id,cases[i].good));
        assert(!setting_value_valid(cases[i].id,cases[i].bad));
        assert(!setting_value_valid(cases[i].id,UINT32_MAX));
    }
    assert(!setting_value_valid(3,1) && !setting_value_valid(0x1a,1));
}
static void test_model_and_firmware(void) {
    dongle_settings_t s; settings_reset(&s);
    reported(&s,0x0d,3,0); reported(&s,0x28,2,0);
    assert(settings_supported(&s,0x0d,2) && !settings_supported(&s,0x0d,4));
    assert(!settings_supported(&s,0x28,1));
    identify(&s,"A2083","6F21","");
    assert(s.profile && s.firmware_major==6);
    assert(settings_supported(&s,0x0d,1) && !settings_supported(&s,0x0d,4));
    assert(!settings_supported(&s,0x28,1) && settings_supported(&s,1,0));
    identify(&s,"A3063","90.3431.6807","9442752");
    assert(s.profile && s.firmware_major==9 && settings_supported(&s,0x0d,4));
    assert(settings_supported(&s,0x28,1) && !settings_supported(&s,0x0d,1));
    assert(settings_supported(&s,0x34,1));
    s.allow_off_requested=1;
    assert(settings_supported(&s,0x0d,1));
    identify(&s,"A3063","9A348","");
    assert(s.allow_off_requested==1);
    reported(&s,0x34,2,1);
    assert(s.allow_off_requested==-1 && !settings_supported(&s,0x0d,1));
    identify(&s,"A3063","90.3431.6807","6619473");
    assert(s.firmware_major==0 && !settings_supported(&s,0x0d,4));
    identify(&s,"A0000","9A348","");
    assert(!s.profile && !settings_supported(&s,1,0));
    identify(&s,"A1523","6.8.8","");
    assert(s.profile && s.firmware_major==6 && !settings_supported(&s,0x0d,2));
    identify(&s,"A3053","9A348","");
    assert(settings_supported(&s,0x16,0x0505) && !settings_supported(&s,0x16,0x0105));
    identify(&s,"A3454","7E108",""); reported(&s,0x35,2,0);
    assert(!settings_supported(&s,0x35,1));
    identify(&s,"A3454","8B34","");
    assert(settings_supported(&s,0x35,1));
    const struct { const char *text; uint8_t major; } firmware[]={
        {"6F21",6},{"9A5347a",9},{"6.8.8",6},{"9A348garbage",0},
        {"99A1",99},{"100A1",0},{"9",0},{"9A",0},{"90.3431.6807",0},
        {"6.256.1",0},{"6.1.1junk",0},{"6619472",0},{"",0},
    };
    for (unsigned i=0;i<sizeof(firmware)/sizeof(*firmware);++i)
        assert(setting_firmware_major((const uint8_t *)firmware[i].text,(unsigned)strlen(firmware[i].text))==firmware[i].major);
    assert(setting_packed_major((const uint8_t *)"4294967296",10)==0);
}
static void test_all_hold_cycle_combinations(void) {
    const uint8_t masks[]={3,5,6,7,9,10,11,12,13,14,15};
    dongle_settings_t s; settings_reset(&s);
    identify(&s,"A3063","9A348",""); reported(&s,0x34,1,0);
    reported(&s,0x16,0x0501,0);
    for (unsigned value=0;value<256;++value) {
        bool valid=memchr(masks,(int)value,sizeof(masks))!=NULL;
        assert(setting_value_valid(0x1a,value)==valid);
        assert(settings_supported(&s,0x1a,value)==valid);
    }
    assert(!setting_value_valid(0x1a,UINT32_C(0x1000b)));
    for (unsigned i=0;i<sizeof(masks);++i) {
        reported(&s,0x1a,masks[i],i+1);
        assert(setting_report_is(&s,0x1a,masks[i]) && setting_report_is(&s,0x16,0x0501));
    }
    reported(&s,0x34,2,20);
    for (unsigned i=0;i<sizeof(masks);++i)
        assert(settings_supported(&s,0x1a,masks[i])==!(masks[i]&1));
    identify(&s,"A2083","6F21",""); reported(&s,0x34,1,21);
    for (unsigned i=0;i<sizeof(masks);++i)
        assert(settings_supported(&s,0x1a,masks[i])==!(masks[i]&8));
    identify(&s,"A2931","5E135",""); reported(&s,0x34,1,21);
    assert(settings_supported(&s,0x1a,7) && !settings_supported(&s,0x1a,11));
    identify(&s,"A2931","6A300","");
    assert(settings_supported(&s,0x1a,11));
}
static void test_metadata_bounds(void) {
    dongle_settings_t s;
    uint8_t packet[385]; unsigned n=metadata(packet,"A3063","9A348","");
    for (unsigned i=0;i<n;++i) {
        settings_reset(&s); identify(&s,"A3063","9A348","");
        settings_metadata(&s,packet,i);
        assert(!s.profile && s.firmware_major==0);
    }
    settings_metadata(&s,packet,n); assert(s.profile);
    settings_metadata(&s,packet,sizeof(packet)); assert(!s.profile);
    packet[11]=0x1f;
    settings_metadata(&s,packet,n); assert(!s.profile);
}
static void test_report_replacement_and_snapshot(void) {
    dongle_settings_t s; settings_reset(&s); uint8_t out[512];
    assert(settings_snapshot(&s,0,out,sizeof(out))==2 && out[0]==1 && out[1]==0);
    reported(&s,0x28,2,UINT32_MAX-3);
    assert(settings_snapshot(&s,4,out,sizeof(out))==15 && out[1]==1 && out[2]==0x28);
    assert(dongle_u32(out+3)==8 && dongle_u32(out+7)==1 && dongle_u32(out+11)==2);
    reported(&s,0x28,2,4);
    assert(settings_snapshot(&s,4,out,sizeof(out))==15 && dongle_u32(out+7)==2);
    uint8_t bad[]={4,0,4,0,9,0,0x28,1,1,0,0};
    settings_packet(&s,bad,sizeof(bad),5);
    assert(settings_snapshot(&s,5,out,sizeof(out))==2 && out[1]==0);
    int index=setting_index(0x28); assert(s.reports[index].revision==3 && !s.reports[index].known);
    s.reports[index].revision=UINT32_MAX; reported(&s,0x28,1,6);
    assert(s.reports[index].revision==1);
    bad[8]=0; settings_packet(&s,bad,8,7);
    assert(!s.reports[index].known && s.reports[index].revision==2);
    bad[5]=1; settings_packet(&s,bad,sizeof(bad),8);
    assert(s.reports[index].revision==2);
    reported(&s,0x16,0xff000501,8);
    assert(setting_report_is(&s,0x16,0x0501));
    memset(out,0xa5,sizeof(out));
    assert(!settings_snapshot(&s,8,out,14) && out[0]==0xa5);
    assert(!settings_snapshot(&s,8,NULL,512));
    settings_reset(&s); assert(settings_snapshot(&s,8,out,sizeof(out))==2 && !out[1]);
}
int main(void) {
    test_values(); test_model_and_firmware(); test_all_hold_cycle_combinations();
    test_metadata_bounds(); test_report_replacement_and_snapshot();
    puts("dongle settings: bounded catalog, firmware gates, malformed replacement and non-optimistic snapshots passed");
}
