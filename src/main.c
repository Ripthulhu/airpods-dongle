#include "pico/cyw43_arch.h"
#include "pico/cyw43_driver.h"
#include "pico/stdlib.h"
#include "btstack_run_loop.h"

#include "btstack/btstack_avdtp_source.h"
#include "btstack/btstack_hci.h"
#include "btstack/btstack_ble_scan.h"
#include "btstack/btstack_aacp.h"
#include "btstack/aacp_mic_dec.h"
#include "btstack/mic_decoder_lifecycle.h"
#include "btstack/phone_sink.h"
#include "btstack/phone_worker.h"
#include "btstack/phone_benchmark.h"
#include "btstack/phone_control.h"
#include "btstack/phone_call.h"
#include "btstack/dongle_finder.h"
#include "tinyusb/dongle_features.h"

#include <stdio.h>
#include <stdatomic.h>
#include "pico/stdlib.h"


#include "btstack_event.h"

#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/xip.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/timer.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"

#include "tinyusb/uac.h"
#include "tinyusb/debug_cdc.h"
#include "tinyusb/dongle_control.h"
#include "tinyusb/dongle_rpc.h"
#include "pico_w_led.h"
#include "slot_storage.h"
#include "button_gesture.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"
#include "radio_clock.h"
#include "realtime_metrics.h"
#include "runtime_watch.h"

// Preserve fault status for the watchdog reboot; do not attempt to resume a fault.
void __attribute__((noreturn)) isr_hardfault(void) {
    runtime_watch_fault(watchdog_hw->scratch,
        *(volatile uint32_t *)0xe000ed28u,*(volatile uint32_t *)0xe000ed2cu);
    if(get_core_num())hw_set_bits(&watchdog_hw->scratch[0],RUNTIME_WATCH_CORE1);
    for (;;) __asm volatile("wfe");
}

// Scratch-X holds encoder PCM; give the decoder worker its own main-RAM stack.
static uint32_t codec_stack[4096] __attribute__((aligned(8)));
static atomic_uint codec_core_state;
static realtime_metric_t usb_service_metric, bluetooth_main_metric;

static void realtime_metrics_print(void) {
    // USB can preempt the main loop; take a coherent short snapshot.
    uint32_t irq = save_and_disable_interrupts();
    realtime_metric_t usb = usb_service_metric, bluetooth = bluetooth_main_metric;
    uint32_t hits = xip_ctrl_hw->ctr_hit, accesses = xip_ctrl_hw->ctr_acc;
    xip_ctrl_hw->ctr_hit = 0;
    xip_ctrl_hw->ctr_acc = 0;
    restore_interrupts(irq);
    uint32_t system_hz = clock_get_hz(clk_sys);
    uint32_t divider256 = radio_clock_divider256(system_hz);
    printf("[TIMING] cpu_hz=%lu spi_hz=%lu usb_calls=%lu usb_max_us=%lu usb_gap_max_us=%lu usb_over500=%lu bt_calls=%lu bt_max_us=%lu bt_gap_max_us=%lu phone_decode_max_us=%lu\n",
           (unsigned long)system_hz, (unsigned long)((uint64_t)system_hz * 128u / divider256),
           (unsigned long)usb.count, (unsigned long)usb.max_us, (unsigned long)usb.max_gap_us,
           (unsigned long)usb.overruns, (unsigned long)bluetooth.count,
           (unsigned long)bluetooth.max_us, (unsigned long)bluetooth.max_gap_us,
           (unsigned long)phone_worker_max_decode_us());
    printf("[ENCODER] cancelled=%lu\n", (unsigned long)dongle_playback_encode_cancelled());
    printf("[XIP] hits=%lu accesses=%lu (read then cleared; counters saturate)\n",
           (unsigned long)hits, (unsigned long)accesses);
    bt_hci_credit_diagnostics_t credits;
    bool known = bt_hci_credit_diagnostics(&credits);
    printf("[CREDITS] known=%u raw_acl=%lu acl_len=%lu limit=%lu free=%lu outstanding=%lu reserved=%lu airpods_events=%lu airpods_packets=%lu airpods_gap_ms=%lu phone_events=%lu phone_packets=%lu phone_gap_ms=%lu\n",
        known ? 1u : 0u, (unsigned long)credits.raw_acl_count, (unsigned long)credits.raw_acl_length,
        (unsigned long)credits.effective_acl_limit, (unsigned long)credits.free_acl_slots,
        (unsigned long)credits.outstanding_acl_packets, (unsigned long)credits.packet_buffer_reserved,
        (unsigned long)credits.airpods.events, (unsigned long)credits.airpods.packets,
        (unsigned long)credits.airpods.max_active_gap_ms, (unsigned long)credits.phone.events,
        (unsigned long)credits.phone.packets, (unsigned long)credits.phone.max_active_gap_ms);
    printf("[LINKS] airpods_handle=0x%04x airpods_local_role=%u phone_handle=0x%04x phone_local_role=%u\n",
        (unsigned)credits.airpods_handle, (unsigned)credits.airpods_local_role,
        (unsigned)credits.phone_handle, (unsigned)credits.phone_local_role);
    printf("[FLUSH] desired_ms=%u confirmed_ms=%u pending=%u failed=%u status=%u\n",
        credits.media_flush_desired_ms,credits.media_flush_confirmed_ms,
        credits.media_flush_pending,credits.media_flush_failed,credits.media_flush_status);
    printf("[PACKET] requested=0x%04x normal=0x%04x desired=0x%04x confirmed=0x%04x pending=%u failed=%u status=%u\n",
        credits.media_packet_requested,credits.media_packet_normal,
        credits.media_packet_desired,credits.media_packet_confirmed,
        credits.media_packet_pending,credits.media_packet_failed,credits.media_packet_status);
}

