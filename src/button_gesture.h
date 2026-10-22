// SPDX-License-Identifier: GPL-3.0-only
#ifndef BUTTON_GESTURE_H
#define BUTTON_GESTURE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BUTTON_GESTURE_NONE,
    BUTTON_GESTURE_SINGLE,
    BUTTON_GESTURE_DOUBLE,
} button_gesture_event_t;

typedef struct {
    bool pending;
    uint64_t released_us;
} button_gesture_t;

static inline button_gesture_event_t button_gesture_release(button_gesture_t *gesture,
                                                            uint64_t now_us) {
    if (gesture->pending && now_us - gesture->released_us <= 500000u) {
        gesture->pending = false;
        return BUTTON_GESTURE_DOUBLE;
    }
    bool expired = gesture->pending;
    gesture->pending = true;
    gesture->released_us = now_us;
    // A late second release completes the first single and starts another wait.
    return expired ? BUTTON_GESTURE_SINGLE : BUTTON_GESTURE_NONE;
}

static inline button_gesture_event_t button_gesture_poll(button_gesture_t *gesture,
                                                         uint64_t now_us) {
    if (gesture->pending && now_us - gesture->released_us > 500000u) {
        gesture->pending = false;
        return BUTTON_GESTURE_SINGLE;
    }
    return BUTTON_GESTURE_NONE;
}

#endif
