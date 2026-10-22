// SPDX-License-Identifier: GPL-3.0-only
#ifndef TEST_SLOT_FLASH_H
#define TEST_SLOT_FLASH_H
#include <stdint.h>
#define PICO_FLASH_SIZE_BYTES 16384u
#define FLASH_SECTOR_SIZE 4096u
#define FLASH_PAGE_SIZE 256u
extern uint8_t legacy_flash[PICO_FLASH_SIZE_BYTES];
#define XIP_BASE ((uintptr_t)legacy_flash)
#endif
