// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/tinyusb/mic_request.h"
#include "../src/tinyusb/audio_control_validation.h"
#include "../src/btstack/volume_scale.h"
#include "../src/btstack/playback_pcm.h"
#include "../src/realtime_metrics.h"

// TinyUSB's full-speed endpoint sizing and public control wire layouts.
#define CFG_TUSB_MCU 1
#define TUD_AUDIO_EP_SIZE(rate, bytes, channels) ((((rate + 999) / 1000) + 1) * (bytes) * (channels))
#include "../tusb_config.h"
#define TU_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define TU_VERIFY(condition) do { if (!(condition)) return false; } while (0)
#define TU_ASSERT(condition) TU_VERIFY(condition)
#define TU_LOG1(...) ((void)0)
#define tu_le16toh(value) (value)
#define tu_htole16(value) (value)
#define tu_htole32(value) (value)
enum { AUDIO_CS_REQ_CUR = 1, AUDIO_CS_REQ_RANGE = 2,
       AUDIO_FU_CTRL_MUTE = 1, AUDIO_FU_CTRL_VOLUME = 2,
       AUDIO_CS_CTRL_SAM_FREQ = 1, AUDIO_CS_CTRL_CLK_VALID = 2 };
typedef struct __attribute__((packed)) {
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} tusb_control_request_t;
typedef struct __attribute__((packed)) {
    uint8_t bmRequestType, bRequest, bChannelNumber, bControlSelector, bInterface, bEntityID;
    uint16_t wLength;
} audio_control_request_t;
typedef struct { int8_t bCur; } audio_control_cur_1_t;
typedef struct { int16_t bCur; } audio_control_cur_2_t;
typedef struct { int32_t bCur; } audio_control_cur_4_t;
#define audio_control_range_2_n_t(n) struct { uint16_t wNumSubRanges; struct { int16_t bMin, bMax, bRes; } subrange[n]; }
#define audio_control_range_4_n_t(n) struct __attribute__((packed)) { uint16_t wNumSubRanges; struct { int32_t bMin, bMax, bRes; } subrange[n]; }
_Static_assert(sizeof(audio_control_range_4_n_t(1)) == 14, "UAC2 clock range wire size");
typedef struct {
    uint8_t bInfo, bAttribute, wValue_cn_or_mcn, wValue_cs, wIndex_ep_or_int, wIndex_entity_id;
} audio_interrupt_data_t;

static uint32_t now;
static unsigned disconnects, connects, descriptor_changes;
static bool presented_audio;
static bool terminal_connected;
static bool mounted, usb_context, media_available, bt_changed, int_busy, interrupts_disabled;
static bool inject_volume_set, bt_known;
static bool volume_ready=true;
static uint32_t media_generation, volume_cancels, seen_set_revision, media_sent, seen_receipt;
static uint8_t bt_volume;
static unsigned starts, stops, pushes, reads, fifo_clears, mic_reads, mic_resets;
static unsigned usb_tasks, media_tasks, debug_tasks, int_attempts, notifications, bt_sets;
static uint8_t traced_format, packet_format, packet_error;
static uint16_t packet_frames;
static int16_t last_bt_db256;
static uint8_t rx_fifo[2 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX];
static size_t rx_used, short_read_limit;
static int16_t played[2048];
static size_t played_samples;
static float played_float[2048];
static size_t played_float_samples;
static int16_t mic_pcm[USB_MIC_SAMPLE_RATE / 1000], host_mic[USB_MIC_SAMPLE_RATE / 1000];
static size_t mic_available;
static uint8_t control_reply[64];
static size_t control_reply_size;
static uint8_t interrupt_reply[6];

void audio_task(void);
void audio_control_task(void);
void tud_umount_cb(void);
bool tud_audio_set_req_entity_cb(uint8_t, tusb_control_request_t const *, uint8_t *);
static void host_volume(int16_t db256);

