// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// The linked-descriptor guard validates their contents; test the real selector here.
static const uint8_t desc_device[]={0x15},desc_device_control[]={0x05};
static const uint8_t desc_configuration[]={6},desc_configuration_control[]={3};
#include "usb_descriptor_selector.inc"

int main(void) {
    assert(!descriptor_audio); // Before main or any USB callback, no audio personality.
    assert(tud_descriptor_device_cb()==desc_device_control);
    assert(tud_descriptor_configuration_cb(0)==desc_configuration_control);
    for(unsigned i=0;i<3;++i) {
        usb_descriptors_set_audio(true);
        assert(tud_descriptor_device_cb()==desc_device);
        assert(tud_descriptor_configuration_cb(0)==desc_configuration);
        usb_descriptors_set_audio(false);
        assert(tud_descriptor_device_cb()==desc_device_control);
        assert(tud_descriptor_configuration_cb(0)==desc_configuration_control);
    }
    puts("USB descriptors: control-only cold boot and matching device/configuration transitions passed");
}
