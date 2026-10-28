// SPDX-License-Identifier: GPL-3.0-only
#include "phone_worker.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifndef PHONE_WORKER_TIME_US
#ifdef PICO_ON_DEVICE
#include "pico/time.h"
#define PHONE_WORKER_TIME_US() time_us_32()
#else
#define PHONE_WORKER_TIME_US() 0u
#endif
#endif

// Measured HCI bursts with 7–14 ms decodes need more slots, not a playout delay.
#define INPUT_SLOTS 8u
// A maximum SBC packet contains 15 * 128 stereo frames. aptX needs less.
#define OUTPUT_FRAMES 1920u

typedef struct {
    uint32_t generation;
    uint16_t length;
    uint8_t sbc_frames;
    bool reset_before;
    uint8_t payload[PHONE_CODEC_MAX_PAYLOAD];
} input_slot_t;

typedef struct {
    atomic_uint generation, input_read, input_written, last_decode_us, max_decode_us;
    atomic_bool output_ready;
    input_slot_t input[INPUT_SLOTS];
    int16_t output[OUTPUT_FRAMES * 2u];
    uint32_t output_generation, worker_generation;
    uint16_t output_frames;
    bool output_ok;
    bool input_gap_pending; // Core0 only; published to core1 in the next accepted slot.
} worker_t;

// The linked memory probe reads the actual ARM layout from this image.
const uint32_t airpods_phone_worker_storage_bytes = sizeof(worker_t);

// Phone storage is allocated only after the mutually exclusive microphone
// decoder is released. Keeping this out of .bss preserves USB mic headroom.
static worker_t *owned;
static worker_t *active;
enum { WORKER_IDLE, WORKER_RUNNING, WORKER_DETACH };
static atomic_uint worker_state;
static phone_codec_t requested_codec;
static phone_sbc_config_t requested_sbc;
static bool reconfigure;

static void request_detach(void) {
    if (owned == NULL) return;
    unsigned expected = WORKER_RUNNING;
    atomic_compare_exchange_strong_explicit(&worker_state, &expected, WORKER_DETACH,
                                            memory_order_acq_rel, memory_order_acquire);
    phone_worker_wake();
}

// Main loop only. Core1 acknowledges detachment after its last codec access.
static bool release(void) {
    if (owned == NULL) return true;
    request_detach();
    if (atomic_load_explicit(&worker_state, memory_order_acquire) != WORKER_IDLE) return false;
    phone_codec_close();
    free(owned);
    owned = NULL;
    return true;
}

bool phone_worker_configure(phone_codec_t codec, const phone_sbc_config_t *sbc) {
    if (!phone_codec_configuration_valid(codec, sbc)) return false;
    requested_codec = codec;
    if (codec == PHONE_CODEC_SBC) requested_sbc = *sbc;
    reconfigure = true;
    request_detach();
    return true;
}

phone_worker_status_t phone_worker_prepare(void) {
    if (reconfigure && !release()) return PHONE_WORKER_WAITING;
    if (requested_codec == PHONE_CODEC_NONE) return PHONE_WORKER_FAILED;
    if (owned != NULL) return PHONE_WORKER_READY;
    worker_t *next = calloc(1, sizeof(*next));
    if (next == NULL) return PHONE_WORKER_FAILED;
    if (!phone_codec_configure(requested_codec, &requested_sbc) || !phone_codec_prepare()) {
        phone_codec_close();
        free(next);
        return PHONE_WORKER_FAILED;
    }
    atomic_init(&next->generation, 1u);
    atomic_init(&next->input_read, 0u);
    atomic_init(&next->input_written, 0u);
    atomic_init(&next->output_ready, false);
    atomic_init(&next->last_decode_us, 0u);
    atomic_init(&next->max_decode_us, 0u);
    owned = next;
    reconfigure = false;
    active = next;
    atomic_store_explicit(&worker_state, WORKER_RUNNING, memory_order_release);
    phone_worker_wake();
    return PHONE_WORKER_READY;
}

bool phone_worker_ready(void) {
    return owned != NULL && !reconfigure &&
           atomic_load_explicit(&worker_state, memory_order_acquire) == WORKER_RUNNING;
}

void phone_worker_reset(void) {
    if (!phone_worker_ready()) return;
    owned->input_gap_pending = false;
    atomic_fetch_add_explicit(&owned->generation, 1u, memory_order_acq_rel);
    phone_worker_wake();
}

