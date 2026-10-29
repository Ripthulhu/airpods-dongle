// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/runtime_watch.h"
#include "../src/realtime_metrics.h"

typedef struct { int64_t delay_us; } repeating_timer_t;
static struct { uint32_t scratch[8]; } watchdog_registers;
#define watchdog_hw (&watchdog_registers)
static realtime_metric_t usb_service_metric;
static uint32_t now, next_interval, selected_interval;
static unsigned calls;
static void hw_set_bits(uint32_t *value,uint32_t bits) { *value|=bits; }
static void hw_clear_bits(uint32_t *value,uint32_t bits) { *value&=~bits; }
static uint32_t time_us_32(void) { return now; }
static void tinyusb_task(void) {
    assert(watchdog_hw->scratch[0]&RUNTIME_WATCH_USB);
    ++calls;now+=25;
    selected_interval=next_interval; // SET_INTERFACE/CDC state is processed here.
}
static uint32_t tinyusb_service_interval_us(void) {
    assert(calls==usb_service_metric.count+1);
    return selected_interval;
}
#include "main_usb_timer.inc"

int main(void) {
    repeating_timer_t timer={.delay_us=-500};
    next_interval=REALTIME_USB_IDLE_INTERVAL_US;
    assert(usb_timer_callback(&timer) && timer.delay_us==-4000);
    assert(!(watchdog_hw->scratch[0]&RUNTIME_WATCH_USB));
    assert(usb_service_metric.count==1 && usb_service_metric.max_us==25);
    // Opening an alternate during the previously scheduled idle tick must
    // restore fast service immediately, with deadlines still start-relative.
    now=4000;next_interval=REALTIME_USB_SERVICE_BUDGET_US;
    assert(usb_timer_callback(&timer) && timer.delay_us==-500);
    assert(usb_service_metric.max_gap_us==4000 && usb_service_metric.overruns==0);
    now=4500;
    assert(usb_timer_callback(&timer) && timer.delay_us==-500);
    now=5000;next_interval=REALTIME_USB_IDLE_INTERVAL_US;
    assert(usb_timer_callback(&timer) && timer.delay_us==-4000);
    assert(calls==4 && usb_service_metric.count==4 && usb_service_metric.max_us==25);
    puts("USB timer: interface changes precede next deadline; idle/active cadence and start-relative pacing passed");
}
