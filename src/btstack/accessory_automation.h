// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_ACCESSORY_AUTOMATION_H
#define AIRPODS_ACCESSORY_AUTOMATION_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    ACCESSORY_AUTOMATION_NONE=0, ACCESSORY_AUTOMATION_CANCEL=1,
    ACCESSORY_AUTOMATION_PAUSE=2, ACCESSORY_AUTOMATION_PLAY=4
};
enum {
    ACCESSORY_AUTO_IDLE, ACCESSORY_AUTO_PAUSE_REQUESTED, ACCESSORY_AUTO_PAUSE_SENT,
    ACCESSORY_AUTO_PAUSE_DELIVERED, ACCESSORY_AUTO_PAUSED,
    ACCESSORY_AUTO_PLAY_REQUESTED, ACCESSORY_AUTO_PLAY_SENT
};
enum { ACCESSORY_EARS_NONE, ACCESSORY_EARS_PARTIAL, ACCESSORY_EARS_FULL };

typedef struct {
    uint32_t pause_since, pause_receipt, speech_since;
    uint8_t phase, presence, wear_action;
    bool ear_enabled, ca_enabled, have_presence, resume_pending, cancel, duck;
} accessory_automation_t;

static inline void accessory_automation_cancel_media(accessory_automation_t *s) {
    s->phase=ACCESSORY_AUTO_IDLE;
    s->wear_action=ACCESSORY_AUTOMATION_NONE;
    s->resume_pending=false;
    s->cancel=true;
}

// Scope owners call reset when the peer, music route, session or USB host changes.
static inline void accessory_automation_reset(accessory_automation_t *s) {
    memset(s,0,sizeof(*s));
    s->ear_enabled=s->ca_enabled=true;
    s->cancel=true;
}

// Accepted choices control host behavior without fabricating an AirPods report.
static inline void accessory_automation_choice(accessory_automation_t *s, uint8_t id, uint32_t value) {
    if(value!=1 && value!=2)return;
    if(id==0x0a) {
        s->ear_enabled=value==2;
        if(!s->ear_enabled)accessory_automation_cancel_media(s);
    } else if(id==0x28) {
        s->ca_enabled=value==1;
        if(!s->ca_enabled)s->duck=false;
    }
}

static inline bool accessory_automation_duck(accessory_automation_t *s, uint32_t now) {
    if(s->duck && (uint32_t)(now-s->speech_since)>=30000u)s->duck=false;
    return s->duck;
}

// The caller admits only current, known-model, owned music packets.
static inline void accessory_automation_packet(accessory_automation_t *s, const uint8_t *p,
                                                uint16_t n, uint32_t now) {
    static const uint8_t header[]={4,0,4,0};
    if(!p || n<6 || memcmp(p,header,sizeof(header)) || p[5])return;
    accessory_automation_duck(s,now);
    if(p[4]==9 && n==11) {
        uint32_t value=(uint32_t)p[7]|(uint32_t)p[8]<<8|(uint32_t)p[9]<<16|(uint32_t)p[10]<<24;
        accessory_automation_choice(s,p[6],value);
    } else if(p[4]==6 && n>=8 && p[6]<=3 && p[7]<=3) {
        unsigned in=(p[6]==0)+(p[7]==0);
        uint8_t presence=in==2 || (in==1 && (p[6]==2 || p[7]==2)) ? ACCESSORY_EARS_FULL :
            in==1 ? ACCESSORY_EARS_PARTIAL : ACCESSORY_EARS_NONE;
        if(s->have_presence && presence!=s->presence && s->ear_enabled) {
            if((s->presence==ACCESSORY_EARS_FULL && presence==ACCESSORY_EARS_PARTIAL) ||
               (s->presence!=ACCESSORY_EARS_NONE && presence==ACCESSORY_EARS_NONE))
                s->wear_action=ACCESSORY_AUTOMATION_PAUSE;
            else if(presence==ACCESSORY_EARS_FULL || s->presence==ACCESSORY_EARS_NONE)
                s->wear_action=ACCESSORY_AUTOMATION_PLAY;
        }
        s->have_presence=true;s->presence=presence;
    } else if(p[4]==0x4b && n>=10 && s->ca_enabled) {
        uint8_t stage=p[9];
        if(stage==1 || ((stage==2 || stage==3) && s->duck)) {
            s->duck=true;s->speech_since=now;
        } else if(stage==4 || (stage>=6 && stage<=9))s->duck=false;
    }
}

