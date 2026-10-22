// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <aacdecoder_lib.h>
#include "../src/btstack/aacp_mic_dec.h"

char __StackLimit[1];
static CStreamInfo info = {48000, 480, 1, 39};
static bool fill_error, partial_fill, decode_error, no_info, allocation_error, config_error, reset_error;
static unsigned core, opened, closed, decoded, reset_count, wakes, logs;
static UINT last_flags;
static uint8_t expected_au;
static bool reset_in_decode;
static bool release_in_decode, decoder_live;

static int checked_printf(const char *format, ...) {
    assert(core == 0); // No worker access to the single-core CDC producer.
    (void)format;
    ++logs;
    return 0;
}
#define printf checked_printf
#include "../src/btstack/aacp_mic_dec.c"
#undef printf

void aacp_mic_dec_wake_worker(void) { ++wakes; }
HANDLE_AACDECODER aacDecoder_Open(int transport, UINT layers) {
    assert(core == 0 && transport == TT_MP4_RAW && layers == 1);
    assert(!decoder_live);
    if (allocation_error) return NULL;
    decoder_live = true;
    ++opened;
    return &info;
}
void aacDecoder_Close(HANDLE_AACDECODER decoder) {
    assert(core == 0 && decoder == &info && decoder_live);
    decoder_live = false;
    ++closed;
}
AAC_DECODER_ERROR aacDecoder_ConfigRaw(HANDLE_AACDECODER decoder, UCHAR **config, UINT *size) {
    const UCHAR expected[] = {0xf8, 0xe6, 0x30, 0x00};
    assert(core == 0 && decoder == &info && *size == sizeof(expected));
    assert(memcmp(*config, expected, sizeof(expected)) == 0);
    assert(!aacp_mic_dec_ready());
    return config_error ? 1 : AAC_DEC_OK;
}
AAC_DECODER_ERROR aacDecoder_SetParam(HANDLE_AACDECODER decoder, int param, int value) {
    assert(core == 1 && decoder == &info && decoder_live && param == AAC_TPDEC_CLEAR_BUFFER && value == 1);
    ++reset_count;
    return reset_error ? 1 : AAC_DEC_OK;
}
AAC_DECODER_ERROR aacDecoder_Fill(HANDLE_AACDECODER decoder, UCHAR **bytes, UINT *size, UINT *valid) {
    assert(core == 1 && decoder == &info && decoder_live && *size == *valid);
    assert((*bytes)[0] == expected_au); // Source caller may already have reused its SDU.
    *valid = partial_fill ? 1 : 0;
    return fill_error ? 1 : AAC_DEC_OK;
}
AAC_DECODER_ERROR aacDecoder_DecodeFrame(HANDLE_AACDECODER decoder, INT_PCM *pcm, int capacity, UINT flags) {
    assert(core == 1 && decoder == &info && decoder_live && capacity == 480);
    ++decoded;
    last_flags = flags;
    for (unsigned i = 0; i < 480; ++i) pcm[i] = (int16_t)(i + 1);
    if (reset_in_decode) {
        // Simulate a core0 USB-close/reset while core1 is inside the codec.
        core = 0;
        assert(aacp_mic_dec_reset());
        core = 1;
        reset_in_decode = false;
    }
    if (release_in_decode) {
        unsigned previous_closes = closed;
        core = 0;
        assert(!aacp_mic_dec_release() && !aacp_mic_dec_ready());
        assert(!aacp_mic_dec_init() && closed == previous_closes && decoder_live);
        core = 1;
        release_in_decode = false;
    }
    return decode_error ? 1 : AAC_DEC_OK;
}
CStreamInfo *aacDecoder_GetStreamInfo(HANDLE_AACDECODER decoder) {
    assert(core == 1 && decoder == &info && decoder_live);
    return no_info ? NULL : &info;
}
uint32_t time_us_32(void) { static uint32_t now; return ++now; }

static bool step(void) {
    core = 1;
    bool worked = aacp_mic_dec_worker_step();
    core = 0;
    return worked;
}
static void drain(void) {
    unsigned iterations = 0;
    while (step()) assert(++iterations <= MIC_INPUT_FRAMES + 4u);
}
static void enqueue(void) {
    uint8_t au[] = {37, 2, 3};
    expected_au = au[0];
    assert(aacp_mic_dec_submit(au, sizeof(au)));
    memset(au, 0xa5, sizeof(au));
}
static void reset_and_drain(void) {
    assert(aacp_mic_dec_reset());
    assert(!aacp_mic_dec_reset_done());
    drain();
    assert(aacp_mic_dec_reset_done());
    int16_t discarded[480];
    assert(aacp_mic_pcm_read(discarded, 480) == 0);
}

