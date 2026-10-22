// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
#ifndef AACP_MIC_DEC_H
#define AACP_MIC_DEC_H

#include <stdint.h>
#include <stdbool.h>

// Allocate/configure in core0 thread context with the Bluetooth lock held.
bool aacp_mic_dec_init(void);
bool aacp_mic_dec_ready(void);

// Core0 thread only: request worker detachment, then free after its acknowledgement.
// Returns false while work is in flight; keep polling before initializing again.
bool aacp_mic_dec_release(void);

// Core0 producer: copies a complete AU; success means queued, not decoded.
bool aacp_mic_dec_submit(const uint8_t *au, uint16_t len);

// Invalidate pending input/output immediately; core1 acknowledges codec reset.
bool aacp_mic_dec_reset(void);
bool aacp_mic_dec_reset_done(void);
void aacp_mic_dec_reset_stats(void);

// Core1 only: bounded work, no allocation, logging, Bluetooth or USB calls.
bool aacp_mic_dec_worker_step(void);

// Successful frame counter is monotonic across resets; enqueue is not progress.
uint32_t aacp_mic_dec_completed(void);

// Core0 requests/retrieves a coherent statistics report without blocking core1.
void aacp_mic_dec_report(void);
void aacp_mic_dec_poll(void);

// Platform hook: publish a wake event after submitting work or control requests.
void aacp_mic_dec_wake_worker(void);

// USB consumer only; stale-generation frames are discarded before delivery.
uint32_t aacp_mic_pcm_read(int16_t *dst, uint32_t n);
void aacp_mic_pcm_reset(void);

#endif
