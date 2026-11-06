#include "readback.h"
#include <string.h>

static uint32_t crc_byte(uint32_t crc, uint8_t value) {
    crc ^= value;
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    return crc;
}
uint32_t rb_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) crc = crc_byte(crc, data[i]);
    return ~crc;
}
int rb_read_allowed(const rb_span_t *spans, size_t count, uint32_t address, uint32_t length) {
    if (!spans || !length || length > RB_CHUNK || (length & 3u) || (address & 3u) ||
        (address & 0xfffu) + length > 0x1000u || address > UINT32_MAX - length) return 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t size = spans[i].length;
        if (!size || size > 16384u || spans[i].address > UINT32_MAX - size - 3u) continue;
        if (address >= spans[i].address && address + length <= spans[i].address + ((size + 3u) & ~3u)) return 1;
    }
    return 0;
}
int rb_compare(const rb_span_t *span, rb_result_t *out, rb_read_t read, void *context) {
    if (!out) return RB_INVALID;
    memset(out, 0, sizeof(*out));
    if (!span || !read || !span->expected || !span->length || span->length > 16384u ||
        (span->address & 3u) || span->address > UINT32_MAX - span->length - 3u) {
        out->error = RB_INVALID;
        return out->error;
    }
    uint8_t bytes[RB_CHUNK] __attribute__((aligned(4)));
    uint32_t crc = 0xffffffffu;
    while (out->compared < span->length) {
        uint32_t address = span->address + out->compared;
        uint32_t defined = span->length - out->compared;
        if (defined > RB_CHUNK) defined = RB_CHUNK;
        uint32_t room = 0x1000u - (address & 0xfffu);
        if (defined > room) defined = room;
        uint32_t transfer = (defined + 3u) & ~3u;
        memset(bytes, 0xa5, sizeof(bytes));
        int error = read(context, address, transfer, bytes);
        ++out->reads;
        if (error) {
            out->error = error; out->error_address = address; out->error_length = transfer;
            out->observed_crc32 = ~crc;
            return error;
        }
        for (uint32_t i = 0; i < defined; ++i) {
            uint32_t offset = out->compared + i;
            crc = crc_byte(crc, bytes[i]);
            if (bytes[i] != span->expected[offset]) {
                ++out->mismatches;
                if (out->difference_count < RB_DIFF_CAP) {
                    rb_difference_t *difference = &out->differences[out->difference_count++];
                    *difference = (rb_difference_t){offset, span->expected[offset], bytes[i]};
                }
            }
        }
        out->compared += defined;
    }
    out->observed_crc32 = ~crc;
    return 0;
}