static uint32_t time_us_32(void) { return now; }
static void set_usb_streaming(bool active) { if (active) ++starts; else ++stops; }
static void dongle_playback_usb_packet(uint32_t us,uint16_t frames,uint8_t format,uint8_t error) {
    (void)us;packet_frames=frames;packet_format=format;packet_error=error;
}
static void dongle_playback_usb_format(uint8_t format) {traced_format=format;}
static void audio_slot_push_samples(const int16_t *pcm, uint16_t frames) {
    assert(played_samples + 2u * frames <= TU_ARRAY_SIZE(played));
    memcpy(played + played_samples, pcm, 2u * frames * sizeof(*pcm));
    played_samples += 2u * frames;
    ++pushes;
}
static void audio_slot_push_float(const float *pcm,uint16_t frames) {
    assert(played_float_samples+2u*frames<=TU_ARRAY_SIZE(played_float));
    memcpy(played_float+played_float_samples,pcm,2u*frames*sizeof(float));
    played_float_samples+=2u*frames;
    ++pushes;
}
static void audio_slot_usb_reset(void) { }
static uint16_t tud_audio_read(void *buf, uint16_t bytes) {
    assert(bytes <= CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX);
    size_t count = bytes < rx_used ? bytes : rx_used;
    if (count > short_read_limit) count = short_read_limit;
    memcpy(buf, rx_fifo, count);
    memmove(rx_fifo, rx_fifo + count, rx_used - count);
    rx_used -= count;
    ++reads;
    return (uint16_t)count;
}
static bool tud_audio_clear_ep_out_ff(void) { ++fifo_clears; rx_used = 0; return true; }
static void aacp_mic_pcm_reset(void) { ++mic_resets; mic_available = 0; }
static uint32_t aacp_mic_usb_read(int16_t *buf, uint32_t count) {
    ++mic_reads;
    if (count > mic_available) count = (uint32_t)mic_available;
    memcpy(buf, mic_pcm, count * sizeof(*buf));
    mic_available -= count;
    return count;
}
static uint16_t tud_audio_write(const void *buf, uint16_t size) {
    assert(size == sizeof(host_mic));
    memcpy(host_mic, buf, size);
    return size;
}
static void tud_task(void) { assert(usb_context); ++usb_tasks; }
static bool tud_mounted(void) { return mounted; }
static bool tud_disconnect(void) { assert(usb_context); ++disconnects; return true; }
static bool tud_connect(void) { assert(usb_context); ++connects; return true; }
void usb_descriptors_set_audio(bool enabled) {
    assert(usb_context && disconnects > connects);
    presented_audio = enabled; ++descriptor_changes;
}
static void usb_media_set_available(bool available) { media_available = available; media_generation=((media_generation+2u)&~1u)|(uint32_t)available; }
static uint32_t usb_media_generation(void) {return media_generation;}
static uint32_t usb_media_volume_sent(void) {return media_sent;}
static void usb_media_cancel_volume(void) {++volume_cancels;}
static void usb_media_task(void) { assert(usb_context); ++media_tasks; }
static void debug_cdc_task(void) { assert(usb_context); ++debug_tasks; }
static bool debug_cdc_terminal_connected(void) { return terminal_connected; }
static bool tud_audio_int_write(const audio_interrupt_data_t *data) {
    assert(usb_context);
    assert(data->bInfo == 0 && data->bAttribute == AUDIO_CS_REQ_CUR);
    assert(data->wValue_cn_or_mcn == 0 && data->wValue_cs == AUDIO_FU_CTRL_VOLUME);
    assert(data->wIndex_ep_or_int == 0 && data->wIndex_entity_id == UAC2_ENTITY_SPK_FEATURE_UNIT);
    ++int_attempts;
    if (int_busy) return false;
    _Static_assert(sizeof(audio_interrupt_data_t) == sizeof(interrupt_reply), "UAC2 interrupt wire size");
    memcpy(interrupt_reply, data, sizeof(interrupt_reply));
    ++notifications;
    return true;
}
static bool *get_is_bt_sink_volume_changed_ptr(void) { return &bt_changed; }
static bool get_bt_volume(uint8_t *out) {
    uint8_t previous = bt_volume;
    if (inject_volume_set) {
        assert(!interrupts_disabled);
        inject_volume_set = false;
        host_volume(-20 * 256);
    }
    *out = previous;
    return bt_known;
}
uint32_t usb_volume_set_revision(void);
static bool bt_volume_usb_ready(void) {return volume_ready;}
static void set_bt_volume_from_usb(int16_t db256,uint32_t revision,uint32_t generation,uint32_t receipt) {
    seen_receipt=receipt;
    assert(!usb_context && !interrupts_disabled && generation==media_generation);
    assert(revision==usb_volume_set_revision());seen_set_revision=revision;
    last_bt_db256=db256;++bt_sets;
}
static uint32_t save_and_disable_interrupts(void) {
    assert(!interrupts_disabled);
    interrupts_disabled = true;
    return 1;
}
static void restore_interrupts(uint32_t saved) { assert(saved == 1 && interrupts_disabled); interrupts_disabled = false; }
static bool tud_audio_buffer_and_schedule_control_xfer(uint8_t rhport, const tusb_control_request_t *request,
                                                       const void *buf, uint16_t size) {
    (void)rhport; (void)request;
    assert(size <= sizeof(control_reply));
    memcpy(control_reply, buf, size);
    control_reply_size = size;
    return true;
}

// All callback bodies and their state come from src/tinyusb/uac.c at test time.
#include "uac.inc"

static void service(void) { usb_context = true; tinyusb_task(); usb_context = false; }
static void fixture(void) {
    // Most callback tests start after a headset has connected and audio enumerated.
    atomic_store(&usb_audio_requested,true);atomic_store(&usb_audio_present,true);
    usb_descriptor_audio=presented_audio=true;usb_mode_reconnecting=false;usb_reconnect_at=0;
    disconnects=connects=descriptor_changes=0;
    terminal_connected=false;
    volume_ready=true;media_generation=volume_cancels=seen_set_revision=media_sent=seen_receipt=0;
    atomic_store(&volume_set_revision,0);atomic_store(&volume_set_generation,0);
    atomic_store(&need_change_bt_volume, false);
    for (unsigned i = 0; i < TU_ARRAY_SIZE(mute); ++i) { atomic_store(&mute[i], 0); atomic_store(&volume[i], 0); }
    mic_request = (usb_mic_request_t){0};
    usb_mounted = usb_suspended = mic_interface_open = speaker_active = speaker_interface_open = false;
    atomic_store(&usb_host_available, false);
    atomic_store(&volume_notification_pending, false);
    now = speaker_last_packet_us = 0;
    mounted = usb_context = media_available = bt_changed = int_busy = interrupts_disabled = inject_volume_set = bt_known = false;
    starts = stops = pushes = reads = fifo_clears = mic_reads = mic_resets = 0;
    usb_tasks = media_tasks = debug_tasks = int_attempts = notifications = bt_sets = 0;
    played_samples = rx_used = mic_available = control_reply_size = 0;
    played_float_samples=0;speaker_format=0;
    traced_format=packet_format=packet_error=0;packet_frames=0;
    short_read_limit = SIZE_MAX;
    last_bt_db256 = 999;
    bt_volume = 127;
    memset(host_mic, 0x55, sizeof(host_mic));
}
static void mount_host(void) {
    mounted=true;tud_mount_cb();
    assert(usb_mounted && atomic_load(&usb_host_available) && media_available);
    assert(usb_host_active()==usb_audio_available());
}
static void interface_alt(uint16_t itf, uint16_t alt, bool closing) {
    tusb_control_request_t request = {.wIndex = itf, .wValue = alt};
    assert(closing ? tud_audio_set_itf_close_EP_cb(0, &request) : tud_audio_set_itf_cb(0, &request));
}
static void open_speaker(void) { mount_host(); interface_alt(ITF_NUM_AUDIO_STREAMING_SPK, 1, false); }
static bool packet(const void *pcm, size_t bytes, uint16_t reported, uint8_t alt) {
    assert(rx_used + bytes <= sizeof(rx_fifo));
    if (bytes) memcpy(rx_fifo + rx_used, pcm, bytes);
    rx_used += bytes;
    return tud_audio_rx_done_post_read_cb(0, reported, 0, 1, alt);
}
static bool feature_set(uint8_t selector, uint8_t channel, uint8_t *data, uint16_t bytes) {
    audio_control_request_t request = {.bRequest = AUDIO_CS_REQ_CUR, .bChannelNumber = channel,
        .bControlSelector = selector, .bEntityID = UAC2_ENTITY_SPK_FEATURE_UNIT, .wLength = bytes};
    return tud_audio_set_req_entity_cb(0, (const tusb_control_request_t *)&request, data);
}
static void host_volume(int16_t db256) {
    uint8_t bytes[] = {(uint8_t)db256, (uint8_t)((uint16_t)db256 >> 8)};
    assert(feature_set(AUDIO_FU_CTRL_VOLUME, 0, bytes, sizeof(bytes)));
}
static int16_t read_volume(void) {
    audio_control_request_t request = {.bRequest = AUDIO_CS_REQ_CUR, .bControlSelector = AUDIO_FU_CTRL_VOLUME,
        .bEntityID = UAC2_ENTITY_SPK_FEATURE_UNIT, .wLength = 2};
    assert(tud_audio_get_req_entity_cb(0, (const tusb_control_request_t *)&request));
    assert(control_reply_size == 2);
    int16_t result;
    memcpy(&result, control_reply, sizeof(result));
    return result;
}
static void expect_silence(const int16_t *samples, size_t count) {
    for (size_t i = 0; i < count; ++i) assert(samples[i] == 0);
}

