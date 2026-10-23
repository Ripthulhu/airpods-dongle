// SPDX-License-Identifier: GPL-3.0-only
#ifndef RADIO_CLOCK_H
#define RADIO_CLOCK_H

#include <stdint.h>

#define RADIO_CLOCK_SPI_MAX_HZ 31250000u

// The SDK's CYW43 PIO program takes two cycles per SPI bit. Return its
// divider in 1/256ths, rounding up so changing clk_sys cannot exceed the cap.
static inline uint32_t radio_clock_divider256(uint32_t system_hz) {
    const uint64_t pio_hz = 2u * RADIO_CLOCK_SPI_MAX_HZ;
    uint64_t divider = ((uint64_t)system_hz * 256u + pio_hz - 1u) / pio_hz;
    return divider < 256u ? 256u : (uint32_t)divider;
}

#endif
