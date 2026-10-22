// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/button_gesture.h"

int main(void) {
    button_gesture_t gesture = {0};
    assert(button_gesture_poll(&gesture, 1000000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_release(&gesture, 120000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_poll(&gesture, 619999) == BUTTON_GESTURE_NONE);
    assert(button_gesture_poll(&gesture, 620000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_release(&gesture, 620000) == BUTTON_GESTURE_DOUBLE);
    assert(!gesture.pending);
    assert(button_gesture_poll(&gesture, 2000000) == BUTTON_GESTURE_NONE);

    assert(button_gesture_release(&gesture, 2000000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_poll(&gesture, 2500001) == BUTTON_GESTURE_SINGLE);
    assert(!gesture.pending);
    assert(button_gesture_poll(&gesture, 3000000) == BUTTON_GESTURE_NONE);

    assert(button_gesture_release(&gesture, 120000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_poll(&gesture, 620000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_release(&gesture, 640000) == BUTTON_GESTURE_SINGLE);
    assert(gesture.pending && gesture.released_us == 640000);
    assert(button_gesture_poll(&gesture, 640000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_poll(&gesture, 1140000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_poll(&gesture, 1140001) == BUTTON_GESTURE_SINGLE);
    assert(!gesture.pending);

    assert(button_gesture_release(&gesture, 2000000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_release(&gesture, 2500001) == BUTTON_GESTURE_SINGLE);
    assert(button_gesture_release(&gesture, 2700000) == BUTTON_GESTURE_DOUBLE);
    assert(!gesture.pending);

    assert(button_gesture_release(&gesture, 3000000) == BUTTON_GESTURE_NONE);
    gesture.pending = false; // A long press cancels the pending short press.
    assert(button_gesture_poll(&gesture, 4000000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_release(&gesture, 4100000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_release(&gesture, 4200000) == BUTTON_GESTURE_DOUBLE);

    assert(button_gesture_release(&gesture, UINT64_MAX - 100000) == BUTTON_GESTURE_NONE);
    assert(button_gesture_release(&gesture, 100000) == BUTTON_GESTURE_DOUBLE);
    puts("button releases: inclusive double boundary, expiry, late release and long cancellation passed");
    return 0;
}
