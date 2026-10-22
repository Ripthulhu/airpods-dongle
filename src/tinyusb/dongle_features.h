// SPDX-License-Identifier: GPL-3.0-only
#ifndef DONGLE_FEATURES_H
#define DONGLE_FEATURES_H
#include <stdint.h>
// Zero means completed locally; errors use report-3 failure codes.
uint8_t dongle_features_request(uint8_t client, const uint8_t *request, uint16_t size,
    uint8_t *response, uint16_t capacity, uint16_t *length);
void dongle_features_disconnect(uint8_t client);
void dongle_features_poll(void);
#endif
