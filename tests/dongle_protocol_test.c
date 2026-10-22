// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/tinyusb/dongle_protocol.h"
int main(void) {
    uint8_t command[64];
    dongle_header(command); command[7] = 3; command[22] = 2;
    dongle_put32(command + 18, 0xabcdef01);
    assert(dongle_u32(command + 18) == 0xabcdef01);
    assert(dongle_command_valid(command, sizeof(command)));
    for (unsigned i = 0; i < sizeof(command); ++i) assert(!dongle_command_valid(command, i));
    command[5]++; assert(!dongle_command_valid(command, sizeof(command))); command[5]--;
    command[63] = 1; assert(!dongle_command_valid(command, sizeof(command))); command[63] = 0;
    command[22] = 4; assert(!dongle_command_valid(command, sizeof(command)));
    command[7] = 1; command[22] = 0; assert(dongle_command_valid(command, sizeof(command)));
    command[7] = 0x58; assert(!dongle_command_valid(command, sizeof(command)));
    dongle_readback_t readback;
    dongle_readback_clear(&readback);
    assert(readback.battery[0] == 255 && !readback.known[0]);
    uint8_t battery[] = {4,0,4,0,4,0,3, 4,1,95,1,0, 2,1,91,2,0, 8,1,62,5,0};
    for (unsigned i = 0; i < sizeof(battery); ++i) dongle_readback_packet(&readback, battery, i, 10);
    assert(!readback.known[0]);
    dongle_readback_packet(&readback, battery, sizeof(battery), 20);
    assert(readback.battery[0] == 95 && readback.battery[1] == 91 && readback.battery[2] == 62);
    assert(readback.charging == 5 && readback.updated[0] == 20);
    uint8_t absent[] = {4,0,4,0,4,0,1, 2,1,255,4,0};
    dongle_readback_packet(&readback, absent, sizeof(absent), 100);
    assert(readback.battery[1] == 255 && readback.updated[1] == 100 && readback.updated[0] == 20);
    battery[9] = 20; battery[19] = 101;
    dongle_readback_packet(&readback, battery, sizeof(battery), 200);
    assert(readback.battery[0] == 95 && readback.updated[0] == 20); // no partial mutation
    uint8_t anc[] = {4,0,4,0,9,0,0x0d,3,0,0,0};
    dongle_readback_packet(&readback, anc, sizeof(anc), 200);
    assert(readback.anc == 3);
    anc[8] = 1; dongle_readback_packet(&readback, anc, sizeof(anc), 210);
    assert(readback.anc == 3);
    dongle_readback_clear(&readback); assert(!readback.anc && !readback.known[1]);
    puts("dongle protocol: bounded commands, readback, stale components and malformed packets passed");
}