bool phone_worker_submit(const uint8_t *payload, size_t length, uint8_t sbc_frames) {
    if (!phone_worker_ready() || payload == NULL || length == 0 || length > PHONE_CODEC_MAX_PAYLOAD)
        return false;
    unsigned written = atomic_load_explicit(&owned->input_written, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&owned->input_read, memory_order_acquire);
    if (written - read >= INPUT_SLOTS) {
        owned->input_gap_pending = true;
        return false;
    }
    input_slot_t *slot = &owned->input[written % INPUT_SLOTS];
    slot->generation = atomic_load_explicit(&owned->generation, memory_order_acquire);
    slot->length = (uint16_t)length;
    slot->sbc_frames = sbc_frames;
    slot->reset_before = owned->input_gap_pending;
    owned->input_gap_pending = false;
    memcpy(slot->payload, payload, length);
    atomic_store_explicit(&owned->input_written, written + 1u, memory_order_release);
    phone_worker_wake();
    return true;
}

bool phone_worker_poll(phone_codec_pcm_callback callback, void *context) {
    if (!phone_worker_ready() || !atomic_load_explicit(&owned->output_ready, memory_order_acquire))
        return true;
    bool ok = true;
    if (owned->output_generation == atomic_load_explicit(&owned->generation, memory_order_acquire)) {
        ok = owned->output_ok;
        if (ok && owned->output_frames != 0 && callback != NULL)
            callback(owned->output, owned->output_frames, context);
    }
    atomic_store_explicit(&owned->output_ready, false, memory_order_release);
    phone_worker_wake();
    return ok;
}

uint32_t phone_worker_last_decode_us(void) {
    return phone_worker_ready() ? atomic_load_explicit(&owned->last_decode_us, memory_order_acquire) : 0u;
}

uint32_t phone_worker_max_decode_us(void) {
    return phone_worker_ready() ? atomic_load_explicit(&owned->max_decode_us, memory_order_acquire) : 0u;
}

phone_worker_queue_snapshot_t phone_worker_queue_snapshot(void) {
    phone_worker_queue_snapshot_t result = {0};
    if (!phone_worker_ready()) return result;
    // Core0 owns input_written; core1 can only advance input_read toward it.
    unsigned written = atomic_load_explicit(&owned->input_written, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&owned->input_read, memory_order_acquire);
    result.queued_packets = written - read;
    result.output_ready = atomic_load_explicit(&owned->output_ready, memory_order_acquire);
    return result;
}

bool phone_worker_close(void) {
    requested_codec = PHONE_CODEC_NONE;
    reconfigure = false;
    return release();
}

bool phone_worker_released(void) { return owned == NULL; }

static void collect_pcm(const int16_t *pcm, uint16_t frames, void *context) {
    worker_t *worker = context;
    if (pcm == NULL || frames > OUTPUT_FRAMES - worker->output_frames) {
        worker->output_ok = false;
        return;
    }
    memcpy(worker->output + worker->output_frames * 2u, pcm, frames * 4u);
    worker->output_frames += frames;
}

bool phone_worker_step(void) {
    unsigned state = atomic_load_explicit(&worker_state, memory_order_acquire);
    if (state == WORKER_DETACH) {
        active = NULL;
        atomic_store_explicit(&worker_state, WORKER_IDLE, memory_order_release);
        return true;
    }
    if (state != WORKER_RUNNING) return false;
    worker_t *worker = active;
    if (worker == NULL || atomic_load_explicit(&worker->output_ready, memory_order_acquire)) return false;
    unsigned read = atomic_load_explicit(&worker->input_read, memory_order_relaxed);
    if (read == atomic_load_explicit(&worker->input_written, memory_order_acquire)) return false;
    input_slot_t *slot = &worker->input[read % INPUT_SLOTS];
    unsigned generation = atomic_load_explicit(&worker->generation, memory_order_acquire);
    if (worker->worker_generation != generation) {
        phone_codec_reset();
        worker->worker_generation = generation;
    }
    if (slot->generation == generation) {
        // Keep valid queued audio; reset only where a rejected input left a gap.
        if (slot->reset_before) phone_codec_reset();
        worker->output_frames = 0;
        worker->output_ok = true;
        worker->output_generation = generation;
        uint32_t started = PHONE_WORKER_TIME_US();
        bool ok = phone_codec_decode(slot->payload, slot->length, slot->sbc_frames, collect_pcm, worker);
        uint32_t elapsed = PHONE_WORKER_TIME_US() - started;
        atomic_store_explicit(&worker->last_decode_us, elapsed, memory_order_release);
        if (elapsed > atomic_load_explicit(&worker->max_decode_us, memory_order_relaxed))
            atomic_store_explicit(&worker->max_decode_us, elapsed, memory_order_release);
        worker->output_ok = ok && worker->output_ok;
        // Never expose a partial decode or results cancelled during decoding.
        if (generation == atomic_load_explicit(&worker->generation, memory_order_acquire))
            atomic_store_explicit(&worker->output_ready, true, memory_order_release);
    }
    atomic_store_explicit(&worker->input_read, read + 1u, memory_order_release);
    return true;
}
