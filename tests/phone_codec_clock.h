// Synthetic timing for the integrated startup-gate regression, never a benchmark.
#include <stdint.h>
uint32_t phone_test_time_us(void);
#define PHONE_WORKER_TIME_US() phone_test_time_us()
