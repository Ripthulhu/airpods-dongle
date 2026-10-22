#include <assert.h>
#include "../src/btstack/peer_address.h"

int main(void) {
    const uint8_t peers[2][6] = {{1,2,3,4,5,6}, {7,8,9,10,11,12}};
    uint8_t selected[6];
    bool bonded[2] = {true, true};
    assert(peer_select(peers, bonded, 1, selected));
    assert(memcmp(selected, peers[0], 6) == 0);
    assert(peer_select(peers, bonded, 2, selected));
    assert(memcmp(selected, peers[1], 6) == 0);
    bonded[0] = false;
    assert(!peer_select(peers, bonded, 1, selected));
    assert(!peer_address_valid(selected));
    assert(!peer_select(peers, bonded, 0, selected));
    assert(!peer_select(peers, bonded, 255, selected));
    uint8_t erased[2][6] = {{0}, {255,255,255,255,255,255}};
    assert(!peer_select(erased, bonded, 2, selected));
    bonded[0] = true;
    assert(!peer_select(erased, bonded, 1, selected));
    // An address starting with FF is not an erased flash sector.
    uint8_t real[6] = {255,1,2,3,4,5};
    assert(peer_address_valid(real));
    return 0;
}
