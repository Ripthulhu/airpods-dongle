// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
// The stream parameters (AudioSpecificConfig, frame layout) come from
// librepods (https://github.com/librepods-org/librepods), GPL-3.0.
//
// AAC-ELD decoder for the AirPods hi-res mic stream — see header.
//
// Runs in the BTstack run loop (called per AU from the AACP L2CAP handler).
// One mono 480-sample ELD frame decodes in ~1.4ms at 250MHz (measured), well
// inside the 7.5ms frame budget; the decode-time stats below keep watch.
//
// LOGGING RULE: nothing is printed per AU. One "[DEC]" line per second.
//

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <malloc.h>

#include <aacdecoder_lib.h>
#include "pico/time.h"

#include "aacp_mic_dec.h"
#include "mic_pcm_ring.h"

// Fixed AudioSpecificConfig for the mic stream (from librepods):
// AAC-ELD (AOT 39), mono, 480 samples/frame, signalled 48 kHz.
// The inherited USB presentation clock remains 64 kHz pending calibrated timing.
static UCHAR aacp_mic_asc[] = { 0xF8, 0xE6, 0x30, 0x00 };

// librepods ELD_INBUF_MAX — AUs above this are malformed, don't feed them.
#define MIC_AU_MAX 255

// FDK must reject larger output; only mono 480 is accepted below.
#define MIC_PCM_MAX_SAMPLES 480
static INT_PCM mic_pcm[MIC_PCM_MAX_SAMPLES];

static _Atomic(HANDLE_AACDECODER) mic_dec;
static bool mic_dec_streaminfo_logged = false;
static UINT mic_dec_flags = AACDEC_CLRHIST;

// The USB consumer owns its cursor even when BTstack requests a reset.
static mic_pcm_ring_t mic_ring;
static uint32_t mic_ring_overrun;

static void mic_pcm_write(const int16_t *src, uint32_t count) {
    uint32_t written = mic_pcm_ring_write(&mic_ring, src, count);
    mic_ring_overrun += count - written;
}

uint32_t aacp_mic_pcm_read(int16_t *dst, uint32_t count) {
    return mic_pcm_ring_read(&mic_ring, dst, count);
}

void aacp_mic_pcm_reset(void) {
    mic_pcm_ring_reset(&mic_ring);
}

bool aacp_mic_dec_reset(void) {
    aacp_mic_pcm_reset();
    mic_dec_flags = AACDEC_CLRHIST;
    HANDLE_AACDECODER decoder = atomic_load_explicit(&mic_dec, memory_order_acquire);
    if (decoder == NULL) return false;
    return aacDecoder_SetParam(decoder, AAC_TPDEC_CLEAR_BUFFER, 1) == AAC_DEC_OK;
}

static struct {
    // interval (reset each report)
    uint32_t ok_interval;
    uint32_t err_interval;
    uint32_t dec_us_sum;
    uint32_t dec_us_max;
    uint64_t sq_sum;          // sum of sample^2 across the interval
    uint32_t sq_samples;      // samples contributing to sq_sum
    int32_t  peak;            // max |sample| this interval
    // cumulative (reset on mic START)
    uint32_t ok_total;
    uint32_t err_total;
} dstat;

// NOTE: must be called from plain thread context (main(), at boot) — NOT from
// a BTstack/async-context callback. aacDecoder_Open mallocs ~258KB in many
// chunks; doing that from the async worker context hung the system on
// hardware (same family as upstream's "FDK-AAC hangs on Core 1" note).
// Heap occupancy is logged at open so an out-of-memory failure is visible:
// arena = heap claimed from the system, uordblks = in use, fordblks = free;
// region = the total heap area from the linker (end .. __StackLimit).
extern char __StackLimit[];
extern char end[];   // linker: end of .bss = start of heap

static void dec_report_heap(const char *tag) {
    struct mallinfo mi = mallinfo();
    printf("[DEC] heap %s: region=%u arena=%u inuse=%u free=%u\n",
           tag, (unsigned)((uintptr_t)__StackLimit - (uintptr_t)end),
           (unsigned) mi.arena, (unsigned) mi.uordblks, (unsigned) mi.fordblks);
}