void aacp_mic_dec_wake_worker(void) {
    __sev();
}

void phone_worker_wake(void) { __sev(); }

static void codec_core_entry(void) {
    bool safe = flash_safe_execute_core_init();
    atomic_store_explicit(&codec_core_state, safe ? 1u : 2u, memory_order_release);
    __sev();
    for (;;) {
        if (!safe || (!phone_worker_step() && !aacp_mic_dec_worker_step())) __wfe();
    }
}

static bool codec_core_start(void) {
    if (!flash_safe_execute_core_init()) return false;
    multicore_launch_core1_with_stack(codec_core_entry, codec_stack, sizeof(codec_stack));
    absolute_time_t deadline = make_timeout_time_ms(1000);
    unsigned state;
    while ((state = atomic_load_explicit(&codec_core_state, memory_order_acquire)) == 0 &&
           !time_reached(deadline)) tight_loop_contents();
    return state == 1;
}


static bool __no_inline_not_in_flash_func(get_bootsel_button)() {
    const uint CS_PIN_INDEX = 1;

    // Must disable interrupts, as interrupt handlers may be in flash, and we
    // are about to temporarily disable flash access!
    uint32_t flags = save_and_disable_interrupts();

    // Set chip select to Hi-Z
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Note we can't call into any sleep functions in flash right now
    for (volatile int i = 0; i < 1000; ++i);

    // The HI GPIO registers in SIO can observe and control the 6 QSPI pins.
    // Note the button pulls the pin *low* when pressed.
#if PICO_RP2040
    #define CS_BIT (1u << 1)
#else
    #define CS_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif
    bool button_state = !(sio_hw->gpio_hi_in & CS_BIT);

    // Need to restore the state of chip select, else we are going to have a
    // bad time when we return to code in flash!
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);

    return button_state;
}

// Keep Bluetooth actions in thread context with its async lock held.
static atomic_bool btn_single_pending;
static atomic_bool btn_double_pending;
static atomic_bool btn_long_pending;
static atomic_bool btn_phone_pending;

void on_single_press(void){
    atomic_store(&btn_single_pending, true);
}

void on_double_press(void){
    atomic_store(&btn_double_pending, true);
}

void on_long_press(void){
    atomic_store(&btn_long_pending, true);
}

