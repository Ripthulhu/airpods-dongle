// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/btstack/playback_duck.h"

static void unity_is_bit_exact(void) {
    playback_duck_t state;
    playback_duck_reset(&state);
    int32_t wide[] = {INT32_MIN, INT32_MAX, -1, 1, 0, 8388609, -8388609, 123456789};
    int32_t original[8]; memcpy(original, wide, sizeof(wide));
    playback_duck_q31(&state, wide, 4, 48000, false);
    assert(!memcmp(wide, original, sizeof(wide)));
    for (int value=INT16_MIN; value<=INT16_MAX; ++value) {
        int16_t narrow[2] = {(int16_t)value, (int16_t)-1};
        playback_duck_s16(&state, narrow, 1, 44100, false);
        assert(narrow[0] == value && narrow[1] == -1);
    }
    assert(sizeof(state) == 32);
}

static void check_ramps(uint32_t rate) {
    playback_duck_t wide, narrow;
    playback_duck_reset(&wide); playback_duck_reset(&narrow);
    int32_t previous=PLAYBACK_DUCK_UNITY;
    for (unsigned direction=0; direction<2; ++direction) {
        uint32_t frames=rate*(direction ? 500u : 150u)/1000u;
        for (uint32_t frame=0; frame<frames; ++frame) {
            int32_t samples[2]={INT32_MAX, INT32_MAX};
            int16_t small[2]={INT16_MIN, INT16_MIN};
            playback_duck_q31(&wide,samples,1,rate,!direction);
            playback_duck_s16(&narrow,small,1,rate,!direction);
            assert(samples[0]==samples[1] && small[0]==small[1]);
            assert(wide.gain==narrow.gain);
            assert(wide.gain>=PLAYBACK_DUCK_ATTENUATED && wide.gain<=PLAYBACK_DUCK_UNITY);
            assert(direction ? wide.gain>=previous : wide.gain<=previous);
            assert(llabs((long long)wide.gain-previous)<=PLAYBACK_DUCK_UNITY/frames+1);
            previous=wide.gain;
            assert(wide.remaining==frames-frame-1);
        }
        assert(wide.gain==(direction ? PLAYBACK_DUCK_UNITY : PLAYBACK_DUCK_ATTENUATED));
    }
    int32_t exact[2]={-8388609,INT32_MIN};
    playback_duck_q31(&wide,exact,1,rate,false);
    assert(exact[0]==-8388609 && exact[1]==INT32_MIN);
}

static void attenuation_is_bounded_and_rounded(void) {
    const int32_t inputs[]={INT32_MIN,INT32_MAX,-123456789,-65537,-3,-2,-1,0,1,2,3,65537,123456789};
    for (unsigned i=0;i<sizeof(inputs)/sizeof(*inputs);++i) {
        int32_t result=playback_duck_sample(inputs[i],PLAYBACK_DUCK_ATTENUATED);
        assert(result==llround((double)inputs[i]/4));
        assert(llabs((long long)result)<=llabs((long long)inputs[i]));
    }
    playback_duck_t state;
    playback_duck_reset(&state);
    playback_duck_target(&state,true,48000);
    for (unsigned i=0;i<7200;++i)playback_duck_next(&state);
    int32_t wide[2]={INT32_MIN,INT32_MAX};
    playback_duck_q31(&state,wide,1,48000,true);
    assert(wide[0]==INT32_MIN/4 && wide[1]==536870912);
    int16_t small[2]={INT16_MIN,INT16_MAX};
    playback_duck_s16(&state,small,1,48000,true);
    assert(small[0]==INT16_MIN/4 && small[1]==8192);
}

static void block_sizes_and_mid_ramp_reversal(void) {
    playback_duck_t whole, split;
    playback_duck_reset(&whole); playback_duck_reset(&split);
    int32_t full[480*2],chunks[480*2];
    for(unsigned i=0;i<960;++i)full[i]=chunks[i]=INT32_MAX-(int32_t)i;
    playback_duck_q31(&whole,full,480,48000,true);
    const unsigned blocks[]={1,127,3,221,128};
    unsigned offset=0;
    for(unsigned i=0;i<sizeof(blocks)/sizeof(*blocks);++i) {
        playback_duck_q31(&split,chunks+offset*2,blocks[i],48000,true);
        offset+=blocks[i];
    }
    assert(offset==480 && !memcmp(full,chunks,sizeof(full)) && !memcmp(&whole,&split,sizeof(whole)));
    int32_t before=split.gain,samples[2]={INT32_MAX,INT32_MAX};
    playback_duck_q31(&split,samples,1,48000,false);
    assert(split.gain>=before && split.gain-before<50000);
    before=split.gain;
    playback_duck_q31(&split,samples,1,44100,true);
    assert(split.gain<=before && before-split.gain<150000);
    playback_duck_reset(&split);
    assert(split.gain==PLAYBACK_DUCK_UNITY && !split.remaining);
    samples[0]=INT32_MIN;samples[1]=INT32_MAX;
    playback_duck_q31(&split,samples,1,48000,false);
    assert(samples[0]==INT32_MIN && samples[1]==INT32_MAX);
    whole=split;
    playback_duck_q31(&split,samples,1,0,true);
    playback_duck_s16(&split,NULL,1,48000,true);
    playback_duck_q31(&split,samples,0,48000,true);
    assert(!memcmp(&whole,&split,sizeof(whole)));
}

int main(void) {
    unity_is_bit_exact();
    check_ramps(48000);check_ramps(44100);check_ramps(8000);check_ramps(96000);
    attenuation_is_bounded_and_rounded();
    block_sizes_and_mid_ramp_reversal();
    puts("playback duck: bit-exact unity, bounded Q31/S16 stereo ramps, rounding, block splits and reset passed");
}
