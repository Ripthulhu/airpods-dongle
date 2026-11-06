#ifndef CYW_READBACK_H
#define CYW_READBACK_H
#include <stddef.h>
#include <stdint.h>
enum { RB_CHUNK = 32, RB_DIFF_CAP = 8, RB_INVALID = -100, RB_DEADLINE = -101 };
typedef struct { uint32_t address, length; const uint8_t *expected; uint32_t crc32; const char *name; } rb_span_t;
typedef struct { uint32_t offset; uint8_t expected, observed; } rb_difference_t;
typedef struct {
    uint32_t compared, reads, mismatches, observed_crc32, error_address, error_length;
    int error;
    unsigned difference_count;
    rb_difference_t differences[RB_DIFF_CAP];
} rb_result_t;
typedef int (*rb_read_t)(void *, uint32_t, uint32_t, uint8_t *);
uint32_t rb_crc32(const uint8_t *, size_t);
int rb_read_allowed(const rb_span_t *, size_t, uint32_t, uint32_t);
int rb_compare(const rb_span_t *, rb_result_t *, rb_read_t, void *);
#endif