// Runs in main-loop (thread) context.
static void process_button_actions(void){
    async_context_t *ctx = cyw43_arch_async_context();

    if (atomic_exchange(&btn_phone_pending, false)) {
        runtime_watch_set(WATCH_BUTTON_ACTION_LOCK);
        async_context_acquire_lock_blocking(ctx);
        runtime_watch_set(WATCH_BUTTON_ACTION);
        bt_hci_phone_pairing_start();
        runtime_watch_set(WATCH_BUTTON_ACTION_UNLOCK);
        async_context_release_lock(ctx);
    }

    if (atomic_exchange(&btn_single_pending, false)) {
        runtime_watch_set(WATCH_BUTTON_ACTION_LOCK);
        async_context_acquire_lock_blocking(ctx);
        runtime_watch_set(WATCH_BUTTON_ACTION);
        const uint8_t *peer = (const uint8_t *)get_device_addr();
        if (peer && (peer[0] || peer[1] || peer[2] || peer[3] || peer[4] || peer[5])) {
            if (aacp_takeover_request(peer))
                a2dp_source_reconnect();
        } else {
            gap_start_scanning();
        }
        runtime_watch_set(WATCH_BUTTON_ACTION_UNLOCK);
        async_context_release_lock(ctx);
    }

    if (atomic_exchange(&btn_double_pending, false)) {
        runtime_watch_set(WATCH_BUTTON_ACTION_LOCK);
        async_context_acquire_lock_blocking(ctx);
        runtime_watch_set(WATCH_BUTTON_ACTION);
        (void)dongle_playback_game_toggle();
        runtime_watch_set(WATCH_BUTTON_ACTION_UNLOCK);
        async_context_release_lock(ctx);
    }

    if (atomic_exchange(&btn_long_pending, false)) {
        runtime_watch_set(WATCH_BUTTON_ACTION_LOCK);
        async_context_acquire_lock_blocking(ctx);
        runtime_watch_set(WATCH_BUTTON_ACTION);
        avdtp_disconnect_and_scan();
        runtime_watch_set(WATCH_BUTTON_ACTION_UNLOCK);
        async_context_release_lock(ctx);
    }
}

// ---------- parameters ----------
#define DEBOUNCE_US          5000          // 5 ms
#define SHORT_PRESS_MIN_US  100000         // 100 ms
#define LONG_PRESS_MIN_US  1000000         // 1  s

// ---------- state ----------
static bool     debounced_state  = false;   // last stable level
static uint64_t last_edge_us     = 0;       // last time level changed
static uint64_t press_start_us   = 0;       // when current press began
static button_gesture_t button_gesture;

static void __no_inline_not_in_flash_func(read_bootsel)(void *value) {
    runtime_watch_set(WATCH_BOOTSEL_READ);
    *(bool *)value = get_bootsel_button();
    runtime_watch_set(WATCH_BOOTSEL_EXIT);
}

// BOOTSEL temporarily disconnects QSPI; pause core1 before sampling it.
void check_bootsel_state(void)
{
    bool raw;
    runtime_watch_set(WATCH_BOOTSEL_ENTER);
    if (flash_safe_execute(read_bootsel, &raw, 1) != PICO_OK) return;
    runtime_watch_set(WATCH_BUTTON);
    uint64_t now = time_us_64();

    // --- debounce ---
    if (raw != debounced_state) {
        if (now - last_edge_us >= DEBOUNCE_US) {
            debounced_state = raw;
            last_edge_us = now;

            if (debounced_state) {              // -------- press --------
                press_start_us = now;
            } else {                            // -------- release ------
                uint64_t press_len = now - press_start_us;

                // classify type of this press
                if (press_len >= 3000000u) {
                    atomic_store(&btn_phone_pending, true);
                    button_gesture.pending = false;
                } else if (press_len >= LONG_PRESS_MIN_US) {
                    on_long_press();
                    button_gesture.pending = false;
                } else if (press_len >= SHORT_PRESS_MIN_US) {
                    button_gesture_event_t event = button_gesture_release(&button_gesture, now);
                    if (event == BUTTON_GESTURE_SINGLE) on_single_press();
                    else if (event == BUTTON_GESTURE_DOUBLE) on_double_press();
                }
            }
        }
    }

    // timeout: single tap
    if (button_gesture_poll(&button_gesture, now) == BUTTON_GESTURE_SINGLE) on_single_press();
}