static void current_packet_stereo(void) {
    fixture(); open_speaker();
    const int16_t first[] = {101, -201, 102, -202, 103, -203};
    const int16_t second[] = {-30000, 30000, -1, 1};
    assert(packet(first, sizeof(first), sizeof(first), 1));
    assert(played_samples == TU_ARRAY_SIZE(first) && memcmp(played, first, sizeof(first)) == 0);
    assert(packet(second, sizeof(second), sizeof(second), 1));
    assert(played_samples == TU_ARRAY_SIZE(first) + TU_ARRAY_SIZE(second));
    assert(memcmp(played + TU_ARRAY_SIZE(first), second, sizeof(second)) == 0);
    assert(rx_used == 0 && pushes == 2 && starts == 1 && stops == 0);
    int16_t largest[CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT / sizeof(int16_t)];
    for (unsigned i = 0; i < TU_ARRAY_SIZE(largest); ++i) largest[i] = (int16_t)(500 + i);
    assert(packet(largest, sizeof(largest), sizeof(largest), 1));
    assert(memcmp(played + TU_ARRAY_SIZE(first) + TU_ARRAY_SIZE(second), largest, sizeof(largest)) == 0);
    int16_t ordinary[48 * 2];
    for (unsigned i = 0; i < TU_ARRAY_SIZE(ordinary); ++i) ordinary[i] = (int16_t)(-10000 + 29 * i);
    _Static_assert(sizeof(largest) == 196 && sizeof(ordinary) == 192, "48/49-frame USB packets");
    for (unsigned i = 0; i < 8; ++i) {
        const int16_t *pcm = i % 2 ? largest : ordinary;
        size_t bytes = i % 2 ? sizeof(largest) : sizeof(ordinary);
        size_t offset = played_samples;
        assert(packet(pcm, bytes, (uint16_t)bytes, 1));
        assert(played_samples == offset + bytes / sizeof(*pcm));
        assert(memcmp(played + offset, pcm, bytes) == 0);
    }
}
static void malformed_packets_are_discarded(void) {
    fixture(); open_speaker();
    uint8_t oversized[CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX + 4] = {0};
    assert(!packet(oversized, sizeof(oversized), sizeof(oversized), 1));
    assert(!packet(oversized, 6, 6, 1));
    const int16_t good[] = {99, -99, 100, -100};
    short_read_limit = 4;
    assert(!packet(good, sizeof(good), sizeof(good), 1));
    short_read_limit = SIZE_MAX;
    assert(pushes == 0 && starts == 0 && fifo_clears == 3 && rx_used == 0);
    assert(packet(good, sizeof(good), sizeof(good), 1));
    assert(played_samples == TU_ARRAY_SIZE(good) && memcmp(played, good, sizeof(good)) == 0);
    assert(packet(NULL, 0, 0, 1) && pushes == 1);
}
static void mute_preserves_volume_and_stereo(void) {
    const int16_t levels[] = {-50 * 256, -25 * 256, 0};
    const int16_t pcm[] = {32767, -32768, 1000, -1000, 17, -777};
    const float floating[] = {.12345678f, -.23456789f, .99999f, -.99999f, .0000001f, -.0000002f};
    for (unsigned alt = 1; alt <= (AIRPODS_USB_FLOAT ? 3u : 1u); ++alt) {
        fixture(); open_speaker(); interface_alt(ITF_NUM_AUDIO_STREAMING_SPK, (uint16_t)alt, false);
        for (unsigned level = 0; level < TU_ARRAY_SIZE(levels); ++level) {
            host_volume(levels[level]); tinyusb_control_task();
            assert(bt_sets == level + 1u && last_bt_db256 == levels[level] && read_volume() == levels[level]);
            for (unsigned phase = 0; phase < 3; ++phase) {
                uint8_t muted = phase == 1;
                assert(feature_set(AUDIO_FU_CTRL_MUTE, 0, &muted, 1));
                tinyusb_control_task();
                // Master volume is a device control, never another PCM attenuation stage.
                size_t offset = alt == 1 ? played_samples : played_float_samples;
                if (alt == 1) {
                    assert(packet(pcm, sizeof(pcm), sizeof(pcm), 1));
                    assert(played_samples == offset + TU_ARRAY_SIZE(pcm));
                    if (muted) expect_silence(played + offset, TU_ARRAY_SIZE(pcm));
                    else assert(memcmp(played + offset, pcm, sizeof(pcm)) == 0);
                } else if (alt == 2) {
                    assert(packet(floating, sizeof(floating), sizeof(floating), 2));
                    assert(played_float_samples == offset + TU_ARRAY_SIZE(floating));
                    if (muted) for (unsigned i = 0; i < TU_ARRAY_SIZE(floating); ++i) assert(played_float[offset + i] == 0);
                    else assert(memcmp(played_float + offset, floating, sizeof(floating)) == 0);
                } else {
                    const uint8_t integer32[] = {1,0,0,0, 0xff,0xff,0xff,0xff, 0,0,0,0x40, 0,0,0,0xc0};
                    const float expected[] = {0x1p-31f, -0x1p-31f, .5f, -.5f};
                    assert(packet(integer32, sizeof(integer32), sizeof(integer32), 3));
                    assert(played_float_samples == offset + TU_ARRAY_SIZE(expected));
                    for (unsigned i = 0; i < TU_ARRAY_SIZE(expected); ++i)
                        assert(played_float[offset+i] == (muted ? 0 : expected[i]));
                }
                assert(bt_sets == level + 1u && read_volume() == levels[level]);
            }
        }
    }
    uint8_t value = 2;
    assert(!feature_set(AUDIO_FU_CTRL_MUTE, 0, &value, 1));
    assert(!feature_set(AUDIO_FU_CTRL_MUTE, 1, &value, 1));
}
static void playback_ownership_edges(void) {
    fixture(); open_speaker();
    const int16_t pcm[] = {1, -1};
    for (unsigned i = 0; i < 500; ++i) {
        now += 1000;
        assert(packet(pcm, sizeof(pcm), sizeof(pcm), 1));
        service();
    }
    assert(starts == 1 && stops == 0 && pushes == 500);
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK, 0, true);
    assert(stops == 1);
    assert(packet(pcm, sizeof(pcm), sizeof(pcm), 1) && pushes == 500);
    service(); assert(stops == 1);
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK, 1, false);
    assert(packet(pcm, sizeof(pcm), sizeof(pcm), 0) && starts == 1);
    assert(packet(pcm, sizeof(pcm), sizeof(pcm), 1) && starts == 2);
}
static void idle_uses_elapsed_time(void) {
    const int16_t pcm[] = {12, -13};
    for (unsigned wrap = 0; wrap < 2; ++wrap) {
        fixture(); open_speaker();
        uint32_t start = wrap ? UINT32_MAX - 10000u : 100u;
        now = start; assert(packet(pcm, sizeof(pcm), sizeof(pcm), 1));
        for (unsigned i = 0; i < 5000; ++i) service();
        assert(stops == 0);
        const uint32_t offsets[] = {1, 777, 29000, 49999};
        for (unsigned i = 0; i < TU_ARRAY_SIZE(offsets); ++i) { now = start + offsets[i]; service(); assert(stops == 0); }
        now = start + 50000u; service(); assert(stops == 1);
        now += 1000000u; service(); assert(stops == 1);
        assert(packet(pcm, sizeof(pcm), sizeof(pcm), 1) && starts == 2);
        now += 49000; service(); assert(stops == 1);
        assert(packet(pcm, sizeof(pcm), sizeof(pcm), 1));
        now += 49000; service(); assert(stops == 1);
        now += 1000; service(); assert(stops == 2);
    }
}
static void lifecycle_stops_both_directions(void) {
    const int16_t pcm[] = {20, -20};
    for (unsigned event = 0; event < 5; ++event) {
        fixture(); open_speaker();
        interface_alt(ITF_NUM_AUDIO_STREAMING_MIC, 1, false);
        bool wanted = false;
        assert(usb_mic_take_request(&wanted) && wanted && !usb_mic_take_request(&wanted));
        assert(packet(pcm, sizeof(pcm), sizeof(pcm), 1));
        switch (event) {
            case 0: tud_suspend_cb(false); break;
            case 1: mounted = false; tud_umount_cb(); break;
            case 2: mounted = false; service(); break; // Bus reset has no unmount callback.
            case 3:
                interface_alt(ITF_NUM_AUDIO_STREAMING_SPK, 0, false);
                interface_alt(ITF_NUM_AUDIO_STREAMING_MIC, 0, true);
                break;
            case 4: tud_mount_cb(); break; // Reconfiguration cannot retain the previous stream.
        }
        assert(stops == 1 && !usb_mic_requested());
        assert(usb_mic_take_request(&wanted) && !wanted);
        assert(!usb_mic_take_request(NULL));
        assert(packet(pcm, sizeof(pcm), sizeof(pcm), 1) && pushes == 1);
        assert(tud_audio_tx_done_pre_load_cb(0, ITF_NUM_AUDIO_STREAMING_MIC, 0x82, 1));
        expect_silence(host_mic, TU_ARRAY_SIZE(host_mic));
        assert(mic_reads == 0 && mic_resets == 2);
        if (event <= 2) assert(!usb_host_active() && !media_available);
        if (event == 0) {
            tud_resume_cb();
            assert(usb_host_active() && media_available && usb_mic_requested());
            assert(packet(pcm, sizeof(pcm), sizeof(pcm), 1) && starts == 2);
        }
        service(); assert(stops == 1);
    }
}
static void microphone_fills_one_host_frame(void) {
    fixture(); mount_host();
    interface_alt(ITF_NUM_AUDIO_STREAMING_MIC, 1, false);
    for (unsigned count = 0; count <= TU_ARRAY_SIZE(mic_pcm); count += 16) {
        for (unsigned i = 0; i < TU_ARRAY_SIZE(mic_pcm); ++i) mic_pcm[i] = (int16_t)(-300 + 13 * i);
        mic_available = count;
        assert(tud_audio_tx_done_pre_load_cb(0, ITF_NUM_AUDIO_STREAMING_MIC, 0x82, 1));
        assert(memcmp(host_mic, mic_pcm, count * sizeof(*mic_pcm)) == 0);
        expect_silence(host_mic + count, TU_ARRAY_SIZE(host_mic) - count);
    }
    unsigned before = mic_reads;
    mic_available = TU_ARRAY_SIZE(mic_pcm);
    assert(tud_audio_tx_done_pre_load_cb(0, ITF_NUM_AUDIO_STREAMING_MIC, 0x82, 0));
    expect_silence(host_mic, TU_ARRAY_SIZE(host_mic));
    assert(mic_reads == before && mic_available == TU_ARRAY_SIZE(mic_pcm));
}
static void volume_notifications_coalesce_and_retry(void) {
    fixture(); mount_host();
    bt_known = true;
    const uint8_t levels[] = {0, 64, 127};
    for (unsigned i = 0; i < TU_ARRAY_SIZE(levels); ++i) {
        bt_volume = levels[i]; bt_changed = true; tinyusb_control_task();
        assert(!bt_changed && int_attempts == 0 && bt_sets == 0);
    }
    assert(read_volume() == 0);
    int_busy = true;
    service(); service(); assert(int_attempts == 2 && notifications == 0);
    bt_volume = 0; bt_changed = true; tinyusb_control_task();
    assert(read_volume() == -50 * 256 && int_attempts == 2);
    int_busy = false; service();
    assert(int_attempts == 3 && notifications == 1);
    const uint8_t expected_interrupt[] = {0, 1, 0, 2, 0, 2};
    assert(memcmp(interrupt_reply, expected_interrupt, sizeof(expected_interrupt)) == 0);
    assert(read_volume() == bt_volume_to_usb(0)); // Host GET_CUR sees the latest coalesced native level.
    for (unsigned i = 0; i < 100; ++i) service();
    assert(int_attempts == 3);
    host_volume(-30 * 256); host_volume(-20 * 256); host_volume(-10 * 256);
    assert(bt_sets == 0 && int_attempts == 3);
    tinyusb_control_task(); assert(bt_sets == 1 && last_bt_db256 == -10 * 256);
    service(); assert(notifications == 2);
    host_volume(-10 * 256); tinyusb_control_task(); service();
    assert(bt_sets == 2 && notifications == 3);
    bt_changed = true; tinyusb_control_task(); tud_suspend_cb(false); service();
    assert(notifications == 3);
    assert(usb_tasks == media_tasks && usb_tasks == debug_tasks);
}
static void usb_set_wins_readback_race(void) {
    fixture(); mount_host();
    bt_known = true; bt_volume = 127; bt_changed = true; inject_volume_set = true;
    tinyusb_control_task();
    assert(!inject_volume_set && !interrupts_disabled);
    assert(read_volume() == -20 * 256 && bt_sets == 1 && last_bt_db256 == -20 * 256);
    assert(int_attempts == 0);
    service(); assert(notifications == 1);
}
static void volume_scale_preserves_every_bt_level(void) {
    fixture(); mount_host(); bt_known = true;
    assert(usb_volume_to_bt(-12800) == 0 && usb_volume_to_bt(0) == 127);
    assert(usb_volume_to_bt(INT16_MIN) == 0 && usb_volume_to_bt(INT16_MAX) == 127);
    assert(bt_volume_to_usb(0) == -12800 && bt_volume_to_usb(127) == 0);
    assert(bt_volume_to_usb(255) == 0);
    int16_t previous = INT16_MIN;
    for (unsigned level = 0; level < 128; ++level) {
        int16_t db256 = bt_volume_to_usb((uint8_t)level);
        assert(db256 > previous && db256 >= -12800 && db256 <= 0);
        assert(usb_volume_to_bt(db256) == level);
        bt_volume = (uint8_t)level; bt_changed = true; tinyusb_control_task();
        assert(read_volume() == db256 && bt_sets == level);
        host_volume(read_volume()); tinyusb_control_task();
        assert(last_bt_db256 == db256 && usb_volume_to_bt(last_bt_db256) == level && bt_sets == level + 1);
        previous = db256;
    }
    uint8_t previous_level = 0;
    for (int db256 = -12800; db256 <= 0; ++db256) {
        uint8_t level = usb_volume_to_bt((int16_t)db256);
        assert(level >= previous_level);
        previous_level = level;
    }
    assert(usb_volume_to_bt(-6401) == 63 && usb_volume_to_bt(-6400) == 64);
    assert(usb_volume_to_bt(-6399) == 64);
    assert(bt_volume_to_usb(1) == -12699 && bt_volume_to_usb(126) == -101);
    assert(bt_volume_to_usb(64) == -6350 && usb_volume_to_bt(-6350) == 64);
}
static void exact_usb_requests_and_resolution(void) {
    fixture(); mount_host();
    audio_control_request_t request = {.bRequest = AUDIO_CS_REQ_RANGE, .bControlSelector = AUDIO_FU_CTRL_VOLUME,
        .bEntityID = UAC2_ENTITY_SPK_FEATURE_UNIT, .wLength = 8};
    assert(tud_audio_get_req_entity_cb(0, (const tusb_control_request_t *)&request));
    const uint8_t range[] = {1, 0, 0, 0xce, 0, 0, 1, 0};
    assert(control_reply_size == sizeof(range) && memcmp(control_reply, range, sizeof(range)) == 0);
    // A real SET at the default cached level must still reach an unknown peer.
    host_volume(0); tinyusb_control_task();
    assert(bt_sets == 1 && last_bt_db256 == 0);
    host_volume(-6350); tinyusb_control_task();
    assert(bt_sets == 2 && last_bt_db256 == -6350 && read_volume() == -6350);
    for (unsigned i = 0; i < 50; ++i) tinyusb_control_task();
    assert(bt_sets == 2);
}
static void confirmed_shared_readbacks_never_echo(void) {
    fixture(); mount_host();
    bt_volume = 3; bt_changed = true; tinyusb_control_task(); service();
    assert(read_volume() == 0 && bt_sets == 0 && notifications == 0);
    bt_known = true; bt_volume = 64; bt_changed = true;
    tinyusb_control_task(); service();
    assert(read_volume() == -6350 && bt_sets == 0 && notifications == 1);
    // A disconnected/new peer has no usable level; the getter may leave stale output.
    bt_known = false; bt_volume = 127; bt_changed = true;
    for (unsigned i = 0; i < 50; ++i) { tinyusb_control_task(); service(); }
    assert(read_volume() == -6350 && bt_sets == 0 && notifications == 1);
    // The shared confirmed level also follows changes while the phone owns audio.
    bt_known = true; bt_volume = 17; bt_changed = true;
    tinyusb_control_task(); service();
    assert(read_volume() == bt_volume_to_usb(17) && bt_sets == 0 && notifications == 2);
    bt_known = false; mounted = false; tud_umount_cb();
    mount_host(); tinyusb_control_task(); service();
    assert(bt_sets == 0 && read_volume() == bt_volume_to_usb(17));
    host_volume(bt_volume_to_usb(17)); tinyusb_control_task();
    assert(bt_sets == 1 && last_bt_db256 == bt_volume_to_usb(17));
}
#if AIRPODS_USB_FLOAT
static void float_packets_and_switching(void) {
    fixture();open_speaker();
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,2,false);
    float input[98];
    for(unsigned i=0;i<49;++i) {input[2*i]=.12345678f;input[2*i+1]=-.23456789f;}
    assert(sizeof(input)==392);
    assert(packet(input,sizeof(input),sizeof(input),2));
    assert(played_float_samples==98 && !memcmp(input,played_float,sizeof(input)));
    assert(!packet(input,12,12,2)); // Float stereo frames require eight-byte alignment.
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,1,false);
    assert(packet(input,8,8,2) && played_float_samples==98); // Stale-format callback cannot push.
    const int16_t integer[]={1,-2,3,-4};
    assert(packet(integer,sizeof(integer),sizeof(integer),1));
    assert(played_samples==4 && !memcmp(integer,played,sizeof(integer)));
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,2,false);
    uint8_t muted=1;assert(feature_set(AUDIO_FU_CTRL_MUTE,0,&muted,1));
    assert(packet(input,8,8,2));
    assert(played_float[98]==0 && played_float[99]==0);
}
static void pcm32_packets_and_switching(void) {
    fixture();open_speaker();
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,3,false);
    assert(traced_format==3);
    // Byte literals exercise endian decoding and sub-S16 samples independently of host layout.
    const uint8_t input[]={1,0,0,0, 0xff,0xff,0xff,0xff,
                          0,0,0,0x80, 0xff,0xff,0xff,0x7f,
                          0x56,0x34,0x12,0, 0xaa,0xcb,0xed,0xff};
    const float expected[]={0x1p-31f,-0x1p-31f,-1,1,0x123456*0x1p-31f,-0x123456*0x1p-31f};
    assert(packet(input,sizeof(input),sizeof(input),3));
    assert(played_float_samples==6 && !memcmp(expected,played_float,sizeof(expected)));
    assert(packet_format==3 && packet_frames==3 && !packet_error && starts==1);
    uint8_t largest[CFG_TUD_AUDIO_FUNC_1_FORMAT_3_EP_SZ_OUT];
    for(unsigned i=0;i<sizeof(largest);++i)largest[i]=input[i%sizeof(input)];
    assert(sizeof(largest)==392 && packet(largest,sizeof(largest),sizeof(largest),3));
    assert(packet_frames==49 && played_float_samples==104 && !packet_error);
    assert(!packet(input,12,12,3) && packet_error==1 && !rx_used);
    uint8_t oversized[sizeof(largest)+8]={0};
    assert(!packet(oversized,sizeof(oversized),sizeof(oversized),3) && packet_error==1 && !rx_used);
    short_read_limit=4;
    assert(!packet(input,8,8,3) && packet_error==2 && !rx_used);
    short_read_limit=SIZE_MAX;
    assert(packet(NULL,0,0,3) && pushes==2);
    assert(packet(input,8,8,2) && played_float_samples==104); // Old float packet is not PCM32.
    tusb_control_request_t invalid={.wIndex=ITF_NUM_AUDIO_STREAMING_SPK,.wValue=4};
    assert(!tud_audio_set_itf_cb(0,&invalid) && !tud_audio_set_itf_close_EP_cb(0,&invalid));
    assert(packet(input,8,8,4) && played_float_samples==104);
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,2,false);
    assert(traced_format==2 && packet(input,8,8,3) && played_float_samples==104);
    const float floating[]={.25f,-.125f};
    assert(packet(floating,sizeof(floating),sizeof(floating),2));
    assert(played_float[104]==floating[0] && played_float[105]==floating[1]);
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,3,false);
    tud_suspend_cb(false);
    assert(packet(input,8,8,3) && played_float_samples==106);
    tud_resume_cb();
    assert(packet(input,8,8,3) && played_float_samples==108);
    mounted=false;service();
    assert(traced_format==0 && !speaker_format);
    assert(packet(input,8,8,3) && played_float_samples==108);
    mount_host();interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,3,false);
    assert(packet(input,8,8,3) && played_float_samples==110 && traced_format==3);
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,0,true);
    assert(!speaker_format && !traced_format && !speaker_interface_open);
}
#endif
static void usb_speaker_microphone_duplex(void) {
    for(unsigned alt=1;alt<=(AIRPODS_USB_FLOAT?3u:1u);++alt) {
        fixture();open_speaker();interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,(uint16_t)alt,false);
        interface_alt(ITF_NUM_AUDIO_STREAMING_MIC,1,false);
        int16_t speaker[96];float floating[96];uint8_t integer32[384];
        for(unsigned i=0;i<96;++i) {
            speaker[i]=(int16_t)(i-48);floating[i]=(float)speaker[i]/32768.0f;
            uint32_t raw=(uint32_t)((int32_t)speaker[i]*65536);
            for(unsigned b=0;b<4;++b)integer32[4*i+b]=(uint8_t)(raw>>(8*b));
        }
        for(unsigned ms=0;ms<1000;++ms) {
            if(ms==500)interface_alt(ITF_NUM_AUDIO_STREAMING_MIC,0,true);
            if(ms==600)interface_alt(ITF_NUM_AUDIO_STREAMING_MIC,1,false);
            now=ms*1000u;
            played_samples=played_float_samples=0;
            assert(alt==1?packet(speaker,sizeof(speaker),sizeof(speaker),1):alt==2?
                          packet(floating,sizeof(floating),sizeof(floating),2):
                          packet(integer32,sizeof(integer32),sizeof(integer32),3));
            assert(alt==1?played_samples==96:played_float_samples==96);
            if(alt==3)assert(!memcmp(floating,played_float,sizeof(floating)));
            for(unsigned i=0;i<64;++i)mic_pcm[i]=(int16_t)(ms<600?1000:2000);
            mic_available=64;
            unsigned before=mic_reads;
            assert(tud_audio_tx_done_pre_load_cb(0,ITF_NUM_AUDIO_STREAMING_MIC,0x82,usb_mic_requested()?1:0));
            if(usb_mic_requested()) {
                assert(mic_reads==before+1 && !mic_available);
                for(unsigned i=0;i<64;++i)assert(host_mic[i]==(ms<600?1000:2000));
            } else {assert(mic_reads==before);expect_silence(host_mic,64);}
            service();assert(speaker_active && starts==1 && !stops);
        }
        interface_alt(ITF_NUM_AUDIO_STREAMING_MIC,0,true);
        assert(speaker_active && !usb_mic_requested() && !stops);
    }
}

