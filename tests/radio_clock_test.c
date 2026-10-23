// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/radio_clock.h"

static void check_clock(uint32_t hz) {
    uint32_t divider = radio_clock_divider256(hz);
    // Check the resulting SPI frequency without floating-point rounding.
    uint64_t requested = (uint64_t)hz * 256u;
    uint64_t budget = (uint64_t)2u * RADIO_CLOCK_SPI_MAX_HZ;
    assert(divider >= 256u && (divider >> 8) <= UINT16_MAX);
    assert(requested <= budget * divider);
    // Choose the fastest representable safe divider, except the PIO minimum.
    assert(divider == 256u || requested > budget * (divider - 1u));
}

int main(void) {
    assert(radio_clock_divider256(250000000u) == 4u * 256u);
    assert(radio_clock_divider256(150000000u) == 2u * 256u + 103u);
    assert(radio_clock_divider256(125000000u) == 2u * 256u);
    assert(radio_clock_divider256(62500000u) == 256u);
    assert(radio_clock_divider256(62500001u) == 257u);
    assert(radio_clock_divider256(0) == 256u);
    assert(radio_clock_divider256(1) == 256u);

    // Cover low clocks, fractional boundaries, supported clocks and arithmetic
    // limits; the helper must also remain safe if startup changes in the future.
    const uint32_t clocks[] = {0, 1, 60000000u, 62499999u, 62500000u,
        62500001u, 125000000u, 125000001u, 133000000u, 150000000u,
        200000000u, 250000000u, 250000001u, 300000000u, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i)
        check_clock(clocks[i]);
    for (uint32_t hz = 1000000u; hz <= 500000000u; hz += 999983u)
        check_clock(hz);

    puts("radio clock: fixed clocks, fractional rounding, minimum and overflow bounds passed");
    return 0;
}
