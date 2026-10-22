#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico_w_led.h"



static uint16_t repeating_time = 500;
static uint16_t repeating_time_short = 200;
static bool is_repeating = false;
static bool led_static_state = false;
static bool led_on = true;
static bool led_short = true;

static void led_do_work(async_context_t *context, async_at_time_worker_t *worker);
async_at_time_worker_t led_timeout = { .do_work = led_do_work };

static void led_do_work(async_context_t *context, async_at_time_worker_t *worker) {
    // Invert the led

    led_on = !led_on;
    led_short = !led_short;

    if (!is_repeating){
        led_on = led_static_state;
    }

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
    if (is_repeating){
        async_context_add_at_time_worker_in_ms(context, worker, (led_short ? repeating_time : repeating_time_short) );
    }
}


void set_led_mode_pairing(){
    // In main start the process off - AFTER having called cyw43_arch_init...
    led_on = true;
    led_short = true;
    is_repeating = true;
    repeating_time = 100;
    repeating_time_short = 100;
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &led_timeout, repeating_time);
}

void set_led_mode_playing_sbc(){
    led_on = true;
    led_short = true;
    is_repeating = true;
    repeating_time = 1000;
    repeating_time_short = 1000;
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &led_timeout, repeating_time);
}

void set_led_mode_playing_ldac(){
    led_on = true;
    led_short = true;
    is_repeating = true;
    repeating_time_short = 1000;
    repeating_time = 3000;
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &led_timeout, repeating_time);
}

void set_led_mode_playing_aac(){
    led_on = true;
    led_short = true;
    is_repeating = true;
    repeating_time_short = 1000;
    repeating_time = 200;
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &led_timeout, repeating_time);
}




void set_led_mode_off(){
    stop_double_blink();
    stop_triple_blink();
    led_on = true;
    led_short = true;
    is_repeating = false;
    led_static_state = false;
}


void set_led_mode_on(){
    led_on = true;
    led_short = true;
    is_repeating = false;
    led_static_state = true;
}


// ——————————————————————————————————————————
// Double-blink repeat:
// on–off–on–off–pause → repeat
// ——————————————————————————————————————————
static const uint16_t dbl_durations[] = {
    200,   // step 0: LED on  (first blink on-time)
    200,   // step 1: LED off (first blink off-time)
    200,   // step 2: LED on  (second blink on-time)
    200,   // step 3: LED off (second blink off-time)
   2000    // step 4: pause (LED off) before repeating
};
static const bool dbl_states[] = {
    true, false, true, false, false
};
#define DBL_STEPS (sizeof(dbl_states)/sizeof(dbl_states[0]))

static size_t dbl_index = 0;
static async_at_time_worker_t dbl_worker = { .do_work = NULL };

static void dbl_do_work(async_context_t *ctx, async_at_time_worker_t *w){
    // apply LED state for this step
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, dbl_states[dbl_index]);
    // schedule next
    uint16_t delay = dbl_durations[dbl_index];
    dbl_index = (dbl_index + 1) % DBL_STEPS;
    async_context_add_at_time_worker_in_ms(ctx, w, delay);
}

void set_led_mode_double_blink(void){
    // wire up on first use
    if (!dbl_worker.do_work) dbl_worker.do_work = dbl_do_work;
    // start at step 0
    dbl_index = 0;
    async_context_add_at_time_worker_in_ms(
      cyw43_arch_async_context(),
      &dbl_worker,
      dbl_durations[0]
    );
}


// ——————————————————————————————————————————
// Triple-blink repeat:
// on–off–on–off–on–off–pause → repeat
// ——————————————————————————————————————————
static const uint16_t tri_durations[] = {
    200,  // step 0: on
    200,  // step 1: off
    200,  // step 2: on
    200,  // step 3: off
    200,  // step 4: on
    200,  // step 5: off
   2000   // step 6: pause
};
static const bool tri_states[] = {
    true, false, true, false, true, false, false
};
#define TRI_STEPS (sizeof(tri_states)/sizeof(tri_states[0]))

static size_t tri_index = 0;
static async_at_time_worker_t tri_worker = { .do_work = NULL };

static void tri_do_work(async_context_t *ctx, async_at_time_worker_t *w){
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, tri_states[tri_index]);
    uint16_t delay = tri_durations[tri_index];
    tri_index = (tri_index + 1) % TRI_STEPS;
    async_context_add_at_time_worker_in_ms(ctx, w, delay);
}

void set_led_mode_triple_blink(void){
    if (!tri_worker.do_work) tri_worker.do_work = tri_do_work;
    tri_index = 0;
    async_context_add_at_time_worker_in_ms(
      cyw43_arch_async_context(),
      &tri_worker,
      tri_durations[0]
    );
}

// stop double-blink
void stop_double_blink(void){
    async_context_remove_at_time_worker(
        cyw43_arch_async_context(),
        &dbl_worker   // the worker you started in set_led_mode_double_blink_repeat()
    );
    // restore LED to your “static” on/off state
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_static_state);
}

// stop triple-blink
void stop_triple_blink(void){
    async_context_remove_at_time_worker(
        cyw43_arch_async_context(),
        &tri_worker   // the worker you started in set_led_mode_triple_blink_repeat()
    );
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_static_state);
}
