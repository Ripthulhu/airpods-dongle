// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_DONGLE_SETTINGS_H
#define AIRPODS_DONGLE_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../tinyusb/dongle_protocol.h"

// Value validation and model masks mirror NTPods device_settings/device-models.
static const uint8_t setting_ids[] = {
    0x01,0x0a,0x0d,0x16,0x17,0x18,0x1a,0x1b,0x1c,0x1f,0x20,0x23,
    0x24,0x25,0x26,0x28,0x2e,0x31,0x34,0x35,0x36,0x3b,0x40,
    0x37,0x2f // Append controls to preserve the existing model-mask bit positions.
};
#define DONGLE_SETTING_COUNT (sizeof(setting_ids) / sizeof(setting_ids[0]))
#define DONGLE_LEGACY_SETTING_COUNT 23u
typedef struct {
    uint16_t models[3];
    uint32_t controls;
    bool listening, adaptive, allow_off_option, silent_allow_off;
} setting_profile_t;
static const setting_profile_t setting_profiles[] = {
    {{1523,1722,0},       0x00000203,false,false,false,false},
    {{2032,2031,0},       0x00000603,false,false,false,false},
    {{2565,2564,0},       0x0020163b,false,false,false,false},
    {{3053,3050,3054},    0x0028563b,false,false,false,false},
    {{3056,3055,3057},    0x006bd6ff,true,true,false,false},
    {{3531,3532,3533},    0x0029d6ff,true,true,false,false},
    {{3439,3440,3441},    0x006bfeff,true,true,false,false},
    {{2084,2083,0},       0x002016ff,true,false,false,false},
    {{2931,2699,2698},    0x017ffeff,true,true,true,false},
    {{3047,3048,3049},    0x017ffeff,true,true,true,false},
    {{3063,3064,3065},    0x01fffeff,true,true,true,true},
    {{2096,3184,0},       0x00201746,true,false,false,false},
    {{3454,0,0},          0x0029d746,true,true,false,false},
    {{3157,0,0},          0x00000006,true,false,false,false},
};
typedef struct { uint32_t value, updated, revision; bool known; } setting_report_t;
typedef struct {
    setting_report_t reports[DONGLE_SETTING_COUNT];
    const setting_profile_t *profile;
    uint8_t firmware_major;
    int8_t allow_off_requested;
} dongle_settings_t;

