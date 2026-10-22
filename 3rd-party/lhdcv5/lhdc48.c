// SPDX-License-Identifier: GPL-3.0-only
#include "lhdc48.h"
#include "decoder/lhdc_dec.h"
#include "decoder/lhdc_imdct.h"
#include <stdlib.h>
#include <string.h>

static lhdc_decoder_t *decoder;
static uint8_t source_bits;
extern int lhdc_dec_prewarm_48(void);
extern void lhdc_dec_free_window(void);
extern void lhdc_dec_free_slot_scratch(void);

bool lhdc48_ready(void) { return decoder != NULL; }
void lhdc48_close(void) {
    if (decoder) { lhdc_dec_deinit(decoder); free(decoder); decoder = NULL; }
    lhdc_dec_free_window();
    lhdc_dec_free_slot_scratch();
    lhdc_imdct_free_480(); lhdc_imdct_free_960();
    lhdc_imdct_free_1920(); lhdc_imdct_free_cos();
}
bool lhdc48_prepare(uint8_t bit_depth) {
    lhdc48_close();
    if (bit_depth != 16 && bit_depth != 24) return false;
    void *storage = calloc(1, lhdc_dec_get_workspace_size(48000, 5));
    if (!storage) return false;
    lhdc_dec_config_t config = {
        .sample_rate = LHDC_DEC_SR_48000, .bit_depth = bit_depth,
        .frame_duration = LHDC_DEC_FRAME_5MS, .channels = 2, .max_frame_bytes = 1024,
    };
    decoder = lhdc_dec_init(storage, &config);
    if (!decoder) { free(storage); lhdc48_close(); return false; }
    if (!lhdc_dec_prewarm_48()) { lhdc48_close(); return false; }
    source_bits = bit_depth;
    return true;
}
void lhdc48_reset(void) { if (decoder) lhdc_dec_reset(decoder); }

static size_t frame_length(const uint8_t *input, size_t available) {
    if (available < 2) return 0;
    unsigned header = input[0] | (unsigned)input[1] << 8;
    size_t payload = (header & 0x3ffu) * 2u;
    // Only the reviewed 48 kHz / 5 ms stereo framing; each channel needs its header.
    if (header >> 10 != 1 || payload < 18 || payload + 2 > available) return 0;
    return payload + 2;
}
bool lhdc48_decode(const uint8_t *input, size_t length, uint8_t frames,
                   lhdc48_pcm_callback callback, void *context) {
    if (!decoder || !input || !callback || !frames || frames > 8 || length > 1024) return false;
    size_t offset = 0;
    for (unsigned i = 0; i < frames; ++i) {
        size_t bytes = frame_length(input + offset, length - offset);
        if (!bytes) return false;
        offset += bytes;
    }
    if (offset != length) return false;
    for (unsigned i = 0; i < frames; ++i) {
        // Upstream writes 24-bit input into signed 32-bit containers.
        int32_t wide[480];
        int16_t narrow[480];
        size_t bytes = frame_length(input, length), consumed = 0;
        uint32_t generated = 0;
        void *output = source_bits == 24 ? (void *)wide : (void *)narrow;
        if (lhdc_dec_decode_frame(decoder, input, bytes, output, 240, &consumed,
                                  &generated, NULL) != LHDC_DEC_OK ||
            consumed != bytes || generated != 240) {
            lhdc48_reset();
            return false;
        }
        if (source_bits == 24) {
            for (unsigned n = 0; n < 480; ++n) {
                uint16_t upper = (uint32_t)wide[n] >> 16;
                memcpy(narrow + n, &upper, sizeof(upper));
            }
        }
        callback(narrow, 240, context);
        input += consumed; length -= consumed;
    }
    return true;
}
