// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int INT;
#ifdef FDK_DONGLE_PCM32
typedef int32_t INT_PCM;
#else
typedef int16_t INT_PCM;
#endif
typedef enum {
    AACENC_OK, AACENC_INVALID_CONFIG, AACENC_MEMORY_ERROR
} AACENC_ERROR;

static unsigned allocations, frees;
static size_t last_bytes;
static int fail_next;
static void *checked_calloc(size_t count, size_t size) {
    ++allocations;
    last_bytes=count*size;
    if(fail_next) { fail_next=0; return NULL; }
    return calloc(count,size);
}
static void checked_free(void *pointer) { ++frees; free(pointer); }
#define FDKcalloc checked_calloc
#define FDKfree checked_free
#define FDKmemcpy memcpy
#include "../3rd-party/fdk-aac/libAACenc/src/dongle_input_buffer.h"

static void invalid_and_first_failure(void) {
    INT_PCM *buffer=NULL;
    INT stride=0,total=0;
    unsigned before=allocations;
    const INT bad[][2]={{0,480},{3,480},{2,0},{2,1025},{INT_MAX,INT_MAX}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i)
        assert(fdk_dongle_input_buffer(&buffer,&stride,&total,bad[i][0],bad[i][1])==AACENC_INVALID_CONFIG);
    stride=1;
    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,2,480)==AACENC_INVALID_CONFIG);
    stride=0; total=1;
    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,2,480)==AACENC_INVALID_CONFIG);
    assert(allocations==before && buffer==NULL);
    total=0; fail_next=1;
    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,2,480)==AACENC_MEMORY_ERROR);
    assert(buffer==NULL && stride==0 && total==0 && allocations==before+1);
}

static void eld_grow_and_retune(void) {
    INT_PCM *buffer=NULL;
    INT stride=0,total=0;
    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,2,480)==AACENC_OK);
    assert(stride==480 && total==960 && last_bytes==960*sizeof(INT_PCM));
    for(INT channel=0;channel<2;++channel)
        for(INT i=0;i<480;++i) buffer[channel*stride+i]=(INT_PCM)((channel? -1:1)*(i+1));
    INT_PCM *original=buffer;
    unsigned before=allocations,free_before=frees;
    const INT retunes[]={480,480,240,120,480};
    for(unsigned i=0;i<sizeof(retunes)/sizeof(*retunes);++i)
        assert(fdk_dongle_input_buffer(&buffer,&stride,&total,2,retunes[i])==AACENC_OK);
    assert(buffer==original && allocations==before && frees==free_before);

    fail_next=1;
    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,2,1024)==AACENC_MEMORY_ERROR);
    assert(buffer==original && stride==480 && total==960 && frees==free_before);
    for(INT channel=0;channel<2;++channel)
        for(INT i=0;i<480;++i) assert(buffer[channel*stride+i]==(INT_PCM)((channel? -1:1)*(i+1)));

    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,2,1024)==AACENC_OK);
    assert(buffer!=original && stride==1024 && total==2048 && frees==free_before+1);
    for(INT channel=0;channel<2;++channel) {
        for(INT i=0;i<480;++i) assert(buffer[channel*stride+i]==(INT_PCM)((channel? -1:1)*(i+1)));
        for(INT i=480;i<1024;++i) assert(buffer[channel*stride+i]==0);
    }
    before=allocations;
    for(unsigned i=0;i<200;++i)
        assert(fdk_dongle_input_buffer(&buffer,&stride,&total,2,i%2?480:1024)==AACENC_OK);
    assert(allocations==before && stride==1024);
    INT saved_stride=stride;
    stride=INT_MAX;
    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,2,480)==AACENC_INVALID_CONFIG);
    stride=saved_stride;
    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,1,480)==AACENC_INVALID_CONFIG);
    checked_free(buffer);
}

static void lc_first_and_mono(void) {
    INT_PCM *buffer=NULL;
    INT stride=0,total=0;
    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,2,1024)==AACENC_OK);
    assert(stride==1024 && total==2048 && last_bytes==2048*sizeof(INT_PCM));
    unsigned before=allocations;
    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,2,480)==AACENC_OK);
    assert(allocations==before && stride==1024);
    checked_free(buffer);

    buffer=NULL; stride=total=0;
    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,1,480)==AACENC_OK);
    assert(stride==480 && total==480);
    for(INT i=0;i<480;++i) buffer[i]=(INT_PCM)i;
    assert(fdk_dongle_input_buffer(&buffer,&stride,&total,1,512)==AACENC_OK);
    for(INT i=0;i<512;++i) assert(buffer[i]==(i<480?(INT_PCM)i:0));
    checked_free(buffer);
}

int main(void) {
    invalid_and_first_failure();
    eld_grow_and_retune();
    lc_first_and_mono();
    assert(allocations==frees+2); /* exactly the two deliberately denied requests */
    puts("FDK frame-sized input allocation, rollback, channel history and retune: PASS");
    return 0;
}
