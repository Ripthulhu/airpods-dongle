// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/realtime_metrics.h"

int main(void) {
    assert(realtime_loop_idle_us(100u,100u,false)==1000u);
    assert(realtime_loop_idle_us(100u,1099u,false)==1u);
    assert(realtime_loop_idle_us(100u,1100u,false)==0u);
    assert(realtime_loop_idle_us(100u,10000u,false)==0u);
    assert(realtime_loop_idle_us(UINT32_MAX-249u,250u,false)==500u);
    assert(realtime_loop_idle_us(100u,100u,true)==5000u);
    assert(realtime_loop_idle_us(100u,5099u,true)==1u);
    assert(realtime_loop_idle_us(100u,5100u,true)==0u);
    assert(realtime_loop_idle_us(100u,10000u,true)==0u);
    assert(realtime_loop_idle_us(UINT32_MAX-249u,250u,true)==4500u);
    _Static_assert(sizeof(realtime_metric_t) == 20u, "Timing counters must remain bounded");
    realtime_metric_t metric = {0};
    realtime_metric_record(&metric, 0u, 0u, REALTIME_USB_SERVICE_BUDGET_US);
    assert(metric.count == 1u && metric.max_us == 0u && metric.overruns == 0u);
    realtime_metric_record(&metric, 1000u, 1500u, REALTIME_USB_SERVICE_BUDGET_US);
    assert(metric.count == 2u && metric.max_us == 500u && metric.overruns == 0u);
    realtime_metric_record(&metric, 1000u, 1501u, REALTIME_USB_SERVICE_BUDGET_US);
    assert(metric.count == 3u && metric.max_us == 501u && metric.overruns == 1u);
    realtime_metric_record(&metric, UINT32_MAX - 249u, 250u, REALTIME_USB_SERVICE_BUDGET_US);
    assert(metric.count == 4u && metric.max_us == 501u && metric.overruns == 1u);
    realtime_metric_record(&metric, UINT32_MAX - 249u, 251u, REALTIME_USB_SERVICE_BUDGET_US);
    assert(metric.count == 5u && metric.max_us == 501u && metric.overruns == 2u);
    realtime_metric_record(&metric, 0u, UINT32_MAX, UINT32_MAX);
    assert(metric.count == 6u && metric.max_us == UINT32_MAX && metric.overruns == 2u);

    metric = (realtime_metric_t){.count = UINT32_MAX - 1u, .overruns = UINT32_MAX - 1u};
    realtime_metric_record(&metric, 10u, 11u, 0u);
    assert(metric.count == UINT32_MAX && metric.overruns == UINT32_MAX && metric.max_us == 1u);
    realtime_metric_record(&metric, 10u, 12u, 0u);
    assert(metric.count == UINT32_MAX && metric.overruns == UINT32_MAX && metric.max_us == 2u);
    metric = (realtime_metric_t){0};
    realtime_metric_record(&metric, UINT32_MAX - 249u, UINT32_MAX - 200u, 500u);
    assert(metric.max_gap_us == 0u);
    realtime_metric_record(&metric, 250u, 299u, 500u);
    assert(metric.max_gap_us == 500u);
    realtime_metric_record(&metric, 2250u, 2299u, 500u);
    assert(metric.max_gap_us == 2000u && metric.max_us == 49u && metric.overruns == 0u);
    puts("realtime metrics: zero, budget boundary, timer wrap, service gaps and saturation passed");
}
