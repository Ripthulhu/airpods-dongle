#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/runtime_watch.h"
#include "../src/realtime_metrics.h"

typedef unsigned async_context_t;
static async_context_t context;
static uint32_t now;
static realtime_metric_t bluetooth_lock_metric;
static unsigned lock_depth, samples, actions, deferred_irqs, tlv_writes;
static bool pending_action, pending_irq, flash_lock;
static uint32_t get_absolute_time(void) { return now; }
static uint32_t to_ms_since_boot(uint32_t time) { return time; }
static uint32_t time_us_32(void) { return now*1000u; }
static async_context_t *cyw43_arch_async_context(void) { return &context; }
static void async_context_acquire_lock_blocking(async_context_t *ctx) {
    assert(ctx==&context && lock_depth==0); ++lock_depth;
}
static void bluetooth_irq(void) {
    if(lock_depth) { pending_irq=true; ++deferred_irqs; return; }
    // A nested TLV write would wait for the SDK flash mutex held by the sampler.
    assert(!flash_lock); ++tlv_writes;
}
static void async_context_release_lock(async_context_t *ctx) {
    assert(ctx==&context && lock_depth==1); --lock_depth;
    if(pending_irq) { pending_irq=false; bluetooth_irq(); }
}
static void process_button_actions(void) {
    assert(lock_depth==0);
    if(pending_action) { ++actions; pending_action=false; }
}
static void check_bootsel_state(void) {
    assert(lock_depth==1);
    flash_lock=true;
    bluetooth_irq();
    ++samples; pending_action=true;
    flash_lock=false;
}
static void dongle_playback_encode_complete(void) {assert(lock_depth==1);}

#include "main_button_poll.inc"

int main(void) {
    uint32_t last_sample=0;
    now=20; main_button_poll(&last_sample);
    assert(samples==1 && actions==0 && last_sample==20);
    assert(deferred_irqs==1 && tlv_writes==1 && lock_depth==0);
    now=21; main_button_poll(&last_sample);
    assert(samples==1 && actions==1);
    now=40; main_button_poll(&last_sample);
    assert(samples==2 && actions==1 && last_sample==40);
    now=41; main_button_poll(&last_sample);
    assert(samples==2 && actions==2);
    last_sample=UINT32_MAX-9; now=10; main_button_poll(&last_sample);
    assert(samples==3 && last_sample==10);
    now=11; main_button_poll(&last_sample);
    assert(actions==3 && samples==3 && tlv_writes==3 && lock_depth==0);
    assert(bluetooth_lock_metric.count==6 && bluetooth_lock_metric.total_us==0);
    puts("main button polling: flash lockout serialized, IRQ deferred, next-pass actions and timer wrap passed");
    return 0;
}
