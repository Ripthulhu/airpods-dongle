#include <assert.h>
#include <stdio.h>
#include "../src/pico_w_led.c"
static async_context_t context;
static bool level;
static unsigned scheduled,removed,last_delay;
async_context_t *cyw43_arch_async_context(void) {return &context;}
void cyw43_arch_gpio_put(unsigned pin,bool value) {assert(pin==0);level=value;}
void async_context_add_at_time_worker_in_ms(async_context_t *ctx,async_at_time_worker_t *w,uint32_t delay) {
    assert(ctx==&context && w && delay>0);++scheduled;last_delay=delay;
}
void async_context_remove_at_time_worker(async_context_t *ctx,async_at_time_worker_t *w) {
    assert(ctx==&context && w);++removed;
}
static void tick(void) {led_do_work(&context,&led_timeout);}
int main(void) {
    set_led_mode_pairing();assert(pattern==LED_PAIRING && level && last_delay==100);
    tick();assert(!level && last_delay==100);
    set_led_mode_on();assert(level && pattern==LED_STATIC);
    unsigned before=scheduled;tick();assert(level && scheduled==before);
    set_led_game_feedback(true);assert(!level && last_delay==150);
    tick();assert(level);tick();assert(!level);tick();assert(level);tick();assert(!level);
    tick();assert(level && pattern==LED_STATIC); // Two finite pulses, then connected solid.
    before=scheduled;tick();assert(level && scheduled==before);
    set_led_game_feedback(false);assert(!level && last_delay==3000);
    tick();assert(level && pattern==LED_STATIC);
    set_led_game_feedback(false);set_led_game_feedback(true);
    tick();assert(level && step==1); // A new toggle replaces the earlier timer.
    set_led_mode_off();tick();assert(!level && pattern==LED_STATIC);
    set_led_game_feedback(false);tick();assert(!level); // Disconnected base stays off.
    set_led_game_feedback(true);set_led_mode_pairing();tick();assert(pattern==LED_PAIRING);
    assert(removed>0);
    puts("LED: solid connection, finite two pulses, three-second off, cancellation and pairing passed");
    return 0;
}