static void host_volume_proof_and_hold(void) {
    fixture();mount_host();
    uint32_t session=usb_media_generation();
    assert(usb_volume_set_revision()==0);
    uint8_t invalid[]={0xff,0x7f};
    assert(!feature_set(AUDIO_FU_CTRL_VOLUME,0,invalid,2));
    assert(usb_volume_set_revision()==0);
    media_sent=1;host_volume(-6400);assert(usb_volume_set_revision()==1 && usb_volume_set_generation()==session);
    media_sent=2; // A second key transmitted after SET IRQ but before the main task.
    tinyusb_control_task();assert(bt_sets==1 && seen_set_revision==1 && seen_receipt==1 && usb_volume_set_receipt()==1);
    volume_ready=false;bt_known=true;bt_volume=80;bt_changed=true;
    tinyusb_control_task();assert(bt_changed && read_volume()==-6400);
    volume_ready=true;tinyusb_control_task();
    assert(!bt_changed && read_volume()==bt_volume_to_usb(80));
    host_volume(-9000);tud_umount_cb();mount_host();
    assert(usb_media_generation()!=session);
    tinyusb_control_task();assert(bt_sets==1); // Previous USB session cannot command the pair.
    host_volume(-8000);tinyusb_control_task();assert(bt_sets==2 && seen_set_revision==3);
}

