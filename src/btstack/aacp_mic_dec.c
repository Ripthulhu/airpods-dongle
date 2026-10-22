// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
// Stream parameters derive from librepods (GPL-3.0).
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <stdatomic.h>
#include <aacdecoder_lib.h>
#include "pico/time.h"
#include "aacp_mic_dec.h"
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico/platform.h"
#define MIC_SCRATCH_Y(name) __scratch_y(name)
#else
#define MIC_SCRATCH_Y(name)
#endif

#define MIC_AU_MAX 255u
#define MIC_INPUT_FRAMES 8u
#define MIC_OUTPUT_FRAMES 16u
#define MIC_FRAME_SAMPLES 480u

typedef struct {
    uint32_t generation;
    uint16_t length;
    uint8_t data[MIC_AU_MAX];
} mic_input_t;

typedef struct {
    uint32_t generation;
    int16_t samples[MIC_FRAME_SAMPLES];
} mic_output_t;

_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_POINTER_LOCK_FREE == 2,
               "Codec queues require lock-free cursor and handle publication");
_Static_assert(sizeof(INT_PCM) == sizeof(int16_t), "Microphone PCM must be 16-bit");
_Static_assert(sizeof(mic_input_t) * MIC_INPUT_FRAMES +
               sizeof(INT_PCM) * MIC_FRAME_SAMPLES <= 4096u,
               "Core1 codec staging must fit scratch-Y");
static mic_input_t MIC_SCRATCH_Y("mic_input") input[MIC_INPUT_FRAMES];
static mic_output_t output[MIC_OUTPUT_FRAMES];
static _Atomic uint32_t input_written, input_read;
static _Atomic uint32_t output_written, output_read;
static uint32_t output_offset; // USB consumer only
static _Atomic uint32_t generation = 1u;
static _Atomic uint32_t reset_ack;
static _Atomic uint32_t completed;
static _Atomic uint32_t queue_overruns;
static _Atomic bool reset_stats_requested, report_requested, report_ready;
static _Atomic bool decoder_failed;
static _Atomic(HANDLE_AACDECODER) mic_dec;
static _Atomic bool release_requested;
static _Atomic(HANDLE_AACDECODER) released_dec;
static bool failure_reported; // Core0 only.

// Everything below is owned by core1 after the configured handle is published.
static uint32_t worker_generation;
static UINT decode_flags = AACDEC_CLRHIST;
static INT_PCM MIC_SCRATCH_Y("mic_decode") decoded_pcm[MIC_FRAME_SAMPLES];
typedef struct {
    uint32_t ok, errors, errors_total;
    uint32_t us_sum, us_max;
    uint64_t sq_sum;
    uint32_t samples;
    int32_t peak;
    uint32_t overruns;
} mic_stats_t;
static mic_stats_t stats, report;

extern char __StackLimit[];
extern char end[];

static void report_heap(const char *tag) {
    struct mallinfo mi = mallinfo();
    printf("[DEC] heap %s: region=%u arena=%u inuse=%u free=%u\n", tag,
           (unsigned)((uintptr_t)__StackLimit - (uintptr_t)end),
           (unsigned)mi.arena, (unsigned)mi.uordblks, (unsigned)mi.fordblks);
}

bool aacp_mic_dec_init(void) {
    if (atomic_load_explicit(&release_requested, memory_order_acquire)) return false;
    if (atomic_load_explicit(&mic_dec, memory_order_acquire) != NULL)
        return !atomic_load_explicit(&decoder_failed, memory_order_acquire);
    // Keep large FDK allocations on core0, outside Bluetooth callbacks.
    report_heap("before");
    HANDLE_AACDECODER decoder = aacDecoder_Open(TT_MP4_RAW, 1);
    if (decoder == NULL) {
        printf("[DEC] allocation failed\n");
        return false;
    }
    UCHAR asc[] = {0xf8, 0xe6, 0x30, 0x00};
    UCHAR *config[] = {asc};
    UINT length = sizeof(asc);
    if (aacDecoder_ConfigRaw(decoder, config, &length) != AAC_DEC_OK) {
        aacDecoder_Close(decoder);
        printf("[DEC] configuration failed\n");
        return false;
    }
    failure_reported = false;
    atomic_store_explicit(&decoder_failed, false, memory_order_release);
    aacp_mic_pcm_reset();
    atomic_store_explicit(&mic_dec, decoder, memory_order_release);
    report_heap("after");
    printf("[DEC] configured: AAC-ELD mono/480, core1 worker\n");
    aacp_mic_dec_wake_worker();
    return true;
}

bool aacp_mic_dec_ready(void) {
    return !atomic_load_explicit(&release_requested, memory_order_acquire) &&
           atomic_load_explicit(&mic_dec, memory_order_acquire) != NULL &&
           !atomic_load_explicit(&decoder_failed, memory_order_acquire);
}

