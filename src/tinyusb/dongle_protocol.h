// SPDX-License-Identifier: GPL-3.0-only
#ifndef DONGLE_PROTOCOL_H
#define DONGLE_PROTOCOL_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define DONGLE_REPORT_ID 2
#define DONGLE_REPORT_SIZE 64
enum { DONGLE_ACCEPTED = 1, DONGLE_BUSY, DONGLE_STALE,
       DONGLE_UNSUPPORTED, DONGLE_DISCONNECTED, DONGLE_MALFORMED };
static inline uint32_t dongle_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void dongle_put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8));
}
static inline void dongle_header(uint8_t *p) {
    memset(p, 0, DONGLE_REPORT_SIZE);
    p[0] = DONGLE_REPORT_ID;
    memcpy(p + 1, "APDG", 4); p[5] = 1; p[6] = DONGLE_REPORT_SIZE;
}
static inline bool dongle_command_valid(const uint8_t *p, unsigned size) {
    if (size != DONGLE_REPORT_SIZE || p[0] != DONGLE_REPORT_ID ||
        memcmp(p + 1, "APDG", 4) || p[5] != 1 || p[6] != size ||
        !dongle_u32(p + 18)) return false;
    for (unsigned i = 23; i < size; ++i) if (p[i]) return false;
    if (p[7] == 3) return p[22] == 2 || p[22] == 3;
    return p[7] >= 1 && p[7] <= 4 && p[22] == 0;
}

// Each component retains its own age; omitted slots are not refreshed.
typedef struct {
    uint8_t anc, battery[3], charging;
    uint32_t updated[3];
    bool known[3];
} dongle_readback_t;
static inline void dongle_readback_clear(dongle_readback_t *s) {
    memset(s, 0, sizeof(*s));
    memset(s->battery, 255, sizeof(s->battery));
}
static inline void dongle_readback_packet(dongle_readback_t *s, const uint8_t *p,
                                         unsigned size, uint32_t now) {
    static const uint8_t header[] = {4, 0, 4, 0};
    if (size < 7 || memcmp(p, header, 4)) return;
    if (size == 11 && p[4] == 9 && p[5] == 0 && p[6] == 0x0d &&
        dongle_u32(p + 7) >= 1 && dongle_u32(p + 7) <= 4) s->anc = p[7];
    if (p[4] != 4 || p[5] || !p[6] || size < 7u + p[6] * 5u) return;
    // Validate the entire battery report before changing any slot.
    for (unsigned i = 0; i < p[6]; ++i) {
        uint8_t level = p[7 + i * 5 + 2];
        if (level > 100 && level != 255) return;
    }
    for (unsigned i = 0; i < p[6]; ++i) {
        const uint8_t *b = p + 7 + i * 5;
        int slot = b[0] == 4 ? 0 : b[0] == 2 ? 1 : b[0] == 8 ? 2 : -1;
        if (slot < 0) continue;
        s->battery[slot] = b[3] == 4 ? 255 : b[2];
        s->charging &= (uint8_t)~(1u << slot);
        if (s->battery[slot] <= 100 && (b[3] == 1 || b[3] == 5)) s->charging |= 1u << slot;
        s->updated[slot] = now;
        s->known[slot] = true;
    }
}
#endif