static void bluetooth_removes_audio_preserves_control(void) {
    fixture();open_speaker();
    int16_t pcm[96]={0};
    assert(packet(pcm,sizeof(pcm),sizeof(pcm),1));
    interface_alt(ITF_NUM_AUDIO_STREAMING_MIC,1,false);
    assert(speaker_active && usb_mic_requested());
    usb_audio_set_route(true,true);
    service();assert(!disconnects && usb_audio_available() && usb_mic_requested());
    interface_alt(ITF_NUM_AUDIO_STREAMING_MIC,0,true);
    host_volume(-8000); // Queued in the old host generation.
    usb_audio_set_route(true,true);
    assert(!usb_audio_available() && !usb_host_active());
    unsigned before=pushes;
    assert(packet(pcm,sizeof(pcm),sizeof(pcm),1) && pushes==before && !rx_used);
    tusb_control_request_t stale={.wIndex=ITF_NUM_AUDIO_STREAMING_MIC,.wValue=1};
    assert(!tud_audio_set_itf_cb(0,&stale));
    assert(!tud_audio_set_itf_close_EP_cb(0,&stale));
    uint8_t volume_bytes[2]={0};
    assert(!feature_set(AUDIO_FU_CTRL_VOLUME,0,volume_bytes,2));
    assert(!tud_audio_tx_done_pre_load_cb(0,ITF_NUM_AUDIO_STREAMING_MIC,0x82,1));
    service();
    assert(disconnects==1 && !connects && !speaker_active && !usb_mic_requested());
    assert(!media_available && !usb_mounted && stops==1);
    // TinyUSB may still say mounted until the next physical bus reset.
    assert(mounted);tud_mount_cb();assert(!usb_mounted && !media_available);
    tinyusb_control_task();assert(!bt_sets && !atomic_load(&volume_notification_pending));
    now=USB_REENUMERATE_US-1;service();assert(!connects);
    now++;service();assert(connects==1 && descriptor_changes==1 && !presented_audio);
    tud_mount_cb();assert(usb_mounted && media_available && !usb_host_active());
    for(unsigned i=0;i<100;++i){now+=1000;service();}
    assert(disconnects==1 && connects==1 && !int_attempts);
    usb_audio_set_route(true,false);service();
    assert(disconnects==2 && connects==1 && !usb_audio_available());
    now+=USB_REENUMERATE_US;service();assert(connects==2 && presented_audio);
    mount_host();interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,1,false);
    assert(packet(pcm,sizeof(pcm),sizeof(pcm),1) && pushes==before+1);
    assert(usb_host_active() && starts==2);
}