bool aacp_mic_dec_release(void) {
    if (!atomic_load_explicit(&release_requested, memory_order_acquire)) {
        if (atomic_load_explicit(&mic_dec, memory_order_acquire) == NULL) return true;
        aacp_mic_pcm_reset();
        atomic_store_explicit(&release_requested, true, memory_order_release);
        aacp_mic_dec_wake_worker();
    }
    HANDLE_AACDECODER decoder = atomic_exchange_explicit(&released_dec, NULL, memory_order_acq_rel);
    if (decoder == NULL) return false;
    // Core1 cannot load this detached handle while release_requested remains set.
    aacDecoder_Close(decoder);
    atomic_store_explicit(&report_ready, false, memory_order_release);
    atomic_store_explicit(&release_requested, false, memory_order_release);
    return true;
}

void aacp_mic_pcm_reset(void) {
    atomic_fetch_add_explicit(&generation, 1u, memory_order_acq_rel);
    aacp_mic_dec_wake_worker();
}

bool aacp_mic_dec_reset(void) {
    aacp_mic_pcm_reset();
    return aacp_mic_dec_ready();
}

bool aacp_mic_dec_reset_done(void) {
    uint32_t current = atomic_load_explicit(&generation, memory_order_acquire);
    return aacp_mic_dec_ready() &&
           atomic_load_explicit(&reset_ack, memory_order_acquire) == current &&
           atomic_load_explicit(&generation, memory_order_acquire) == current;
}

bool aacp_mic_dec_submit(const uint8_t *au, uint16_t length) {
    if (!aacp_mic_dec_ready()) return false;
    if (au == NULL || length == 0 || length > MIC_AU_MAX) {
        aacp_mic_pcm_reset();
        return false;
    }
    uint32_t w = atomic_load_explicit(&input_written, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&input_read, memory_order_acquire);
    if (w - r == MIC_INPUT_FRAMES) {
        atomic_fetch_add_explicit(&queue_overruns, 1u, memory_order_relaxed);
        aacp_mic_pcm_reset(); // Drop the generation rather than build stale latency.
        return false;
    }
    mic_input_t *job = &input[w % MIC_INPUT_FRAMES];
    job->generation = atomic_load_explicit(&generation, memory_order_acquire);
    job->length = length;
    memcpy(job->data, au, length);
    atomic_store_explicit(&input_written, w + 1u, memory_order_release);
    aacp_mic_dec_wake_worker();
    return true;
}

static bool decode_job(HANDLE_AACDECODER decoder, const mic_input_t *job) {
    UCHAR *bytes[] = {(UCHAR *)job->data};
    UINT size = job->length, valid = size;
    if (aacDecoder_Fill(decoder, bytes, &size, &valid) != AAC_DEC_OK || valid != 0)
        return false;
    uint32_t start = time_us_32();
    AAC_DECODER_ERROR error = aacDecoder_DecodeFrame(decoder, decoded_pcm,
                                                    MIC_FRAME_SAMPLES, decode_flags);
    uint32_t elapsed = time_us_32() - start;
    if (error != AAC_DEC_OK) return false;
    CStreamInfo *info = aacDecoder_GetStreamInfo(decoder);
    if (info == NULL || info->numChannels != 1 || info->frameSize != 480 ||
        info->sampleRate != 48000 || info->aot != 39) return false;
    decode_flags = 0;

    // A reset can arrive during decode or publication. Each output frame carries
    // its generation too, so the USB reader rejects even that final race.
    if (job->generation != atomic_load_explicit(&generation, memory_order_acquire))
        return true;
    uint32_t w = atomic_load_explicit(&output_written, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&output_read, memory_order_acquire);
    if (w - r == MIC_OUTPUT_FRAMES) return false;
    mic_output_t *frame = &output[w % MIC_OUTPUT_FRAMES];
    frame->generation = job->generation;
    memcpy(frame->samples, decoded_pcm, sizeof(frame->samples));
    atomic_store_explicit(&output_written, w + 1u, memory_order_release);
    atomic_fetch_add_explicit(&completed, 1u, memory_order_release);
    stats.ok++;
    stats.us_sum += elapsed;
    if (elapsed > stats.us_max) stats.us_max = elapsed;
    for (unsigned i = 0; i < MIC_FRAME_SAMPLES; ++i) {
        int32_t sample = decoded_pcm[i];
        stats.sq_sum += (uint64_t)((int64_t)sample * sample);
        int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > stats.peak) stats.peak = magnitude;
    }
    stats.samples += MIC_FRAME_SAMPLES;
    return true;
}

