#ifndef AIRPODS_PEER_ADDRESS_H
#define AIRPODS_PEER_ADDRESS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool peer_address_valid(const uint8_t address[6]) {
    bool zero = true, erased = true;
    for (unsigned i = 0; i < 6; ++i) {
        zero &= address[i] == 0;
        erased &= address[i] == 0xff;
    }
    return !zero && !erased;
}

static inline bool peer_select(const uint8_t addresses[2][6], const bool bonded[2],
                               uint8_t slot, uint8_t selected[6]) {
    memset(selected, 0, 6);
    if (slot < 1 || slot > 2 || !bonded[slot - 1] ||
        !peer_address_valid(addresses[slot - 1])) return false;
    memcpy(selected, addresses[slot - 1], 6);
    return true;
}

#endif