static void usb_mode_coalesces_and_wraps(void) {
    fixture();mount_host();now=UINT32_MAX-100000u;
    usb_audio_set_route(true,true);service();
    usb_audio_set_route(true,false); // Cancel while detached: restore the latest intent.
    now+=USB_REENUMERATE_US-1;service();assert(!connects && !usb_audio_available());
    now++;service();assert(connects==1 && presented_audio && descriptor_changes==1);
    mount_host();service();assert(disconnects==1);
    usb_audio_set_route(true,true);usb_audio_set_route(true,false);service();
    assert(disconnects==1); // No detach for an intent that never reaches USB service.
}

static void service_cadence_preserves_open_endpoints(void) {
    fixture();
    assert(tinyusb_service_interval_us()==REALTIME_USB_IDLE_INTERVAL_US);
    mount_host();
    assert(tinyusb_service_interval_us()==REALTIME_USB_IDLE_INTERVAL_US);
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,1,false);
    assert(!speaker_active && tinyusb_service_interval_us()==REALTIME_USB_SERVICE_BUDGET_US);
    tud_suspend_cb(false);
    assert(tinyusb_service_interval_us()==REALTIME_USB_SERVICE_BUDGET_US);
    tud_resume_cb();
    assert(tinyusb_service_interval_us()==REALTIME_USB_SERVICE_BUDGET_US);
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,0,false);
    assert(tinyusb_service_interval_us()==REALTIME_USB_IDLE_INTERVAL_US);
    interface_alt(ITF_NUM_AUDIO_STREAMING_MIC,1,false);
    assert(tinyusb_service_interval_us()==REALTIME_USB_SERVICE_BUDGET_US);
    tud_suspend_cb(false);
    assert(tinyusb_service_interval_us()==REALTIME_USB_SERVICE_BUDGET_US);
    tud_resume_cb();
    interface_alt(ITF_NUM_AUDIO_STREAMING_MIC,0,false);
    assert(tinyusb_service_interval_us()==REALTIME_USB_IDLE_INTERVAL_US);