int main(void) {
    int16_t pcm[960];
    assert(!step() && !aacp_mic_dec_ready());
    assert(!aacp_mic_dec_reset_done());
    allocation_error = true;
    assert(!aacp_mic_dec_init() && !aacp_mic_dec_ready() && closed == 0 && !decoder_live);
    allocation_error = false;
    config_error = true;
    assert(!aacp_mic_dec_init() && !aacp_mic_dec_ready() && closed == 1);
    config_error = false;
    assert(aacp_mic_dec_init() && aacp_mic_dec_ready());
    assert(step() && aacp_mic_dec_reset_done());
    assert(!step());

    unsigned before = decoded;
    enqueue();
    assert(decoded == before && aacp_mic_dec_completed() == 0);
    assert(step() && decoded == before + 1 && last_flags == AACDEC_CLRHIST);
    assert(aacp_mic_dec_completed() == 1);
    assert(aacp_mic_pcm_read(pcm, 64) == 64);
    for (unsigned i = 0; i < 64; ++i) assert(pcm[i] == (int16_t)(i + 1));
    assert(aacp_mic_pcm_read(pcm, 480) == 416);
    for (unsigned i = 0; i < 416; ++i) assert(pcm[i] == (int16_t)(i + 65));
    enqueue();
    assert(step() && last_flags == 0);

    // Both already-published PCM and queued input are invalidated by a reset.
    enqueue();
    before = decoded;
    uint32_t done = aacp_mic_dec_completed();
    assert(aacp_mic_dec_reset());
    assert(aacp_mic_pcm_read(pcm, 960) == 0);
    drain();
    assert(decoded == before && aacp_mic_dec_completed() == done);
    enqueue();
    assert(step() && last_flags == AACDEC_CLRHIST);
    assert(aacp_mic_pcm_read(pcm, 480) == 480);

    // A stop while the codec is busy cannot publish its stale result.
    enqueue();
    reset_in_decode = true;
    done = aacp_mic_dec_completed();
    assert(step() && aacp_mic_dec_completed() == done);
    assert(aacp_mic_pcm_read(pcm, 480) == 0);
    drain();
    assert(aacp_mic_dec_reset_done());

    // A reset immediately AFTER publication rejects that frame too.
    enqueue();
    assert(step());
    aacp_mic_pcm_reset();
    assert(aacp_mic_pcm_read(pcm, 480) == 0);
    drain();

    // A partial read followed by cancellation must not expose its remainder.
    enqueue();
    assert(step() && aacp_mic_pcm_read(pcm, 27) == 27);
    reset_and_drain();
    enqueue();
    assert(step() && aacp_mic_pcm_read(pcm, 480) == 480);
    assert(pcm[0] == 1 && pcm[479] == 480);

    // Queue exhaustion drops the generation; it does not grow or decode stale work.
    for (unsigned i = 0; i < MIC_INPUT_FRAMES; ++i) enqueue();
    uint8_t au[] = {37};
    before = decoded;
    done = aacp_mic_dec_completed();
    assert(!aacp_mic_dec_submit(au, sizeof(au)));
    drain();
    assert(decoded == before && aacp_mic_dec_completed() == done);
    assert(aacp_mic_dec_reset_done());

    // Wrong format, fill/decode failures and malformed input discard all old PCM.
    int *fields[] = {&info.numChannels, &info.frameSize, &info.sampleRate, &info.aot};
    const int wrong[] = {2, 960, 64000, 2};
    for (unsigned i = 0; i < 4; ++i) {
        int saved = *fields[i];
        *fields[i] = wrong[i];
        enqueue();
        done = aacp_mic_dec_completed();
        assert(step() && aacp_mic_dec_completed() == done);
        assert(aacp_mic_pcm_read(pcm, 480) == 0);
        *fields[i] = saved;
        drain();
        enqueue();
        assert(step() && last_flags == AACDEC_CLRHIST);
        assert(aacp_mic_pcm_read(pcm, 480) == 480);
    }
    bool *errors[] = {&fill_error, &partial_fill, &decode_error, &no_info};
    for (unsigned i = 0; i < 4; ++i) {
        *errors[i] = true;
        enqueue();
        done = aacp_mic_dec_completed();
        assert(step() && aacp_mic_dec_completed() == done);
        assert(aacp_mic_pcm_read(pcm, 480) == 0);
        *errors[i] = false;
        drain();
    }
    assert(!aacp_mic_dec_submit(NULL, 3));
    assert(!aacp_mic_dec_submit(au, 0));
    assert(!aacp_mic_dec_submit(au, 256));
    drain();

    // Slow/closed USB cannot cause an unbounded output backlog.
    for (unsigned i = 0; i < MIC_OUTPUT_FRAMES; ++i) {
        enqueue();
        assert(step());
    }
    enqueue();
    done = aacp_mic_dec_completed();
    assert(step() && aacp_mic_dec_completed() == done);
    assert(aacp_mic_pcm_read(pcm, 960) == 0);
    drain();

    // Cursor wrap and generation wrap retain the same ownership rules.
    atomic_store(&input_written, UINT32_MAX - 2u);
    atomic_store(&input_read, UINT32_MAX - 2u);
    atomic_store(&output_written, UINT32_MAX - 2u);
    atomic_store(&output_read, UINT32_MAX - 2u);
    atomic_store(&generation, UINT32_MAX);
    drain();
    for (unsigned i = 0; i < 8; ++i) {
        enqueue();
        assert(step() && aacp_mic_pcm_read(pcm, 480) == 480);
    }
    reset_and_drain();
    assert(atomic_load(&generation) == 0);
    done = aacp_mic_dec_completed();
    aacp_mic_dec_reset_stats();
    drain();
    assert(aacp_mic_dec_completed() == done);

    // Snapshot exchange is nonblocking and formatted only by core0.
    unsigned previous_logs = logs;
    aacp_mic_dec_report();
    assert(step() && logs == previous_logs);
    aacp_mic_dec_report();
    assert(!step()); // One unread snapshot prevents overwrite.
    aacp_mic_dec_poll();
    assert(logs == previous_logs + 1);
    assert(step());
    aacp_mic_dec_poll();
    assert(!step());

    // Release never frees an in-flight codec or publishes its cancelled PCM.
    enqueue();
    enqueue();
    before = decoded;
    done = aacp_mic_dec_completed();
    release_in_decode = true;
    assert(step() && decoded == before + 1 && aacp_mic_dec_completed() == done);
    assert(aacp_mic_pcm_read(pcm, 480) == 0);
    unsigned previous_closes = closed;
    assert(!aacp_mic_dec_release() && closed == previous_closes);
    assert(step() && closed == previous_closes && !aacp_mic_dec_ready());
    assert(aacp_mic_dec_release() && closed == previous_closes + 1 && !decoder_live);
    assert(aacp_mic_dec_release() && closed == previous_closes + 1 && !step());
    assert(!aacp_mic_dec_ready() && !aacp_mic_dec_reset_done());
    assert(!aacp_mic_dec_submit(au, sizeof(au)));
    unsigned previous_opens = opened;
    assert(aacp_mic_dec_init() && opened == previous_opens + 1 && aacp_mic_dec_ready());
    assert(!aacp_mic_dec_reset_done());
    drain();
    assert(decoded == before + 1); // The second queued AU belonged to the retired handle.
    assert(aacp_mic_dec_reset_done() && aacp_mic_pcm_read(pcm, 480) == 0);
    enqueue();
    assert(step() && last_flags == AACDEC_CLRHIST && aacp_mic_pcm_read(pcm, 480) == 480);

    // Failed reset gates capture until the handle is safely replaced.
    reset_error = true;
    assert(aacp_mic_dec_reset());
    assert(step());
    assert(!aacp_mic_dec_ready() && !aacp_mic_dec_reset_done());
    assert(!aacp_mic_dec_submit(au, sizeof(au)) && !step());
    previous_logs = logs;
    aacp_mic_dec_poll();
    assert(logs == previous_logs + 1);
    aacp_mic_dec_poll();
    assert(logs == previous_logs + 1);
    // Failed handles can also be detached and a later USB capture can initialize anew.
    assert(!aacp_mic_dec_release());
    assert(step() && aacp_mic_dec_release());
    reset_error = false;
    assert(aacp_mic_dec_init());
    drain();
    assert(aacp_mic_dec_ready() && aacp_mic_dec_reset_done());
    enqueue();
    assert(step() && aacp_mic_pcm_read(pcm, 480) == 480);
    assert(wakes != 0);
    puts("mic worker ownership, queue bounds, generations, cancellation and fault recovery: passed");
}
