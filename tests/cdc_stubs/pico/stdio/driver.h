// SPDX-License-Identifier: GPL-3.0-only
#ifndef TEST_CDC_STDIO_DRIVER_H
#define TEST_CDC_STDIO_DRIVER_H
#include <stdbool.h>
typedef struct {
    void (*out_chars)(const char *, int);
    void (*out_flush)(void);
    int (*in_chars)(char *, int);
} stdio_driver_t;
void stdio_set_driver_enabled(stdio_driver_t *driver, bool enabled);
#endif
