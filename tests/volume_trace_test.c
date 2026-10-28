// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/volume_trace.h"

int main(void) {
    volume_trace_t trace = {0};
    uint32_t out[4];
    for (unsigned i = 0; i < 1000; ++i) volume_trace_record(&trace, i, 1, 2, 3);
    assert(!trace.total && !volume_trace_freeze(&trace) && !volume_trace_take(&trace, out));
    volume_trace_arm(&trace, 100);
    for (unsigned i = 0; i < 37; ++i)
        volume_trace_record(&trace, 100u + i, i, 1000u + i, 2000u + i);
    assert(trace.count == 16 && trace.total == 37 && sizeof(trace) == 272);
    assert(volume_trace_freeze(&trace) == 37 && !trace.armed);
    assert(!volume_trace_take(&trace, NULL) && trace.remaining == 16);
    volume_trace_record(&trace, 200, 99, 99, 99);
    for (unsigned i = 21; i < 37; ++i) {
        assert(volume_trace_take(&trace, out));
        assert(out[0] == 100u + i && out[1] == i && out[2] == 1000u + i && out[3] == 2000u + i);
    }
    assert(!volume_trace_take(&trace, out));
    assert(volume_trace_freeze(&trace) == 37 && volume_trace_take(&trace, out) && out[1] == 21);
    volume_trace_arm(&trace, UINT32_MAX - 1000u);
    assert(trace.armed && !trace.total && !trace.count && !trace.remaining);
    volume_trace_record(&trace, trace.started_us + VOLUME_TRACE_LEASE_US - 1u, 4, 5, 6);
    assert(trace.total == 1);
    volume_trace_record(&trace, trace.started_us + VOLUME_TRACE_LEASE_US, 7, 8, 9);
    assert(trace.total == 1 && !trace.armed);
    volume_trace_record(&trace, trace.started_us + VOLUME_TRACE_LEASE_US + 1u, 7, 8, 9);
    assert(volume_trace_freeze(&trace) == 1 && volume_trace_take(&trace, out));
    assert(out[1] == 4 && out[2] == 5 && out[3] == 6 && !volume_trace_take(&trace, out));
    puts("volume trace: default off, bounded chronological wrap, frozen dump, reset and exact 60-second lease passed");
}