static inline int setting_index(uint8_t id) {
    for (unsigned i=0;i<DONGLE_SETTING_COUNT;++i) if (setting_ids[i]==id) return (int)i;
    return -1;
}
static inline bool setting_value_valid(uint8_t id, uint32_t value) {
    switch(id) {
        case 0x0a: case 0x1b: case 0x20: case 0x25: case 0x26: case 0x28:
        case 0x2f: case 0x31: case 0x34: case 0x35: case 0x36: case 0x37: case 0x3b: case 0x1c:
            return value==1 || value==2;
        case 0x01: case 0x17: case 0x18: case 0x23: return value<=2;
        case 0x0d: return value>=1 && value<=4;
        case 0x16: return value<=0xffff && ((value&255)==1 || (value&255)==5) &&
            (((value>>8)&255)==1 || ((value>>8)&255)==5);
        case 0x1a: {
            unsigned bits=0;
            for (unsigned i=0;i<4;++i) bits+=(value>>i)&1u;
            return value<=15 && bits>=2;
        }
        case 0x1f: return (value>>8)==0x50 && (value&255)<=100;
        case 0x24: return value==0x0200 || value==0x0300;
        case 0x2e: case 0x40: return value<=100;
        default: return false;
    }
}
static inline bool setting_explicit_choice(uint8_t id) {
    return id==0x01 || id==0x0a || id==0x16 || id==0x1a || id==0x23 || id==0x31 || id==0x40;
}
static inline void settings_reset(dongle_settings_t *s) {
    memset(s,0,sizeof(*s)); s->allow_off_requested=-1;
}
static inline uint8_t setting_firmware_major(const uint8_t *p, unsigned n) {
    unsigned i=0, major=0;
    while(i<n && p[i]>='0' && p[i]<='9') {
        if (major>99) return 0;
        major=major*10+p[i++]-'0';
    }
    if (!i || major<1 || major>99) return 0;
    if (i<n && p[i]>='A' && p[i]<='Z') {
        ++i; unsigned start=i;
        while(i<n && p[i]>='0' && p[i]<='9') ++i;
        return i>start && (i==n || (i+1==n && p[i]>='a' && p[i]<='z')) ? (uint8_t)major : 0;
    }
    if (i>=n || p[i++]!='.') return 0;
    for (unsigned part=0;part<2;++part) {
        unsigned value=0, start=i;
        while(i<n && p[i]>='0' && p[i]<='9') {
            value=value*10+p[i++]-'0';
            if (value>255) return 0;
        }
        if (i==start || (part==0 && (i>=n || p[i++]!='.'))) return 0;
    }
    return i==n ? (uint8_t)major : 0;
}
static inline uint8_t setting_packed_major(const uint8_t *p, unsigned n) {
    uint32_t value=0;
    if (!n) return 0;
    for (unsigned i=0;i<n;++i) {
        if (p[i]<'0' || p[i]>'9' || value>(UINT32_MAX-(p[i]-'0'))/10u) return 0;
        value=value*10u+p[i]-'0';
    }
    unsigned major=value>>20;
    return (value&15)==0 && major>=1 && major<=99 ? (uint8_t)major : 0;
}
static inline void settings_metadata(dongle_settings_t *s, const uint8_t *p, unsigned n) {
    const setting_profile_t *previous=s->profile;
    int8_t requested=s->allow_off_requested;
    s->profile=NULL; s->firmware_major=0; s->allow_off_requested=-1;
    if (n<11 || n>384 || p[4]!=0x1d || p[5]) return;
    const uint8_t *fields[11]; unsigned lengths[11], offset=11;
    for (unsigned i=0;i<11;++i) {
        if (offset>=n) return;
        const uint8_t *end=memchr(p+offset,0,n-offset);
        if (!end) return;
        fields[i]=p+offset; lengths[i]=(unsigned)(end-(p+offset));
        for (unsigned j=0;j<lengths[i];++j) if (fields[i][j]<32 || fields[i][j]==127) return;
        offset+=(unsigned)lengths[i]+1;
    }
    if (lengths[1]!=5 || fields[1][0]!='A') return;
    unsigned model=0;
    for (unsigned i=1;i<5;++i) {
        if (fields[1][i]<'0' || fields[1][i]>'9') return;
        model=model*10+fields[1][i]-'0';
    }
    for (unsigned i=0;i<sizeof(setting_profiles)/sizeof(*setting_profiles);++i)
        for (unsigned j=0;j<3;++j)
            if (model && setting_profiles[i].models[j]==model) s->profile=&setting_profiles[i];
    if (s->profile && s->profile==previous) s->allow_off_requested=requested;
    s->firmware_major=setting_firmware_major(fields[10],lengths[10]);
    if (!s->firmware_major) s->firmware_major=setting_firmware_major(fields[4],lengths[4]);
    if (!s->firmware_major) s->firmware_major=setting_packed_major(fields[10],lengths[10]);
}
static inline void settings_packet(dongle_settings_t *s, const uint8_t *p, unsigned n, uint32_t now) {
    if (!p || n<6 || memcmp(p,"\x04\0\x04\0",4) || p[5]) return;
    if (p[4]==0x1d) { settings_metadata(s,p,n); return; }
    if (p[4]!=9 || n<7) return;
    int index=setting_index(p[6]);
    if (index<0) return;
    setting_report_t *r=&s->reports[index];
    if (++r->revision==0) ++r->revision;
    r->known=false;
    if (n!=11) return;
    uint32_t value=dongle_u32(p+7);
    if (p[6]==0x16) value&=0xffff; // Upper bytes hold previous actions, not commands.
    if (!setting_value_valid(p[6],value)) return;
    if (p[6]==0x34) s->allow_off_requested=-1;
    r->value=value; r->updated=now; r->known=true;
}
static inline bool setting_report_is(const dongle_settings_t *s, uint8_t id, uint32_t value) {
    int index=setting_index(id);
    return index>=0 && s->reports[index].known && s->reports[index].value==value;
}
static inline bool settings_allow_off(const dongle_settings_t *s) {
    if (s->profile) {
        if (!s->profile->listening) return false;
        if (!s->profile->allow_off_option) return !setting_report_is(s,0x34,2);
    }
    if (s->allow_off_requested>=0) return s->allow_off_requested!=0;
    if (setting_report_is(s,0x34,1)) return true;
    if (setting_report_is(s,0x34,2)) return false;
    return setting_report_is(s,0x0d,1) || (s->profile && s->firmware_major>=1 && s->firmware_major<=6);
}
static inline bool settings_supported(const dongle_settings_t *s, uint8_t id, uint32_t value) {
    int index=setting_index(id);
    if (index<0 || !setting_value_valid(id,value)) return false;
    bool supported=s->profile ? (s->profile->controls&(UINT32_C(1)<<index))!=0 :
        id==0x01 || id==0x0a || id==0x0d || id==0x16 || id==0x17 || id==0x18 ||
        id==0x1a || id==0x1b || id==0x1f || id==0x23 || id==0x24 || id==0x31 || id==0x3b || id==0x40;
    if (!supported) return false;
    bool adaptive=s->profile && s->profile->adaptive && s->firmware_major>=6;
    if (id==0x16 && s->profile && !s->profile->listening &&
        ((value&255)==1 || ((value>>8)&255)==1)) return false;
    if (id==0x0d && ((value==4 && !adaptive) || (value==1 && !settings_allow_off(s)))) return false;
    if (id==0x1a && (((value&8) && !adaptive) || ((value&1) && !settings_allow_off(s)))) return false;
    if ((id==0x2e && !adaptive) || (id==0x35 && s->firmware_major<8)) return false;
    return s->reports[index].known || (s->profile && setting_explicit_choice(id)) ||
        (id==0x34 && s->profile && s->profile->silent_allow_off);
}
static inline uint16_t settings_snapshot(const dongle_settings_t *s, uint32_t now,
                                         uint8_t *out, uint16_t capacity, bool extended) {
    unsigned limit=extended ? DONGLE_SETTING_COUNT : DONGLE_LEGACY_SETTING_COUNT;
    unsigned count=0;
    for (unsigned i=0;i<limit;++i) if (s->reports[i].known) ++count;
    if (!out || capacity<2+count*13) return 0;
    out[0]=1; out[1]=(uint8_t)count;
    unsigned offset=2;
    for (unsigned i=0;i<limit;++i) {
        const setting_report_t *r=&s->reports[i];
        if (!r->known) continue;
        out[offset]=setting_ids[i];
        dongle_put32(out+offset+1,now-r->updated);
        dongle_put32(out+offset+5,r->revision);
        dongle_put32(out+offset+9,r->value);
        offset+=13;
    }
    return (uint16_t)offset;
}
#endif
