// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PHONE_WORKER_H
#define AIRPODS_PHONE_WORKER_H

#include "phone_codec.h"

typedef enum {
    PHONE_WORKER_WAITING, PHONE_WORKER_READY, PHONE_WORKER_FAILED
} phone_worker_status_t;

// All controller calls run on core0 under the Bluetooth lock. Configuration and
// reset are callback-safe; prepare/close allocate/free only in the main loop.
bool phone_worker_configure(phone_codec_t codec, const phone_sbc_config_t *sbc);
phone_worker_status_t phone_worker_prepare(void);
bool phone_worker_ready(void);
void phone_worker_reset(void);
bool phone_worker_submit(const uint8_t *payload, size_t length, uint8_t sbc_frames);
// Publishes a complete decoded packet on core0; false means decode failed.
bool phone_worker_poll(phone_codec_pcm_callback callback, void *context);
// Actual core-1 decode time; zero on host harnesses without the Pico clock.
uint32_t phone_worker_last_decode_us(void);
// Maximum since this worker was prepared, including failed/cancelled decodes.
uint32_t phone_worker_max_decode_us(void);
// Returns true only after core1 has detached and all phone memory is released.
bool phone_worker_close(void);
bool phone_worker_released(void);

// Core1 performs codec work only, and never allocates or calls Bluetooth/USB.
bool phone_worker_step(void);
void phone_worker_wake(void);

#endif
