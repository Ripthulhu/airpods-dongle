#include "readback.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint32_t base, length; uint8_t data[16388]; unsigned calls, fail_at; bool outside; } fake_t;
static int fake_read(void *context, uint32_t address, uint32_t length, uint8_t *bytes) {
    fake_t *fake = context;
    assert(!(address & 3u) && !(length & 3u) && length >= 4 && length <= 32);
    assert((address & 0xfffu) + length <= 0x1000u);
    assert(!((uintptr_t)bytes & 3u));
    ++fake->calls;
    if (fake->calls == fake->fail_at) return -7;
    if (address < fake->base || address + length > fake->base + ((fake->length + 3u) & ~3u)) fake->outside = true;
    assert(!fake->outside);
    memcpy(bytes, fake->data + address - fake->base, length);
    return 0;
}
int main(void) {
    assert(rb_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    uint8_t expected[16384]; for (unsigned i = 0; i < sizeof(expected); ++i) expected[i] = (uint8_t)(i & 127u);
    const unsigned lengths[] = {1,2,3,4,31,32,33,66,100,128,10800,16384};
    for (unsigned n = 0; n < sizeof(lengths)/sizeof(lengths[0]); ++n) {
        fake_t fake = {.base=0x19000ff0,.length=lengths[n]}; memcpy(fake.data, expected, sizeof(expected));
        rb_span_t span = {fake.base,fake.length,expected,rb_crc32(expected,fake.length),"test"}; rb_result_t result;
        assert(rb_compare(&span,&result,fake_read,&fake) == 0);
        assert(result.compared == fake.length && !result.mismatches && !result.error && result.observed_crc32 == span.crc32);
        fake.calls=0; memset(fake.data,0xaa,fake.length);
        assert(rb_compare(&span,&result,fake_read,&fake) == 0 && result.mismatches == fake.length);
        assert(result.difference_count == (fake.length < RB_DIFF_CAP ? fake.length : RB_DIFF_CAP));
    }
    fake_t fake={.base=0x19001000,.length=66,.fail_at=2}; memcpy(fake.data,expected,sizeof(expected));
    rb_span_t span={fake.base,fake.length,expected,0,"error"}; rb_result_t result;
    assert(rb_compare(&span,&result,fake_read,&fake)==-7 && result.compared==32 && result.error_address==fake.base+32 && result.error_length==32);
    fake.calls=0; fake.fail_at=3;
    assert(rb_compare(&span,&result,fake_read,&fake)==-7 && result.compared==64 && result.error_length==4);
    fake.calls=0; fake.fail_at=0; fake.data[65]^=1;
    assert(rb_compare(&span,&result,fake_read,&fake)==0 && result.mismatches==1 && result.differences[0].offset==65);
    unsigned calls=fake.calls; span.address++; assert(rb_compare(&span,&result,fake_read,&fake)==RB_INVALID && fake.calls==calls);
    span.address=UINT32_MAX-3; assert(rb_compare(&span,&result,fake_read,&fake)==RB_INVALID);
    span.address=fake.base; span.length=0; assert(rb_compare(&span,&result,fake_read,&fake)==RB_INVALID);
    span.length=16385; assert(rb_compare(&span,&result,fake_read,&fake)==RB_INVALID);
    span.length=4; span.expected=NULL; assert(rb_compare(&span,&result,fake_read,&fake)==RB_INVALID);
    assert(rb_compare(NULL,&result,fake_read,&fake)==RB_INVALID);
    assert(rb_compare(&span,NULL,fake_read,&fake)==RB_INVALID);
    span=(rb_span_t){0x19218000,66,expected,0,"owned"};
    assert(rb_read_allowed(&span,1,0x19218040,4)); // Two padding bytes read, never compared.
    assert(!rb_read_allowed(&span,1,0x19218044,4));
    assert(!rb_read_allowed(&span,1,0x19217ffc,4));
    assert(!rb_read_allowed(&span,1,0x19204164,4)); // Final callback excluded.
    assert(!rb_read_allowed(&span,1,0x19218001,4));
    assert(!rb_read_allowed(&span,1,0x19218000,33));
    assert(!rb_read_allowed(&span,1,0x19218ff0,32));
    assert(!rb_read_allowed(&span,1,UINT32_MAX-3,8));
    puts("Readback comparison tests passed");
    return 0;
}