bool usb_timer_callback(repeating_timer_t *rt){
    hw_set_bits(&watchdog_hw->scratch[0],RUNTIME_WATCH_USB);
    uint32_t started = time_us_32();
    tinyusb_task();
    // Apply newly opened endpoints before the next START-relative timer deadline.
    rt->delay_us = -(int64_t)tinyusb_service_interval_us();
    realtime_metric_record(&usb_service_metric, started, time_us_32(), REALTIME_USB_SERVICE_BUDGET_US);
    hw_clear_bits(&watchdog_hw->scratch[0],RUNTIME_WATCH_USB);
    return true;
}

static void ble_diagnostics_task(void) {
    static uint32_t last_ms;
    static ble_scan_stats_t current;
    static uint32_t last_accepted, last_malformed, last_evicted, last_pauses = UINT32_MAX;
    static bool last_requested;
    uint32_t now = btstack_run_loop_get_time_ms();
    if ((uint32_t)(now - last_ms) < 5000u) return;
    last_ms = now;
    ble_scan_get_stats(&current);
    if (current.accepted == last_accepted && current.malformed == last_malformed &&
        current.evicted == last_evicted && current.pause_reasons == last_pauses &&
        current.scan_requested == last_requested) return;
    printf("[BLE] requested=%u paused=%lu cached=%u reports=%lu malformed=%lu evicted=%lu\n",
           current.scan_requested, (unsigned long)current.pause_reasons, current.cache_count,
           (unsigned long)current.accepted, (unsigned long)current.malformed,
           (unsigned long)current.evicted);
    last_accepted = current.accepted;
    last_malformed = current.malformed;
    last_evicted = current.evicted;
    last_pauses = current.pause_reasons;
    last_requested = current.scan_requested;
}

static bool benchmark_idle(void) {
    tinyusb_task();
    sleep_ms(1);
    return true;
}

// Manual volume history: callbacks only store words; main drains one short row
// after earlier CDC output, preserving the existing bounded USB drain.
enum { VOLUME_CONSOLE_OFF, VOLUME_CONSOLE_ARM_REPLY, VOLUME_CONSOLE_ARMED,
       VOLUME_CONSOLE_DUMP_HEADER, VOLUME_CONSOLE_DUMP_ROWS };
typedef struct {
    uint32_t terminal_generation, started_ms;
    uint8_t mode;
} volume_trace_console_t;

static void volume_trace_console_poll(volume_trace_console_t *state, uint32_t now_ms) {
    uint8_t request = debug_cdc_take_volume_trace_request();
    if (!request && state->mode == VOLUME_CONSOLE_OFF) return;
    uint32_t generation = debug_cdc_terminal_generation();
    if (!debug_cdc_terminal_connected() ||
        (state->mode != VOLUME_CONSOLE_OFF && state->terminal_generation != generation)) {
        (void)bt_volume_trace_control(false);
        state->mode = VOLUME_CONSOLE_OFF;
        return;
    }
    if (request == DEBUG_CDC_VOLUME_TRACE_ARM) {
        (void)bt_volume_trace_control(true);
        state->terminal_generation = generation;
        state->started_ms = now_ms;
        state->mode = VOLUME_CONSOLE_ARM_REPLY;
    } else if (request == DEBUG_CDC_VOLUME_TRACE_DUMP) {
        (void)bt_volume_trace_control(false);
        state->terminal_generation = generation;
        state->mode = VOLUME_CONSOLE_DUMP_HEADER;
    } else if ((state->mode == VOLUME_CONSOLE_ARMED || state->mode == VOLUME_CONSOLE_ARM_REPLY) &&
               (uint32_t)(now_ms - state->started_ms) >= 60000u) {
        (void)bt_volume_trace_control(false);
        state->mode = VOLUME_CONSOLE_OFF;
    }
    if (state->mode == VOLUME_CONSOLE_OFF || state->mode == VOLUME_CONSOLE_ARMED ||
        debug_cdc_output_pending()) return;
    if (state->mode == VOLUME_CONSOLE_ARM_REPLY) {
        printf("[VOLTRACE] armed 60s v=1\n");
        state->mode = VOLUME_CONSOLE_ARMED;
    } else if (state->mode == VOLUME_CONSOLE_DUMP_HEADER) {
        printf("[VOLTRACE] v=1 total=%lu\n", (unsigned long)bt_volume_trace_control(false));
        state->mode = VOLUME_CONSOLE_DUMP_ROWS;
    } else {
        uint32_t words[4];
        if (bt_volume_trace_take(words))
            printf("[VOLTRACE] %08lx %08lx %08lx %08lx\n", (unsigned long)words[0],
                (unsigned long)words[1], (unsigned long)words[2], (unsigned long)words[3]);
        else {
            printf("[VOLTRACE] end\n");
            state->mode = VOLUME_CONSOLE_OFF;
        }
    }
}

