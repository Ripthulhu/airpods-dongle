#include "pico/cyw43_arch.h"
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

#include <stdio.h>
#include <stdatomic.h>
#include "pico/stdlib.h"


#include "btstack_event.h"

#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
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

// Scratch-X holds encoder PCM; give the decoder worker its own main-RAM stack.
static uint32_t codec_stack[4096] __attribute__((aligned(8)));
static atomic_uint codec_core_state;

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
        async_context_acquire_lock_blocking(ctx);
        bt_hci_phone_pairing_start();
        async_context_release_lock(ctx);
    }

    if (atomic_exchange(&btn_single_pending, false)) {
        async_context_acquire_lock_blocking(ctx);
        if (! get_a2dp_connected_flag()) {
            a2dp_source_reconnect();
        } else {
            increase_vol_by_key();
        }
        async_context_release_lock(ctx);
    }

    if (atomic_exchange(&btn_double_pending, false)) {
        async_context_acquire_lock_blocking(ctx);
        bool allow = get_allow_switch_slot();
        if (!allow) decrease_vol_by_key();

        if (allow) {
            uint8_t currect_slot = read_uint8_last_flash();
            uint8_t next_slot = currect_slot == 1 ? 2 : 1;
            if (write_uint8_last_flash(next_slot)) {
                set_led_mode_off();
                if (next_slot == 2) set_led_mode_triple_blink();
                else set_led_mode_double_blink();
                get_link_keys();
                avdtp_source_establish_stream();
            } else {
                printf("Slot selection could not be saved\n");
            }
        }
        async_context_release_lock(ctx);
    }

    if (atomic_exchange(&btn_long_pending, false)) {
        async_context_acquire_lock_blocking(ctx);
        avdtp_disconnect_and_scan();
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
    *(bool *)value = get_bootsel_button();
}

// BOOTSEL temporarily disconnects QSPI; pause core1 before sampling it.
void check_bootsel_state(void)
{
    bool raw;
    if (flash_safe_execute(read_bootsel, &raw, 1) != PICO_OK) return;
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
    tinyusb_task();
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

    if (!codec_core_start()) {
        printf("Codec core flash coordination unavailable\n");
        return 1;
    }

    // Distinguish watchdog resets from clean power-on boots in the debug log.
    if (watchdog_caused_reboot()) {
        printf("!!! BOOT REASON: WATCHDOG RESET (previous run hung)\n");
    } else {
        printf("boot reason: normal power-on/reset\n");
    }

    tinyusb_main();

    audio_slot_queue_init();

    printf("init ctw43.\n");

    // initialize CYW43 driver
    if (cyw43_arch_init()) {
        printf("cyw43_arch_init() failed.\n");
        return -1;
    }

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

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

    if (btstack_main(0, NULL) != 0) {
        printf("Bluetooth audio services unavailable\n");
        return 1;
    }
    sleep_ms(200);

    static repeating_timer_t usb_timer;
    // NEGATIVE interval (hard period): fire every 500us measured from the
    // previous START, so tinyusb_task() runs at a steady cadence regardless of
    // how long each call takes. USB audio is isochronous — the OUT endpoint
    // must be serviced on a fixed rhythm or samples drift/underrun.
    if (!add_repeating_timer_us(-500, usb_timer_callback, NULL, &usb_timer)) {
        printf("USB service timer unavailable\n");
        return 1;
    }

    // Enable watchdog — auto-resets if the main loop stops feeding it.
    // Allow bounded codec initialization and flash operations to finish.
    watchdog_enable(8000, true);

    mic_decoder_lifecycle_t decoder = {0};
    uint32_t button_sample_ms = to_ms_since_boot(get_absolute_time());
    while (1) {
        watchdog_update();  // feed the watchdog
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if ((uint32_t)(now_ms - button_sample_ms) >= 20u) {
            button_sample_ms = now_ms;
            check_bootsel_state();
        }
        process_button_actions();

        bool mic_wanted;
        async_context_t *ctx = cyw43_arch_async_context();
        async_context_acquire_lock_blocking(ctx);
        bt_hci_phone_poll();
        if (usb_mic_take_request(&mic_wanted)) {
            // A fresh open also clears an earlier AACP failure before retrying.
            aacp_mic_stop();
            mic_decoder_lifecycle_request(&decoder, mic_wanted, aacp_mic_dec_ready());
        }
        // The idle microphone decoder is too large to retain during a phone
        // session. Core 1 detaches it before core 0 frees its allocations.
        mic_decoder_lifecycle_release(&decoder,
            phone_sink_connected() && !usb_mic_requested() && !aacp_mic_active());
        if ((usb_mic_requested() || aacp_mic_active() || get_usb_streaming()) && phone_sink_connected())
            phone_sink_yield();
        if (!decoder.releasing) phone_sink_poll();
        ble_scan_set_paused(BLE_SCAN_PAUSE_USB_SUSPEND,
                            !usb_host_active() && !phone_sink_connected());
        ble_scan_set_paused(BLE_SCAN_PAUSE_MIC, usb_mic_requested() || aacp_mic_active());
        // Keep decoder allocation in thread context and serialize malloc with
        // Bluetooth encoder callbacks. The USB service timer remains active.
        mic_decoder_prepare_t prepared = mic_decoder_lifecycle_prepare(&decoder,
            usb_mic_requested(), phone_sink_decoder_released());
        if (prepared == MIC_DECODER_FAILED) printf("Microphone decoder unavailable; reopen recording to retry\n");
        aacp_mic_dec_poll();
        tinyusb_control_task();
        if (prepared == MIC_DECODER_READY && usb_mic_requested()) {
            ble_scan_set_paused(BLE_SCAN_PAUSE_MIC, true);
            aacp_mic_start();
        }
        aacp_poll();
        dongle_control_poll();
        dongle_rpc_poll();
        ble_diagnostics_task();
        async_context_release_lock(ctx);
        sleep_ms(1);
    }

}
