// SPDX-License-Identifier: GPL-3.0-only
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <aacdecoder_lib.h>
#include "../src/btstack/aacp_mic_dec.h"

char __StackLimit[1];
static CStreamInfo info = {48000, 480, 1, 39};
static _Thread_local bool worker_thread;
static _Atomic bool finished, worker_finished;
static _Atomic unsigned delivered, probe_delivered;
static _Atomic bool decoder_live;
static _Atomic unsigned codec_calls;
static uint16_t sequence;

static int checked_printf(const char *format, ...) {
    assert(!worker_thread);
    (void)format;
    return 0;
}
#define printf checked_printf
#include "../src/btstack/aacp_mic_dec.c"
#undef printf

void aacp_mic_dec_wake_worker(void) {}
HANDLE_AACDECODER aacDecoder_Open(int transport, UINT layers) {
    assert(!worker_thread && transport == TT_MP4_RAW && layers == 1);
    assert(!atomic_exchange_explicit(&decoder_live, true, memory_order_acq_rel));
    return &info;
}
void aacDecoder_Close(HANDLE_AACDECODER decoder) {
    assert(!worker_thread && decoder == &info && atomic_load(&codec_calls) == 0);
    assert(atomic_exchange_explicit(&decoder_live, false, memory_order_acq_rel));
}
AAC_DECODER_ERROR aacDecoder_ConfigRaw(HANDLE_AACDECODER decoder, UCHAR **config, UINT *size) {
    assert(!worker_thread && decoder == &info && config && *size == 4);
    return AAC_DEC_OK;
}
AAC_DECODER_ERROR aacDecoder_SetParam(HANDLE_AACDECODER decoder, int param, int value) {
    assert(worker_thread && decoder == &info && param == AAC_TPDEC_CLEAR_BUFFER && value == 1);
    assert(atomic_load_explicit(&decoder_live, memory_order_acquire));
    return AAC_DEC_OK;
}
AAC_DECODER_ERROR aacDecoder_Fill(HANDLE_AACDECODER decoder, UCHAR **bytes, UINT *size, UINT *valid) {
    assert(worker_thread && decoder == &info && *size == 255 && *valid == *size);
    atomic_fetch_add(&codec_calls, 1);
    assert(atomic_load_explicit(&decoder_live, memory_order_acquire));
    uint16_t next = (uint16_t)((*bytes)[0] | ((*bytes)[1] << 8));
    assert(next > sequence);
    sequence = next;
    for (unsigned i = 2; i < *size; ++i) assert((*bytes)[i] == (uint8_t)(sequence + i));
    *valid = 0;
    atomic_fetch_sub(&codec_calls, 1);
    return AAC_DEC_OK;
}
AAC_DECODER_ERROR aacDecoder_DecodeFrame(HANDLE_AACDECODER decoder, INT_PCM *pcm, int capacity, UINT flags) {
    (void)flags;
    assert(worker_thread && decoder == &info && capacity == 480);
    atomic_fetch_add(&codec_calls, 1);
    assert(atomic_load_explicit(&decoder_live, memory_order_acquire));
    for (unsigned i = 0; i < 480; ++i) pcm[i] = (INT_PCM)(int16_t)sequence*(sizeof(INT_PCM)==4?65536:1);
    if ((sequence & 31u) == 0) sched_yield();
    assert(atomic_load_explicit(&decoder_live, memory_order_acquire));
    atomic_fetch_sub(&codec_calls, 1);
    return AAC_DEC_OK;
}
CStreamInfo *aacDecoder_GetStreamInfo(HANDLE_AACDECODER decoder) {
    assert(worker_thread && decoder == &info);
    assert(atomic_load_explicit(&decoder_live, memory_order_acquire));
    return &info;
}
uint32_t time_us_32(void) { static uint32_t now; assert(worker_thread); return ++now; }

static void *worker(void *arg) {
    (void)arg;
    worker_thread = true;
    for (;;) {
        if (!aacp_mic_dec_worker_step()) {
            if (atomic_load_explicit(&finished, memory_order_acquire)) break;
            sched_yield();
        }
    }
    atomic_store_explicit(&worker_finished, true, memory_order_release);
    return NULL;
}
static void *usb_consumer(void *arg) {
    (void)arg;
    int16_t pcm[64], previous = 0;
    for (;;) {
        unsigned count = aacp_mic_pcm_read(pcm, 64);
        for (unsigned i = 0; i < count; ++i) {
            assert(pcm[i] >= previous && pcm[i] <= 20001);
            previous = pcm[i];
            if (pcm[i] == 20001) atomic_fetch_add(&probe_delivered, 1u);
        }
        atomic_fetch_add_explicit(&delivered, count, memory_order_relaxed);
        if (atomic_load_explicit(&worker_finished, memory_order_acquire) && count == 0) break;
        if (count == 0) sched_yield();
    }
    return NULL;
}
int main(void) {
    assert(aacp_mic_dec_init());
    pthread_t decoder, usb;
    assert(pthread_create(&decoder, NULL, worker, NULL) == 0);
    assert(pthread_create(&usb, NULL, usb_consumer, NULL) == 0);
    uint8_t bytes[255];
    for (unsigned seq = 1; seq <= 20000; ++seq) {
        bytes[0] = (uint8_t)seq;
        bytes[1] = (uint8_t)(seq >> 8);
        for (unsigned i = 2; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(seq + i);
        if ((seq % 37u) == 0) assert(aacp_mic_dec_reset());
        if ((seq % 113u) == 0) aacp_mic_dec_reset_stats();
        (void)aacp_mic_dec_submit(bytes, sizeof(bytes));
        memset(bytes, 0xa5, sizeof(bytes));
        if ((seq % 5000u) == 0) {
            while (!aacp_mic_dec_release()) sched_yield();
            assert(!aacp_mic_dec_ready() && !atomic_load(&decoder_live));
            assert(aacp_mic_dec_init());
        }
        if ((seq % 17u) == 0) {
            aacp_mic_dec_report();
            aacp_mic_dec_poll();
            sched_yield();
        }
    }
    // Resets may discard every stress frame before a busy USB thread runs.
    struct timespec start, now, pause = {0, 1000000};
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    while (!aacp_mic_dec_reset_done() || atomic_load(&output_read) != atomic_load(&output_written)) {
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        assert(now.tv_sec - start.tv_sec < 5);
        assert(nanosleep(&pause, NULL) == 0);
    }
    bytes[0] = (uint8_t)20001u;
    bytes[1] = (uint8_t)(20001u >> 8);
    for (unsigned i = 2; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(20001u + i);
    assert(aacp_mic_dec_submit(bytes, sizeof(bytes)));
    memset(bytes, 0xa5, sizeof(bytes));
    while (atomic_load(&probe_delivered) != 480u) {
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        assert(now.tv_sec - start.tv_sec < 5);
        assert(nanosleep(&pause, NULL) == 0);
    }
    atomic_store_explicit(&finished, true, memory_order_release);
    assert(pthread_join(decoder, NULL) == 0 && pthread_join(usb, NULL) == 0);
    assert(aacp_mic_dec_completed() != 0 && atomic_load(&delivered) != 0);
    aacp_mic_dec_poll();
    puts("mic worker concurrent copied input, PCM ownership, resets and report mailbox: passed");
}