// signal_recent authorizes Pause; source_active is fresh nonzero PCM, not an open USB endpoint.
static inline uint8_t accessory_automation_poll(accessory_automation_t *s, uint32_t now,
    bool signal_recent, bool source_active, uint32_t pause_receipt) {
    uint8_t result=s->cancel ? ACCESSORY_AUTOMATION_CANCEL : ACCESSORY_AUTOMATION_NONE;
    s->cancel=false;
    accessory_automation_duck(s,now);
    if(s->phase==ACCESSORY_AUTO_PAUSE_SENT && pause_receipt!=s->pause_receipt)
        s->phase=ACCESSORY_AUTO_PAUSE_DELIVERED;
    if(((s->phase==ACCESSORY_AUTO_PAUSE_SENT || s->phase==ACCESSORY_AUTO_PAUSE_DELIVERED ||
         s->phase==ACCESSORY_AUTO_PLAY_SENT) && (uint32_t)(now-s->pause_since)>=1500u) ||
       (s->phase==ACCESSORY_AUTO_PAUSED && (uint32_t)(now-s->pause_since)>=120000u))
        accessory_automation_cancel_media(s);
    uint8_t wear=s->wear_action;s->wear_action=ACCESSORY_AUTOMATION_NONE;
    if(wear==ACCESSORY_AUTOMATION_PAUSE) {
        s->resume_pending=false;
        if(s->phase==ACCESSORY_AUTO_PLAY_SENT || s->phase==ACCESSORY_AUTO_PLAY_REQUESTED)
            accessory_automation_cancel_media(s);
        if(s->phase==ACCESSORY_AUTO_IDLE && signal_recent && s->ear_enabled) {
            s->phase=ACCESSORY_AUTO_PAUSE_REQUESTED;
            result|=ACCESSORY_AUTOMATION_PAUSE;
        }
    } else if(wear==ACCESSORY_AUTOMATION_PLAY) {
        if(s->phase==ACCESSORY_AUTO_PAUSE_SENT || s->phase==ACCESSORY_AUTO_PAUSE_REQUESTED)
            accessory_automation_cancel_media(s);
        else if(s->phase==ACCESSORY_AUTO_PAUSE_DELIVERED || s->phase==ACCESSORY_AUTO_PAUSED)
            s->resume_pending=true;
    }
    if(s->phase==ACCESSORY_AUTO_PAUSE_DELIVERED && !source_active)s->phase=ACCESSORY_AUTO_PAUSED;
    if(s->phase==ACCESSORY_AUTO_PAUSED) {
        if(source_active)accessory_automation_cancel_media(s);
        else if(s->resume_pending && s->ear_enabled && s->presence!=ACCESSORY_EARS_NONE) {
            s->phase=ACCESSORY_AUTO_PLAY_REQUESTED;s->resume_pending=false;
            result|=ACCESSORY_AUTOMATION_PLAY;
        }
    } else if(s->phase==ACCESSORY_AUTO_PLAY_SENT && source_active)
        accessory_automation_cancel_media(s);
    if(s->cancel) {result|=ACCESSORY_AUTOMATION_CANCEL;s->cancel=false;}
    return result;
}

// Capture the delivery counter before enqueue. A rejected enqueue retires the action.
static inline void accessory_automation_sent(accessory_automation_t *s, uint8_t action,
    bool accepted, uint32_t pause_receipt_before, uint32_t now) {
    if(action==ACCESSORY_AUTOMATION_PAUSE && s->phase==ACCESSORY_AUTO_PAUSE_REQUESTED) {
        if(!accepted) {accessory_automation_cancel_media(s);return;}
        s->pause_receipt=pause_receipt_before;s->pause_since=now;s->phase=ACCESSORY_AUTO_PAUSE_SENT;
    } else if(action==ACCESSORY_AUTOMATION_PLAY && s->phase==ACCESSORY_AUTO_PLAY_REQUESTED) {
        if(!accepted) {accessory_automation_cancel_media(s);return;}
        s->pause_since=now;s->phase=ACCESSORY_AUTO_PLAY_SENT;
    }
}

#endif
