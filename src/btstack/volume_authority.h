// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_VOLUME_AUTHORITY_H
#define AIRPODS_VOLUME_AUTHORITY_H
#include "peer_address.h"
#include "../tinyusb/dongle_client.h"
#include "../tinyusb/dongle_protocol.h"

#define VOLUME_AUTHORITY_TTL_MS 5000u
#define VOLUME_AUTHORITY_SNAPSHOT_SIZE 24u

typedef struct {
    uint32_t usb_generation, control_generation;
    uint16_t cid;
    uint8_t peer[6];
    bool ready;
} volume_authority_scope_t;

typedef struct {
    volume_authority_scope_t scope;
    uint32_t serial, token, deadline;
    uint8_t client;
    bool active;
} volume_authority_t;

static inline bool volume_authority_scope_ready(const volume_authority_scope_t *scope) {
    return scope && scope->ready && (scope->usb_generation & 1u) &&
        scope->control_generation && scope->cid && peer_address_valid(scope->peer);
}

static inline bool volume_authority_current(const volume_authority_t *s,
                                            const volume_authority_scope_t *scope,
                                            uint32_t now) {
    return s->active && volume_authority_scope_ready(scope) &&
        s->scope.usb_generation==scope->usb_generation &&
        s->scope.control_generation==scope->control_generation &&
        s->scope.cid==scope->cid && !memcmp(s->scope.peer,scope->peer,6) &&
        (int32_t)(now-s->deadline)<0;
}

static inline bool volume_authority_owner(const volume_authority_t *s,uint8_t client,
                                          const volume_authority_scope_t *scope,
                                          uint32_t now) {
    return client==DONGLE_CLIENT_USB && s->client==client &&
        volume_authority_current(s,scope,now);
}

static inline uint32_t volume_authority_remaining(const volume_authority_t *s,
                                                 const volume_authority_scope_t *scope,
                                                 uint32_t now) {
    return volume_authority_current(s,scope,now) ? s->deadline-now : 0;
}

// The caller retires authority-origin writes when this reports a revoked scope.
static inline bool volume_authority_poll(volume_authority_t *s,
                                         const volume_authority_scope_t *scope,
                                         uint32_t now) {
    if (!s->active || volume_authority_current(s,scope,now)) return false;
    s->active=false; s->token=0;
    return true;
}

static inline bool volume_authority_disconnect(volume_authority_t *s,uint8_t client) {
    if (!s->active || s->client!=client) return false;
    s->active=false; s->token=0;
    return true;
}

// Poll revocation before managing a valid request; malformed requests change nothing.
static inline uint8_t volume_authority_manage(volume_authority_t *s,uint8_t client,
                                              const volume_authority_scope_t *scope,
                                              uint8_t action,uint32_t token,uint8_t level,
                                              uint32_t now) {
    if (action>3 || level>127 || (action!=3 && level) || (!action && token) ||
        (action && !token)) return DONGLE_MALFORMED;
    if (client!=DONGLE_CLIENT_USB) return DONGLE_UNSUPPORTED;
    if (!volume_authority_scope_ready(scope)) return DONGLE_DISCONNECTED;
    if (!action) {
        if (s->active) return DONGLE_BUSY;
        if (!++s->serial) ++s->serial;
        s->scope=*scope; s->client=client; s->token=s->serial;
        s->deadline=now+VOLUME_AUTHORITY_TTL_MS; s->active=true;
        return DONGLE_ACCEPTED;
    }
    if (!volume_authority_owner(s,client,scope,now) || token!=s->token) return DONGLE_STALE;
    if (action==2) { s->active=false; s->token=0; }
    else s->deadline=now+VOLUME_AUTHORITY_TTL_MS;
    return DONGLE_ACCEPTED;
}
#endif