// Runtime BOOTSEL is deliberately separate from the physical button gestures.
typedef struct {
    uint32_t requested_ms;
    uint32_t terminal_generation;
    bool pending;
} bootsel_request_t;

static bool bootsel_busy(void) {
    return get_usb_streaming() || usb_mic_requested() || phone_sink_active() || phone_sink_start_pending() ||
        phone_call_active() || aacp_mic_requested() || aacp_mic_active() ||
        dongle_playback_transitioning();
}

// Called with the Bluetooth context locked. No waits or TinyUSB calls here:
// the USB IRQ drains the acceptance reply while normal main-loop work continues.
static bool bootsel_request_poll(bootsel_request_t *state, uint32_t now_ms) {
    bool requested = debug_cdc_take_bootsel_request();
    if (!state->pending && !requested) return false;
    if (!debug_cdc_terminal_connected()) {
        state->pending = false;
        return false;
    }
    uint32_t generation = debug_cdc_terminal_generation();
    if (state->pending && state->terminal_generation != generation) {
        state->pending = false;
        return false;
    }
    if (!state->pending) {
        state->pending = true;
        state->requested_ms = now_ms;
        state->terminal_generation = generation;
        printf("[BOOTSEL] ready\n");
        return false;
    }
    uint32_t elapsed = now_ms - state->requested_ms;
    if (elapsed < 20u || (elapsed < 100u && debug_cdc_output_pending())) return false;
    state->pending = false;
    return true;
}

// Outside the Bluetooth lock and USB IRQ. The explicit update interrupts audio;
// the SDK resets the whole chip, including core 1, before entering the boot ROM.
static void bootsel_enter_if_current(uint32_t generation) {
    uint32_t irq = save_and_disable_interrupts();
    bool connected = debug_cdc_terminal_connected() &&
        debug_cdc_terminal_generation() == generation;
    if (connected) reset_usb_boot(0, 0);
    restore_interrupts(irq);
}