bool aacp_mic_dec_init(void) {
    if (atomic_load_explicit(&mic_dec, memory_order_acquire) != NULL) return true;

    dec_report_heap("before");
    printf("[DEC] aacDecoder_Open ...\n");
    HANDLE_AACDECODER decoder = aacDecoder_Open(TT_MP4_RAW, /* nrOfLayers */ 1);
    if (decoder == NULL) {
        printf("[DEC] aacDecoder_Open FAILED (out of heap?)\n");
        return false;
    }

    printf("[DEC] aacDecoder_ConfigRaw ...\n");
    UCHAR *conf[] = { aacp_mic_asc };
    UINT   clen   = sizeof(aacp_mic_asc);
    AAC_DECODER_ERROR err = aacDecoder_ConfigRaw(decoder, conf, &clen);
    if (err != AAC_DEC_OK) {
        printf("[DEC] aacDecoder_ConfigRaw FAILED: 0x%x\n", err);
        aacDecoder_Close(decoder);
        return false;
    }

    // BTstack IRQs may ask for capture while main is still configuring FDK.
    atomic_store_explicit(&mic_dec, decoder, memory_order_release);
    dec_report_heap("after");
    printf("[DEC] decoder ready (TT_MP4_RAW, ASC F8 E6 30 00)\n");
    return true;
}

bool aacp_mic_dec_ready(void) {
    return atomic_load_explicit(&mic_dec, memory_order_acquire) != NULL;
}

void aacp_mic_dec_reset_stats(void) {
    memset(&dstat, 0, sizeof(dstat));
    mic_dec_streaminfo_logged = false;
}

static bool mic_decode_failed(void) {
    dstat.err_interval++;
    dstat.err_total++;
    // Broken input must not leave stale AAC prediction or PCM for the next AU.
    (void)aacp_mic_dec_reset();
    return false;
}

bool aacp_mic_dec_decode(const uint8_t *au, uint16_t len) {
    HANDLE_AACDECODER decoder = atomic_load_explicit(&mic_dec, memory_order_acquire);
    if (decoder == NULL || au == NULL || len == 0 || len > MIC_AU_MAX)
        return mic_decode_failed();

    UCHAR *inb[] = { (UCHAR *)au };
    UINT size = len;
    UINT valid = len;
    if (aacDecoder_Fill(decoder, inb, &size, &valid) != AAC_DEC_OK || valid != 0)
        return mic_decode_failed();

    uint32_t start = time_us_32();
    AAC_DECODER_ERROR error = aacDecoder_DecodeFrame(decoder, mic_pcm,
                                                   MIC_PCM_MAX_SAMPLES, mic_dec_flags);
    uint32_t elapsed = time_us_32() - start;
    if (error != AAC_DEC_OK) return mic_decode_failed();
    mic_dec_flags = 0;

    CStreamInfo *info = aacDecoder_GetStreamInfo(decoder);
    if (info == NULL || info->numChannels != 1 || info->frameSize != 480 ||
        info->sampleRate != 48000 || info->aot != 39)
        return mic_decode_failed();

    if (!mic_dec_streaminfo_logged) {
        mic_dec_streaminfo_logged = true;
        printf("[DEC] stream: sampleRate=%d numChannels=%d frameSize=%d aot=%d\n",
               (int)info->sampleRate, (int)info->numChannels,
               (int)info->frameSize, (int)info->aot);
    }
    dstat.ok_interval++;
    dstat.ok_total++;
    dstat.dec_us_sum += elapsed;
    if (elapsed > dstat.dec_us_max) dstat.dec_us_max = elapsed;
    mic_pcm_write(mic_pcm, 480);
    for (unsigned i = 0; i < 480; ++i) {
        int32_t sample = mic_pcm[i];
        dstat.sq_sum += (uint64_t)((int64_t)sample * sample);
        int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > dstat.peak) dstat.peak = magnitude;
    }
    dstat.sq_samples += 480;
    return true;
}

void aacp_mic_dec_report(void) {
    uint32_t ok = dstat.ok_interval;
    uint32_t avg_us = ok ? dstat.dec_us_sum / ok : 0;
    // RMS from the mean square; M33 has an FPU, sqrtf is cheap here (1 Hz).
    uint32_t rms = 0;
    if (dstat.sq_samples > 0) {
        rms = (uint32_t) sqrtf((float)(dstat.sq_sum / dstat.sq_samples));
    }
    printf("[DEC] ok/s=%lu err=%lu(total %lu) dec_us avg/max=%lu/%lu rms=%lu peak=%ld ring=%lu%s\n",
           (unsigned long) ok,
           (unsigned long) dstat.err_interval,
           (unsigned long) dstat.err_total,
           (unsigned long) avg_us,
           (unsigned long) dstat.dec_us_max,
           (unsigned long) rms,
           (long) dstat.peak,
           (unsigned long)(atomic_load_explicit(&mic_ring.written, memory_order_relaxed) -
                           atomic_load_explicit(&mic_ring.read, memory_order_relaxed)),
           mic_ring_overrun ? " OVERRUN" : "");

    dstat.ok_interval  = 0;
    dstat.err_interval = 0;
    dstat.dec_us_sum   = 0;
    dstat.dec_us_max   = 0;
    dstat.sq_sum       = 0;
    dstat.sq_samples   = 0;
    dstat.peak         = 0;
}
