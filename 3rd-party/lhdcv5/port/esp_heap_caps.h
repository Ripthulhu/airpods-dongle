#ifndef LHDC_DONGLE_HEAP_H
#define LHDC_DONGLE_HEAP_H
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_malloc(bytes, flags) malloc(bytes)
#define heap_caps_free(pointer) free(pointer)
#endif
