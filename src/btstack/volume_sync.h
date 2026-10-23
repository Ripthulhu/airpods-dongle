// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_VOLUME_SYNC_H
#define AIRPODS_VOLUME_SYNC_H
#include "peer_address.h"

#define BT_VOLUME_RETRY_MS 50u
#define BT_VOLUME_ACK_MS 1000u
#define BT_VOLUME_REQUEST_MS 5000u
#define BT_VOLUME_ATTEMPTS 3u

typedef struct {
    uint32_t requested_at, sent_at, attempted_at;
    uint8_t peer[6], desired, confirmed, sent, label, attempts;
    uint16_t cid;
    bool selected, known, pending, inflight, phone_request, attempted, waiting_for_connection;
} volume_sync_t;

static inline bool volume_sync_get(const volume_sync_t *s, uint8_t *level) {
    if (!level || !s->selected || !s->known || s->pending || s->inflight) return false;
    *level = s->confirmed;
    return true;
}

static inline bool volume_sync_select(volume_sync_t *s, const uint8_t *peer) {
    bool valid = peer && peer_address_valid(peer);
    if (valid == s->selected && (!valid || !memcmp(s->peer, peer, 6))) return false;
    memset(s, 0, sizeof(*s));
    s->selected = valid;
    if (valid) memcpy(s->peer, peer, 6);
    return true;
}

static inline void volume_sync_cancel_phone(volume_sync_t *s) {
    if (s->pending && s->phone_request) s->pending = false;
    // A command already accepted by AVRCP still needs its acknowledgement or timeout.
}

static inline void volume_sync_connection(volume_sync_t *s, uint16_t cid, uint32_t now) {
    s->cid = cid;
    s->known = s->inflight = s->attempted = false;
    s->attempts = 0;
    volume_sync_cancel_phone(s);
    s->requested_at = now;
    s->waiting_for_connection = cid == 0;
}

static inline bool volume_sync_request(volume_sync_t *s, uint8_t level, bool phone, uint32_t now) {
    if (!s->selected || level > 127) return false;
    s->desired = level;
    s->phone_request = phone;
    s->pending = s->inflight || !s->known || s->confirmed != level;
    s->requested_at = now;
    s->waiting_for_connection = s->cid == 0;
    s->attempts = 0;
    if (!s->inflight) s->attempted = false;
    return true;
}

static inline void volume_sync_step(volume_sync_t *s, bool increase, uint32_t now) {
    uint8_t level;
    if (s->pending) level = s->desired;
    else if (!volume_sync_get(s, &level)) return;
    level = increase ? (level > 117 ? 127 : level + 10) : (level < 10 ? 0 : level - 10);
    (void)volume_sync_request(s, level, false, now);
}

static inline bool volume_sync_due(volume_sync_t *s, uint32_t now, uint8_t *level) {
    if (s->inflight && (uint32_t)(now - s->sent_at) >= BT_VOLUME_ACK_MS) {
        s->known = s->inflight = false;
        s->attempted_at = now;
        if (s->attempts >= BT_VOLUME_ATTEMPTS) s->pending = false;
    }
    if (!s->pending || !s->cid) return false;
    if (s->waiting_for_connection) {
        s->waiting_for_connection = false;
        s->requested_at = now;
    }
    if ((uint32_t)(now - s->requested_at) >= BT_VOLUME_REQUEST_MS) {
        s->pending = false;
        return false;
    }
    if (s->inflight || (s->attempted && (uint32_t)(now - s->attempted_at) < BT_VOLUME_RETRY_MS)) return false;
    *level = s->desired;
    return true;
}

static inline void volume_sync_send(volume_sync_t *s, uint32_t now, bool accepted, uint8_t label) {
    s->attempted = true;
    s->attempted_at = now;
    if (!accepted) return;
    s->inflight = true;
    s->sent = s->desired;
    s->sent_at = now;
    s->label = label;
    ++s->attempts;
}

static inline bool volume_sync_response(volume_sync_t *s, uint16_t cid, uint8_t label,
                                         bool accepted, uint8_t level) {
    if (!cid || cid != s->cid || !s->inflight || label != s->label || (accepted && level > 127)) return false;
    s->inflight = false;
    if (accepted) { s->confirmed = level; s->known = true; }
    // A clamped/quantized ACK satisfies its request; it must not erase a newer level.
    if (s->pending && (s->desired == s->sent || (accepted && s->desired == level))) s->pending = false;
    s->attempts = 0;
    return true;
}

static inline bool volume_sync_notification(volume_sync_t *s, uint16_t cid, uint8_t level) {
    if (!cid || cid != s->cid || level > 127) return false;
    s->confirmed = level;
    s->known = true;
    if (s->pending && !s->inflight && s->desired == level) s->pending = false;
    return true;
}
#endif
