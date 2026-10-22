// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_DONGLE_PLAYBACK_PEER_H
#define AIRPODS_DONGLE_PLAYBACK_PEER_H

#include "peer_address.h"

static inline bool playback_peer_accept(const uint8_t selected[6], const uint8_t event_peer[6],
                                        uint16_t active_cid, uint16_t event_cid,
                                        bool enabled, bool busy) {
    return enabled && event_cid != 0 && selected != NULL &&
           peer_address_valid(selected) && memcmp(selected, event_peer, 6) == 0 &&
           (!busy || (active_cid != 0 && active_cid == event_cid));
}

#endif
