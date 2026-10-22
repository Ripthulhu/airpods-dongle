// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_LHDC48_H
#define AIRPODS_LHDC48_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef void (*lhdc48_pcm_callback)(const int16_t *stereo, uint16_t frames, void *context);
// One decoder. Prepare/close on core 0 only after the worker has detached.
bool lhdc48_prepare(uint8_t bit_depth);
bool lhdc48_ready(void);
void lhdc48_close(void);
// Reset/decode on the owning worker; input is raw AUs, including each LE frame header.
void lhdc48_reset(void);
bool lhdc48_decode(const uint8_t *input, size_t length, uint8_t frames,
                   lhdc48_pcm_callback callback, void *context);
#endif
