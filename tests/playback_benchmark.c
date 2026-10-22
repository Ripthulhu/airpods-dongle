// SPDX-License-Identifier: GPL-3.0-only
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include "../src/btstack/playback_pcm.h"

static playback_pcm_t pcm;
static int16_t input[48 * 2], output[1024 * 2];
static volatile uint64_t checksum;

static uint64_t nanoseconds(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000000u + (uint64_t)value.tv_nsec;
}

static void run(const char *name, uint32_t frames, uint32_t capacity, bool prime) {
    const unsigned iterations = 100000;
    playback_pcm_reset(&pcm, capacity, prime);
    uint64_t start = nanoseconds();
    for (unsigned i = 0; i < iterations; ++i) {
        for (uint32_t sent = 0; sent < frames;) {
            uint32_t chunk = frames - sent < 48 ? frames - sent : 48;
            playback_pcm_push(&pcm, input, chunk);
            sent += chunk;
        }
        playback_pcm_read(&pcm, output, frames);
        checksum += (uint16_t)output[(i % frames) * 2];
    }
    uint64_t elapsed = nanoseconds() - start;
    printf("%s: %.1f ns/block, checksum=%" PRIu64 "\n", name,
           (double)elapsed / iterations, checksum);
}

int main(void) {
    for (unsigned i = 0; i < 48; ++i) {
        input[i * 2] = (int16_t)(i + 1);
        input[i * 2 + 1] = (int16_t)-(int16_t)(i + 1);
    }
    run("ELD480", 480, 7680, true);
    run("AAC1024", 1024, 8192, false);
    run("SBC128", 128, 1024, false);
    return 0;
}