#if AIRPODS_USB_FLOAT
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,2,false);
    assert(tinyusb_service_interval_us()==REALTIME_USB_SERVICE_BUDGET_US);
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,3,false);
    assert(tinyusb_service_interval_us()==REALTIME_USB_SERVICE_BUDGET_US);
#else
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,1,false);
#endif
    mounted=false;service(); // Bus reset clears both raw alternate settings.
    assert(tinyusb_service_interval_us()==REALTIME_USB_IDLE_INTERVAL_US);
    terminal_connected=true;
    assert(tinyusb_service_interval_us()==REALTIME_USB_SERVICE_BUDGET_US);
    terminal_connected=false;
    mount_host();usb_audio_set_route(true,true);service();
    assert(usb_mode_reconnecting && tinyusb_service_interval_us()==REALTIME_USB_IDLE_INTERVAL_US);
    now+=USB_REENUMERATE_US;service();
    assert(!presented_audio && tinyusb_service_interval_us()==REALTIME_USB_IDLE_INTERVAL_US);
    usb_audio_set_route(true,false);service();
    now+=USB_REENUMERATE_US;service();mount_host();
    interface_alt(ITF_NUM_AUDIO_STREAMING_SPK,1,false);
    assert(tinyusb_service_interval_us()==REALTIME_USB_SERVICE_BUDGET_US);
}