bool aacp_mic_dec_worker_step(void) {
    if (atomic_load_explicit(&release_requested, memory_order_acquire)) {
        HANDLE_AACDECODER detached = atomic_exchange_explicit(&mic_dec, NULL, memory_order_acq_rel);
        if (detached == NULL) return false;
        // Submission is gated; only the worker advances its compressed-input cursor.
        atomic_store_explicit(&input_read,
            atomic_load_explicit(&input_written, memory_order_acquire), memory_order_release);
        worker_generation = atomic_load_explicit(&generation, memory_order_acquire) - 1u;
        atomic_store_explicit(&reset_ack, worker_generation, memory_order_release);
        atomic_store_explicit(&reset_stats_requested, false, memory_order_release);
        atomic_store_explicit(&report_requested, false, memory_order_release);
        memset(&stats, 0, sizeof(stats));
        decode_flags = AACDEC_CLRHIST;
        atomic_store_explicit(&released_dec, detached, memory_order_release);
        return true;
    }
    HANDLE_AACDECODER decoder = atomic_load_explicit(&mic_dec, memory_order_acquire);
    if (decoder == NULL || atomic_load_explicit(&decoder_failed, memory_order_acquire))
        return false;
    bool worked = false;
    if (atomic_exchange_explicit(&reset_stats_requested, false, memory_order_acq_rel)) {
        memset(&stats, 0, sizeof(stats));
        worked = true;
    }
    uint32_t current = atomic_load_explicit(&generation, memory_order_acquire);
    if (current != worker_generation) {
        decode_flags = AACDEC_CLRHIST;
        if (aacDecoder_SetParam(decoder, AAC_TPDEC_CLEAR_BUFFER, 1) != AAC_DEC_OK) {
            atomic_store_explicit(&decoder_failed, true, memory_order_release);
            return true;
        }
        worker_generation = current;
        atomic_store_explicit(&reset_ack, current, memory_order_release);
        worked = true;
    }
    uint32_t r = atomic_load_explicit(&input_read, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&input_written, memory_order_acquire);
    if (r != w) {
        const mic_input_t *job = &input[r % MIC_INPUT_FRAMES];
        if (job->generation == worker_generation &&
            job->generation == atomic_load_explicit(&generation, memory_order_acquire) &&
            !decode_job(decoder, job)) {
            stats.errors++;
            stats.errors_total++;
            aacp_mic_pcm_reset();
        }
        atomic_store_explicit(&input_read, r + 1u, memory_order_release);
        worked = true;
    }
    if (!atomic_load_explicit(&report_ready, memory_order_acquire) &&
        atomic_exchange_explicit(&report_requested, false, memory_order_acq_rel)) {
        stats.overruns = atomic_load_explicit(&queue_overruns, memory_order_relaxed);
        report = stats;
        atomic_store_explicit(&report_ready, true, memory_order_release);
        stats.ok = stats.errors = stats.us_sum = stats.us_max = 0;
        stats.sq_sum = stats.samples = 0;
        stats.peak = 0;
        worked = true;
    }
    return worked;
}

uint32_t aacp_mic_dec_completed(void) {
    return atomic_load_explicit(&completed, memory_order_acquire);
}

void aacp_mic_dec_reset_stats(void) {
    atomic_store_explicit(&reset_stats_requested, true, memory_order_release);
    aacp_mic_dec_wake_worker();
}

void aacp_mic_dec_report(void) {
    atomic_store_explicit(&report_requested, true, memory_order_release);
    aacp_mic_dec_wake_worker();
}

void aacp_mic_dec_poll(void) {
    if (!failure_reported && atomic_load_explicit(&decoder_failed, memory_order_acquire)) {
        failure_reported = true;
        printf("[DEC] worker reset failed; microphone decoder must be reinitialized\n");
    }
    if (!atomic_load_explicit(&report_ready, memory_order_acquire)) return;
    mic_stats_t snapshot = report;
    atomic_store_explicit(&report_ready, false, memory_order_release);
    uint32_t rms = snapshot.samples ?
        (uint32_t)sqrtf((float)(snapshot.sq_sum / snapshot.samples)) : 0;
    printf("[DEC] ok/s=%lu err=%lu(total %lu) dec_us avg/max=%lu/%lu rms=%lu peak=%ld queue_overrun=%lu\n",
           (unsigned long)snapshot.ok, (unsigned long)snapshot.errors,
           (unsigned long)snapshot.errors_total,
           (unsigned long)(snapshot.ok ? snapshot.us_sum / snapshot.ok : 0),
           (unsigned long)snapshot.us_max, (unsigned long)rms, (long)snapshot.peak,
           (unsigned long)snapshot.overruns);
    aacp_mic_dec_wake_worker();
}

uint32_t aacp_mic_pcm_read(int16_t *dst, uint32_t count) {
    if (dst == NULL || count == 0) return 0;
    uint32_t current = atomic_load_explicit(&generation, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&output_read, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&output_written, memory_order_acquire);
    uint32_t copied = 0;
    while (r != w && copied < count) {
        const mic_output_t *frame = &output[r % MIC_OUTPUT_FRAMES];
        if (frame->generation != current) {
            output_offset = 0;
            ++r;
            continue;
        }
        uint32_t take = MIC_FRAME_SAMPLES - output_offset;
        if (take > count - copied) take = count - copied;
        memcpy(dst + copied, frame->samples + output_offset, take * sizeof(*dst));
        copied += take;
        output_offset += take;
        if (output_offset == MIC_FRAME_SAMPLES) {
            output_offset = 0;
            ++r;
        }
    }
    atomic_store_explicit(&output_read, r, memory_order_release);
    if (atomic_load_explicit(&generation, memory_order_acquire) != current) return 0;
    return copied;
}