int main() {

    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(100);
    set_sys_clock_khz(250000, true);

    // stdio: UART stdout is disabled at the CMake level (pico_enable_stdio_uart 0)
    // because its blocking writes from IRQs / the BTstack run loop stalled audio.
    // All printf() goes into the CDC ring only (non-blocking producer).
    stdio_init_all();
    debug_cdc_init();   // route printf() into the CDC ring from the very start
                        // (drained over USB once tusb is up and a terminal opens)

    if(watchdog_enable_caused_reboot() &&
       (watchdog_hw->scratch[0]&0xffff0000u)==RUNTIME_WATCH_MAGIC) {
        printf("[HANG] state=%08lx checkpoint_us=%lu cfsr=%08lx hfsr=%08lx\n",
            (unsigned long)watchdog_hw->scratch[0],(unsigned long)watchdog_hw->scratch[1],
            (unsigned long)watchdog_hw->scratch[2],(unsigned long)watchdog_hw->scratch[3]);
    }
    // Scratch 4..7 belong to SDK reboot/BOOTSEL handling.
    for(unsigned i=0;i<4;++i)watchdog_hw->scratch[i]=0;
    runtime_watch_set(WATCH_BOOT);

    if (!codec_core_start()) {
        printf("Codec core flash coordination unavailable\n");
        return 1;
    }

    // Distinguish watchdog resets from clean power-on boots in the debug log.
    if (watchdog_enable_caused_reboot()) {
        printf("!!! BOOT REASON: WATCHDOG RESET (previous run hung)\n");
    } else if (watchdog_caused_reboot()) {
        printf("boot reason: requested watchdog reboot\n");
    } else {
        printf("boot reason: normal power-on/reset\n");
    }

    // USB may deliver PCM while the silent startup benchmark services its tasks.
    audio_slot_queue_init();
    tinyusb_main();

    phone_benchmark_result_t phone_benchmark;
    bool lhdc_ready = phone_benchmark_lhdc(time_us_32, benchmark_idle, &phone_benchmark);
    printf("[PHONE] LHDC benchmark: enabled=%u frames=%lu max_us=%lu cleanup=%u\n",
           lhdc_ready, (unsigned long)phone_benchmark.frames,
           (unsigned long)phone_benchmark.max_decode_us, phone_benchmark.cleanup);
    if (!phone_benchmark.cleanup) {
        printf("Phone decoder did not detach; Bluetooth startup cancelled\n");
        return 1;
    }

    printf("init ctw43.\n");

    // initialize CYW43 driver
    // Keep radio SPI independent of the inherited CPU overclock.
    uint32_t radio_divider256 = radio_clock_divider256(clock_get_hz(clk_sys));
    cyw43_set_pio_clkdiv_int_frac8(radio_divider256 >> 8, (uint8_t)radio_divider256);
    if (cyw43_arch_init()) {
        printf("cyw43_arch_init() failed.\n");
        return -1;
    }
    set_led_mode_off();

    uint8_t currect_slot = read_uint8_last_flash();

    printf("init slot is %d\n", currect_slot);

    if (currect_slot != 0x1 && currect_slot != 0x2){
        printf("Doesn't has slot, set the slot to 1\n");
        if (!write_uint8_last_flash(0x1)) {
            printf("Slot storage unavailable or unsupported\n");
            return 1;
        }
        currect_slot = read_uint8_last_flash();
    }

    if (btstack_main(0, NULL) != 0) {
        printf("Bluetooth audio services unavailable\n");
        return 1;
    }
    async_context_t *startup_context = cyw43_arch_async_context();
    async_context_acquire_lock_blocking(startup_context);
    bool controls_ready = phone_control_init();
    async_context_release_lock(startup_context);
    if (!controls_ready) {
        printf("Phone controls unavailable\n");
        return 1;
    }
    sleep_ms(200);

    static repeating_timer_t usb_timer;
    // NEGATIVE interval (hard period): fire every 500us measured from the
    // previous START, so tinyusb_task() runs at a steady cadence regardless of
    // how long each call takes. USB audio is isochronous — the OUT endpoint
    // must be serviced on a fixed rhythm or samples drift/underrun.
    if (!add_repeating_timer_us(-(int64_t)REALTIME_USB_SERVICE_BUDGET_US, usb_timer_callback, NULL, &usb_timer)) {
        printf("USB service timer unavailable\n");
        return 1;
    }

    // Enable watchdog — auto-resets if the main loop stops feeding it.
    // Allow bounded codec initialization and flash operations to finish.
    watchdog_enable(8000, true);

    mic_decoder_lifecycle_t decoder = {0};
    bootsel_request_t bootsel = {0};
    volume_trace_console_t volume_console = {0};
    uint32_t button_sample_ms = to_ms_since_boot(get_absolute_time());
    while (1) {
        runtime_watch_set(WATCH_LOOP);
        if(!(watchdog_hw->scratch[0]&RUNTIME_WATCH_FAULT))watchdog_update();
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        runtime_watch_set(WATCH_BUTTON_ACTIONS);
        process_button_actions();

        bool mic_wanted;
        async_context_t *ctx = cyw43_arch_async_context();
        runtime_watch_set(WATCH_LOCK);
        async_context_acquire_lock_blocking(ctx);
        uint32_t bluetooth_started = time_us_32();
        dongle_playback_encode_complete();
        // Serialize SDK flash lockout with Bluetooth's interrupt-driven TLV writes.
        if ((uint32_t)(now_ms - button_sample_ms) >= 20u) {
            runtime_watch_set(WATCH_BUTTON);
            button_sample_ms = now_ms;
            check_bootsel_state();
        }
        runtime_watch_set(WATCH_HCI);
        bt_hci_phone_poll();
        phone_call_poll();
        usb_audio_set_route(get_a2dp_connected_flag(), phone_sink_usb_exclusion_requested() ||
                                                     phone_call_usb_exclusion_requested());
        runtime_watch_set(WATCH_MIC);
        if (aacp_mic_update_owner(usb_mic_take_request(&mic_wanted))) {
            // Preserve coalesced close/open revisions even when the final request stays true.
            mic_decoder_lifecycle_request(&decoder, aacp_mic_requested(), aacp_mic_dec_ready());
        }
        // The idle microphone decoder is too large to retain during a phone
        // session. Core 1 detaches it before core 0 frees its allocations.
        mic_decoder_lifecycle_release(&decoder,
            phone_sink_connected() && !aacp_mic_requested() && !aacp_mic_active());
        if (aacp_mic_requested() && phone_sink_connected())
            phone_sink_yield();
        runtime_watch_set(WATCH_PHONE);
        if (!decoder.releasing) phone_sink_poll();
        ble_scan_set_paused(BLE_SCAN_PAUSE_USB_SUSPEND,
                            !usb_host_active() && !phone_sink_connected() && !dongle_finder_active());
        ble_scan_set_paused(BLE_SCAN_PAUSE_MIC, aacp_mic_requested() || aacp_mic_active());
        // Keep decoder allocation in thread context and serialize malloc with
        // Bluetooth encoder callbacks. The USB service timer remains active.
        mic_decoder_prepare_t prepared = mic_decoder_lifecycle_prepare(&decoder,
            aacp_mic_requested(), phone_sink_decoder_released());
        if (prepared == MIC_DECODER_FAILED) printf("Microphone decoder unavailable; reopen recording to retry\n");
        aacp_mic_dec_poll();
        runtime_watch_set(WATCH_CONTROL);
        tinyusb_control_task();
        bt_volume_poll();
        if (prepared == MIC_DECODER_READY && aacp_mic_requested()) {
            ble_scan_set_paused(BLE_SCAN_PAUSE_MIC, true);
            aacp_mic_start();
        }
        runtime_watch_set(WATCH_AACP);
        aacp_poll();
        dongle_control_poll();
        runtime_watch_set(WATCH_RPC);
        dongle_rpc_poll();
        runtime_watch_set(WATCH_FEATURES);
        dongle_features_poll();
        phone_control_poll();
        if (debug_cdc_take_timing_request()) { ble_diagnostics_task();realtime_metrics_print();phone_call_report();bt_volume_report();aacp_routing_report(); }
        volume_trace_console_poll(&volume_console, now_ms);
        bool enter_bootsel = bootsel_request_poll(&bootsel, now_ms);
        // Snapshot under the Bluetooth lock; USB ingress keeps running in its IRQ.
        bool fully_idle = !check_is_streaming() && !bootsel_busy() &&
            !decoder.start_pending && !decoder.releasing && !debug_cdc_terminal_connected();
        realtime_metric_record(&bluetooth_main_metric, bluetooth_started, time_us_32(), UINT32_MAX);
        runtime_watch_set(WATCH_UNLOCK);
        async_context_release_lock(ctx);
        if (enter_bootsel) bootsel_enter_if_current(bootsel.terminal_generation);
        dongle_playback_encode_run();
        runtime_watch_set(WATCH_IDLE);
        uint32_t idle_us = realtime_loop_idle_us(bluetooth_started, time_us_32(), fully_idle);
        if (idle_us) sleep_us(idle_us);
    }

}