static void cold_boot_has_controls_without_audio(void) {
    // This runs before fixture(), using the production static initializers.
    assert(!atomic_load(&usb_audio_requested) && !atomic_load(&usb_audio_present));
    assert(!usb_descriptor_audio && !usb_audio_available());
    service();mount_host();service();
    assert(!disconnects && !connects && !usb_host_active() && media_available);
    assert(usb_tasks && media_tasks && debug_tasks);
    tusb_control_request_t open={.wIndex=ITF_NUM_AUDIO_STREAMING_SPK,.wValue=1};
    assert(!tud_audio_set_itf_cb(0,&open));
    usb_audio_set_route(false,false);service();
    assert(!disconnects && !usb_audio_available());
    usb_audio_set_route(true,false);
    assert(!usb_audio_available()); // Desired audio is not yet enumerated.
    service();assert(disconnects==1 && !connects && !usb_host_active());
    now+=USB_REENUMERATE_US-1;service();assert(!connects);
    now++;service();mount_host();
    assert(connects==1 && presented_audio && usb_audio_available() && usb_host_active());
}

static void headset_absence_closes_audio_even_during_recording(void) {
    fixture();open_speaker();
    interface_alt(ITF_NUM_AUDIO_STREAMING_MIC,1,false);
    bool wanted=false;
    assert(usb_mic_take_request(&wanted) && wanted);
    const int16_t pcm[]={100,-100,200,-200};
    assert(packet(pcm,sizeof(pcm),sizeof(pcm),1) && speaker_active);
    usb_audio_set_route(true,true);service();
    assert(usb_audio_available() && usb_mic_requested() && !disconnects);
    unsigned before=pushes,resets=mic_resets;
    usb_audio_set_route(false,true);
    assert(!usb_audio_available()); // No late packet survives the presence publication.
    assert(packet(pcm,sizeof(pcm),sizeof(pcm),1) && pushes==before);
    tusb_control_request_t open={.wIndex=ITF_NUM_AUDIO_STREAMING_MIC,.wValue=1};
    assert(!tud_audio_set_itf_cb(0,&open));
    service();
    assert(disconnects==1 && !usb_mic_requested() && !speaker_active && stops==1);
    assert(usb_mic_take_request(&wanted) && !wanted && mic_resets>resets);
    assert(!speaker_interface_open && !mic_interface_open && speaker_format==0);
    now+=USB_REENUMERATE_US;service();mount_host();
    assert(!presented_audio && !usb_host_active() && media_available);
    // Returning AirPods cannot steal a retained Bluetooth-input session.
    usb_audio_set_route(true,true);service();
    assert(disconnects==1 && !usb_audio_available());
    usb_audio_set_route(true,false);service();
    now+=USB_REENUMERATE_US;service();mount_host();
    assert(presented_audio && usb_audio_available() && !usb_mic_requested());
    assert(!speaker_interface_open && !mic_interface_open); // No old host stream resumes itself.
}

static void presence_changes_coalesce_without_stale_audio(void) {
    fixture();mount_host();now=UINT32_MAX-100000u;
    usb_audio_set_route(false,false);service();
    usb_audio_set_route(true,false); // Headset returns during the detach interval.
    now+=USB_REENUMERATE_US;service();
    assert(connects==1 && presented_audio && usb_audio_available());
    mount_host();
    usb_audio_set_route(false,false);service();
    usb_audio_set_route(true,false);
    usb_audio_set_route(false,false); // Latest absence wins even if audio was briefly requested.
    now+=USB_REENUMERATE_US;service();mount_host();
    assert(connects==2 && !presented_audio && !usb_audio_available());
    usb_audio_set_route(false,true);service();
    assert(disconnects==2 && !usb_host_active() && media_available);
}

int main(void) {
    cold_boot_has_controls_without_audio();
    headset_absence_closes_audio_even_during_recording();
    presence_changes_coalesce_without_stale_audio();
    service_cadence_preserves_open_endpoints();
    bluetooth_removes_audio_preserves_control();
    usb_mode_coalesces_and_wraps();
    host_volume_proof_and_hold();
    usb_speaker_microphone_duplex();
    current_packet_stereo();
    malformed_packets_are_discarded();
    mute_preserves_volume_and_stereo();
    playback_ownership_edges();
    idle_uses_elapsed_time();
    lifecycle_stops_both_directions();
    microphone_fills_one_host_frame();
    volume_notifications_coalesce_and_retry();
    usb_set_wins_readback_race();
    volume_scale_preserves_every_bt_level();
    exact_usb_requests_and_resolution();
    confirmed_shared_readbacks_never_echo();
#if AIRPODS_USB_FLOAT
    float_packets_and_switching();
    pcm32_packets_and_switching();
#endif
    puts("UAC: control-only boot, headset presence, S16/float/PCM32, duplex, route priorities and re-enumeration wrap passed");
}
