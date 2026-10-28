#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico_w_led.h"

static bool led_static_state,led_on;
enum { LED_STATIC, LED_PAIRING, LED_GAME_ON, LED_GAME_OFF };
static unsigned pattern,step;
static void led_do_work(async_context_t *,async_at_time_worker_t *);
static async_at_time_worker_t led_timeout={.do_work=led_do_work};

static void led_write(bool level) {
    led_on=level;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN,level);
}
static void led_cancel(void) {
    async_context_remove_at_time_worker(cyw43_arch_async_context(),&led_timeout);
    pattern=LED_STATIC;step=0;
}
static void led_do_work(async_context_t *ctx,async_at_time_worker_t *worker) {
    if(pattern==LED_PAIRING) {
        led_write(!led_on);
        async_context_add_at_time_worker_in_ms(ctx,worker,100);
    } else if(pattern==LED_GAME_ON && step<4) {
        led_write((step++&1u)==0);
        async_context_add_at_time_worker_in_ms(ctx,worker,150);
    } else {
        pattern=LED_STATIC;
        led_write(led_static_state);
    }
}
void set_led_mode_pairing(void) {
    led_cancel();pattern=LED_PAIRING;led_write(true);
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(),&led_timeout,100);
}
void set_led_mode_off(void) {
    led_cancel();led_static_state=false;led_write(false);
}
void set_led_mode_on(void) {
    led_cancel();led_static_state=true;led_write(true);
}
void set_led_game_feedback(bool enabled) {
    led_cancel();pattern=enabled?LED_GAME_ON:LED_GAME_OFF;
    led_write(false);
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(),&led_timeout,enabled?150:3000);
}
