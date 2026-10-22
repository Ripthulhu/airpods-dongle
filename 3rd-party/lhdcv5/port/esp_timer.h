#ifndef LHDC_DONGLE_TIME_H
#define LHDC_DONGLE_TIME_H
#include <stdint.h>
// The owning worker measures complete decode/copy time; avoid duplicate inner-loop clocks.
static inline int64_t esp_timer_get_time(void) { return 0; }
#endif
