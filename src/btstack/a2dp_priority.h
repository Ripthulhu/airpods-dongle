// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_A2DP_PRIORITY_H
#define AIRPODS_A2DP_PRIORITY_H
#include <stdbool.h>
#include <stdint.h>

enum priority_reason {
    PRIORITY_RESET, PRIORITY_ARM, PRIORITY_OFF, PRIORITY_START, PRIORITY_SUSPEND,
    PRIORITY_CLOSE, PRIORITY_SCOPE_LOST, PRIORITY_ACL_LOST, PRIORITY_EXPIRED,
    PRIORITY_COMMAND_ERROR, PRIORITY_TIMEOUT, PRIORITY_USB_LOST
};
void bt_hci_priority_snapshot(uint8_t out[9]);
void bt_hci_priority_transition(enum priority_reason reason);
#endif
