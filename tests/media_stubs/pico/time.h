#ifndef MEDIA_TEST_TIME_H
#define MEDIA_TEST_TIME_H
#include <stdint.h>
uint32_t time_us_32(void);
uint64_t get_absolute_time(void);
uint32_t to_ms_since_boot(uint64_t time);
#endif
