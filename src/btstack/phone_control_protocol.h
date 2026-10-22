// SPDX-License-Identifier: GPL-3.0-only
#ifndef PHONE_CONTROL_PROTOCOL_H
#define PHONE_CONTROL_PROTOCOL_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define PHONE_CONTROL_FRAME_MAX 72u
typedef struct { uint8_t bytes[PHONE_CONTROL_FRAME_MAX]; uint8_t used, expected; } phone_control_parser_t;
typedef bool (*phone_control_request_t)(void *context, const uint8_t *frame);
static inline bool phone_control_feed(phone_control_parser_t *p, const uint8_t *data,
    uint16_t size, phone_control_request_t request, void *context) {
    if (!p || (!data && size) || !request) return false;
    for (unsigned at=0; at<size; ++at) {
        p->bytes[p->used++]=data[at];
        if (p->used==8) {
            const uint8_t *h=p->bytes;
            if (memcmp(h,"APDG",4) || h[4]!=1 || (h[6]!=2 && h[6]!=3) ||
                !((h[5]==1 && h[7]==0) || (h[5]==2 && h[7]==64))) return false;
            p->expected=(uint8_t)(8+h[7]);
        }
        if (p->expected && p->used==p->expected) {
            if (p->bytes[7] && (p->bytes[8]!=p->bytes[6] ||
                memcmp(p->bytes+9,"APDG",4) || p->bytes[13]!=1 || p->bytes[14]!=64)) return false;
            if (!request(context,p->bytes)) return false;
            p->used=p->expected=0;
        }
    }
    return true;
}
#endif
