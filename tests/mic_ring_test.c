// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/btstack/mic_pcm_ring.h"
#include "../src/tinyusb/mic_request.h"
#include "../src/tinyusb/audio_control_validation.h"

int main(void) {
    static mic_pcm_ring_t ring;
    int16_t input[MIC_RING_SAMPLES + 1], output[MIC_RING_SAMPLES];
    for (unsigned i = 0; i < MIC_RING_SAMPLES + 1; ++i) input[i] = (int16_t)i;
    assert(mic_pcm_ring_read(&ring, output, 1) == 0);
    assert(mic_pcm_ring_write(&ring, input, MIC_RING_SAMPLES + 1) == MIC_RING_SAMPLES);
    assert(mic_pcm_ring_write(&ring, input, 1) == 0);
    assert(mic_pcm_ring_read(&ring, output, MIC_RING_SAMPLES) == MIC_RING_SAMPLES);
    for (unsigned i = 0; i < MIC_RING_SAMPLES; ++i) assert(output[i] == input[i]);
    atomic_store(&ring.written, UINT32_MAX - 7u);
    atomic_store(&ring.read, UINT32_MAX - 7u);
    assert(mic_pcm_ring_write(&ring, input, 32) == 32);
    assert(mic_pcm_ring_read(&ring, output, 13) == 13);
    for (unsigned i = 0; i < 13; ++i) assert(output[i] == input[i]);
    mic_pcm_ring_reset(&ring);
    assert(mic_pcm_ring_read(&ring, output, 32) == 0);
    assert(mic_pcm_ring_write(&ring, input + 100, 32) == 32);
    assert(mic_pcm_ring_read(&ring, output, 32) == 32);
    for (unsigned i = 0; i < 32; ++i) assert(output[i] == input[100 + i]);
    mic_pcm_ring_reset(&ring);
    mic_pcm_ring_reset(&ring);
    assert(mic_pcm_ring_read(&ring, output, 1) == 0);

    const uint32_t starts[] = {0, 1, MIC_RING_SAMPLES - 1u, UINT32_MAX - 7u, UINT32_MAX};
    const uint32_t counts[] = {0, 1, 63, 64, 479, 480, MIC_RING_SAMPLES};
    for (unsigned i = 0; i < sizeof(starts) / sizeof(*starts); ++i) {
        for (unsigned j = 0; j < sizeof(counts) / sizeof(*counts); ++j) {
            atomic_store(&ring.written, starts[i]);
            atomic_store(&ring.read, starts[i]);
            uint32_t count = counts[j];
            assert(mic_pcm_ring_write(&ring, input, count) == count);
            uint32_t first = count / 2;
            assert(mic_pcm_ring_read(&ring, output, first) == first);
            assert(mic_pcm_ring_read(&ring, output + first, count - first) == count - first);
            for (uint32_t k = 0; k < count; ++k) assert(output[k] == input[k]);
            assert(mic_pcm_ring_read(&ring, output, 1) == 0);
        }
    }
    assert(mic_pcm_ring_write(&ring, NULL, 0) == 0);
    assert(mic_pcm_ring_read(&ring, NULL, 0) == 0);

    usb_mic_request_t request = {0};
    bool wanted = true;
    assert(!usb_mic_request_take(&request, &wanted));
    usb_mic_request_set(&request, true);
    usb_mic_request_set(&request, false);
    assert(usb_mic_request_take(&request, &wanted) && !wanted);
    usb_mic_request_set(&request, true);
    usb_mic_request_set(&request, false);
    usb_mic_request_set(&request, true);
    assert(usb_mic_request_take(&request, &wanted) && wanted);
    assert(usb_mic_request_wanted(&request));
    assert(!usb_mic_request_take(&request, &wanted));
    usb_mic_request_set(&request, false);
    assert(!usb_mic_request_wanted(&request));
    assert(usb_mic_request_take(&request, &wanted) && !wanted);

    const uint8_t rate[] = {0x00, 0xfa, 0x00, 0x00};
    assert(usb_audio_fixed_rate(rate, 4, 64000));
    assert(!usb_audio_fixed_rate(rate, 3, 64000));
    assert(!usb_audio_fixed_rate(rate, 4, 48000));
    assert(!usb_audio_fixed_rate(NULL, 4, 64000));
    int16_t volume;
    const uint8_t min_volume[] = {0, 0xce}, max_volume[] = {0, 0};
    const uint8_t too_quiet[] = {0xff, 0xcd}, too_loud[] = {1, 0};
    assert(usb_audio_volume_value(min_volume, 2, &volume) && volume == -12800);
    assert(usb_audio_volume_value(max_volume, 2, &volume) && volume == 0);
    assert(!usb_audio_volume_value(too_quiet, 2, &volume));
    assert(!usb_audio_volume_value(too_loud, 2, &volume));
    assert(!usb_audio_volume_value(max_volume, 1, &volume));
    puts("mic ring, USB request ordering and control validation: passed");
}
