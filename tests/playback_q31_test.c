// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/btstack/playback_pcm.h"
#include "../src/btstack/playback_inspect.h"

static playback_pcm_t pcm;
static playback_resample_t work;

static int32_t quantize_double_oracle(float sample) {
    if(!isfinite(sample))return 0;
    if(sample>=1.0f)return INT32_MAX;
    if(sample<=-1.0f)return INT32_MIN;
    double scaled=(double)sample*2147483648.0;
    return (int32_t)(scaled<0?ceil(scaled-0.5):floor(scaled+0.5));
}

static void rounding_neighbors(void) {
    const float centers[]={0.0f,0.5f,1.5f,2.5f,63.5f,65535.5f,
                           4194303.5f,8388606.5f,8388607.5f,8388608.0f,
                           8388609.0f,8388611.0f,2147483520.0f};
    for(unsigned i=0;i<sizeof(centers)/sizeof(*centers);++i) {
        for(unsigned negative=0;negative<2;++negative) {
            float sample=(negative?-centers[i]:centers[i])*0x1p-31f;
            const float neighbors[]={nextafterf(sample,-INFINITY),sample,
                                     nextafterf(sample,INFINITY)};
            for(unsigned n=0;n<3;++n)
                assert(playback_pcm_quantize_q31(neighbors[n])==quantize_double_oracle(neighbors[n]));
        }
    }
    assert(playback_pcm_quantize_q31(nextafterf(0x1p-32f,0.0f))==0);
    assert(playback_pcm_quantize_q31(nextafterf(-0x1p-32f,0.0f))==0);
    assert(playback_pcm_quantize_q31(nextafterf(0x1p-32f,INFINITY))==1);
    assert(playback_pcm_quantize_q31(nextafterf(-0x1p-32f,-INFINITY))==-1);
    uint32_t bits=0x31415926;
    for(unsigned i=0;i<100000;++i) {
        bits^=bits<<13; bits^=bits>>17; bits^=bits<<5;
        float sample; memcpy(&sample,&bits,sizeof(sample));
        assert(playback_pcm_quantize_q31(sample)==quantize_double_oracle(sample));
    }
}

static void numeric_boundaries(void) {
    assert(playback_pcm_quantize_q31(NAN)==0);
    assert(playback_pcm_quantize_q31(INFINITY)==0 && playback_pcm_quantize_q31(-INFINITY)==0);
    assert(playback_pcm_quantize_q31(1)==INT32_MAX && playback_pcm_quantize_q31(2)==INT32_MAX);
    assert(playback_pcm_quantize_q31(-1)==INT32_MIN && playback_pcm_quantize_q31(-2)==INT32_MIN);
    assert(playback_pcm_quantize_q31(0x1p-32f)==1 && playback_pcm_quantize_q31(-0x1p-32f)==-1);
    assert(playback_pcm_quantize_q31(0x1p-33f)==0 && playback_pcm_quantize_q31(-0x1p-33f)==0);
    assert(playback_pcm_quantize(0x1p-20f)==0 && playback_pcm_quantize_q31(0x1p-20f)==2048);
    assert(playback_pcm_quantize_q31(0.125f+0x1p-20f)==268437504);
    const int32_t integral[]={8388607,8388608,8388609,8388611,16777218};
    for(unsigned i=0;i<sizeof(integral)/sizeof(*integral);++i) {
        float value=(float)integral[i]/2147483648.0f;
        assert(playback_pcm_quantize_q31(value)==integral[i]);
        assert(playback_pcm_quantize_q31(-value)==-integral[i]);
    }
    assert(playback_pcm_quantize_q31(8388607.5f/2147483648.0f)==8388608);
    assert(playback_pcm_quantize_q31(-8388607.5f/2147483648.0f)==-8388608);
    for(int value=INT16_MIN;value<=INT16_MAX;++value)
        assert(playback_pcm_quantize_q31((float)value/32768)==(int32_t)value*65536);
    assert(sizeof(playback_encoder_pcm_t)==4096);
}

static void ring_and_channels(void) {
    float input[1024];
    for(unsigned i=0;i<512;++i) {
        input[2*i]=(i+1)*0x1p-20f;input[2*i+1]=-(float)(i+1)*0x1p-19f;
    }
    assert(playback_pcm_reset(&pcm,1024,false));
    playback_pcm_push_float(&pcm,input,512);
    assert(pcm.samples[0]==0x1p-20f && pcm.samples[1]==-0x1p-19f);
    assert(playback_pcm_prepare_resample(&pcm,&work,480,655360));
    struct { uint32_t before;int32_t samples[960];uint32_t after; } out={.before=0x12345678,.after=0x87654321};
    playback_pcm_render_q31(&work,out.samples);
    assert(out.before==0x12345678 && out.after==0x87654321);
    for(unsigned i=0;i<480;++i) {
        int32_t n=i<PLAYBACK_FILTER_DELAY?0:(int32_t)(i-PLAYBACK_FILTER_DELAY+1);
        assert(out.samples[2*i]==n*2048 && out.samples[2*i+1]==-n*4096);
    }
    assert(pcm.used==32 && pcm.read==480 && !pcm.fraction);
    assert(playback_pcm_read_q31(&pcm,out.samples,64)==32);
    for(unsigned i=32;i<64;++i)assert(!out.samples[2*i] && !out.samples[2*i+1]);
    assert(!pcm.used);
    assert(playback_pcm_reset(&pcm,1024,true)==false);
    assert(playback_pcm_reset(&pcm,8192,true));
    assert(playback_pcm_read_q31(&pcm,out.samples,480)==0 && pcm.priming==4320);
    for(unsigned i=0;i<960;++i)assert(!out.samples[i]);
}

static void fractional_precision(void) {
    static const uint32_t rates[]={654720,655360,656000,9*65536,11*65536};
    float input[2048];
    for(unsigned i=0;i<1024;++i) {
        input[2*i]=(float)(sin(i*.13)*0x1p-20);
        input[2*i+1]=(float)(cos(i*.07)*0x1p-21);
    }
    for(unsigned rate=0;rate<sizeof(rates)/sizeof(*rates);++rate) {
        assert(playback_pcm_reset(&pcm,2048,false));
        playback_pcm_push_float(&pcm,input,1024);pcm.fraction=327680;
        assert(playback_pcm_prepare_resample(&pcm,&work,480,rates[rate]));
        int32_t output[960];playback_pcm_render_q31(&work,output);
        unsigned nonzero_low=0;
        for(unsigned i=64;i<480;++i) {
            const uint32_t denominator=655360;
            uint32_t position=work.fraction+i*work.rate;
            uint32_t phase=(position%denominator)*PLAYBACK_FILTER_PHASES;
            unsigned bin=phase/denominator;
            double blend=(double)(phase%denominator)/denominator;
            uint32_t center=PLAYBACK_FILTER_TAPS+position/denominator;
            unsigned filter=work.rate>denominator+denominator/20?1:0;
            for(unsigned ch=0;ch<2;++ch) {
                double expected=0;
                for(unsigned tap=0;tap<PLAYBACK_FILTER_TAPS;++tap) {
                    double c=playback_filter[filter][bin][tap]+blend*
                        ((double)playback_filter[filter][bin+1][tap]-playback_filter[filter][bin][tap]);
                    expected+=c*work.samples[2*(center-tap)+ch];
                }
                long long wanted=llround(expected*2147483648.0);
                assert(llabs((long long)output[2*i+ch]-wanted)<=1);
                if(output[2*i+ch] && ((uint32_t)output[2*i+ch]&65535))++nonzero_low;
            }
        }
        assert(nonzero_low>700); // An S16 intermediate would turn this quiet fixture into silence.
    }
}

static void inspect_keeps_its_q15_contract(void) {
    playback_inspect_t inspect={0};
    int32_t input[]={0,2048,-2048,INT32_MIN,INT32_MAX};
    playback_inspect_samples_q31(&inspect,input,5,123);
    assert(inspect.stats[4]==5 && inspect.stats[5]==4 && inspect.stats[6]==32768 && inspect.stats[7]==123);
    playback_inspect_samples_q31(&inspect,input+1,2,124);
    assert(inspect.stats[4]==7 && inspect.stats[5]==6 && !inspect.stats[6] && inspect.stats[7]==124);
}

static float past(unsigned offset) {
    assert(offset && offset<=pcm.valid_past);
    return pcm.samples[2*((pcm.read+pcm.capacity-offset)%pcm.capacity)];
}

static void retained_raw_history(void) {
    float input[40];
    int32_t output[32];
    for(unsigned i=0;i<20;++i) {input[2*i]=(float)(i+1)/32;input[2*i+1]=-input[2*i];}
    assert(playback_pcm_reset(&pcm,16,false));
    uint32_t generation=pcm.generation;
    assert(generation && !pcm.valid_past);
    playback_pcm_push_float(&pcm,input,8);
    assert(playback_pcm_read_q31(&pcm,output,6)==6);
    assert(pcm.valid_past==6 && pcm.used==2 && pcm.generation==generation);
    for(unsigned i=1;i<=6;++i)assert(past(i)==(float)(7-i)/32);
    playback_pcm_push_float(&pcm,input+16,6);
    assert(pcm.valid_past==6 && pcm.used==8 && pcm.generation==generation);
    playback_pcm_push_float(&pcm,input+28,4); // Wrap overwrites only the oldest consumed frames.
    assert(pcm.valid_past==4 && pcm.used==12 && pcm.generation==generation);
    for(unsigned i=1;i<=4;++i)assert(past(i)==(float)(7-i)/32);
    playback_pcm_push_float(&pcm,input+36,2);
    assert(pcm.valid_past==2 && pcm.used==14 && pcm.generation==generation);
    assert(!playback_pcm_consume(&pcm,15));
    assert(pcm.valid_past==2 && pcm.used==14 && pcm.read==6 && pcm.generation==generation);
    assert(playback_pcm_consume(&pcm,10));
    assert(pcm.valid_past==12 && pcm.used==4 && pcm.read==0 && pcm.generation==generation);
    for(unsigned i=1;i<=12;++i)assert(past(i)==(float)(17-i)/32);
    assert(playback_pcm_consume(&pcm,0));
    assert(pcm.valid_past==12 && pcm.generation==generation);
    playback_pcm_push_float(&pcm,input,20);
    assert(!pcm.valid_past && pcm.used==16 && pcm.generation!=generation);
    generation=pcm.generation;
    assert(playback_pcm_limit(&pcm,4)==12 && !pcm.valid_past && pcm.generation!=generation);
    generation=pcm.generation;
    assert(playback_pcm_limit(&pcm,8)==0 && pcm.generation==generation);
    assert(playback_pcm_consume(&pcm,4) && pcm.valid_past==4);
    playback_pcm_clear(&pcm,16);
    assert(!pcm.valid_past && !pcm.used && pcm.generation!=generation);
    generation=pcm.generation;
    assert(!playback_pcm_reset(&pcm,0,false) && pcm.generation==generation);
    playback_pcm_clear(&pcm,0);
    assert(pcm.generation==generation);
    assert(playback_pcm_reset(&pcm,16,false) && pcm.generation!=generation);
    pcm.generation=UINT32_MAX;
    playback_pcm_clear(&pcm,16);
    assert(pcm.generation==1 && !pcm.valid_past);
}

static void resampled_raw_history(void) {
    float input[1200]={0};
    int32_t output[32];
    assert(playback_pcm_reset(&pcm,1024,false));
    uint32_t generation=pcm.generation;
    playback_pcm_push_float(&pcm,input,600);
    assert(playback_pcm_prepare_resample(&pcm,&work,480,655360));
    assert(pcm.valid_past==480 && pcm.used==120 && pcm.generation==generation);
    assert(!playback_pcm_prepare_resample(&pcm,&work,480,9u*65536u));
    assert(pcm.valid_past==480 && pcm.used==120 && pcm.generation==generation);
    assert(playback_pcm_reset(&pcm,1024,false));
    generation=pcm.generation;
    playback_pcm_push_float(&pcm,input,432);
    pcm.fraction=634880;
    uint32_t missing=0;
    assert(playback_pcm_prepare_resample_fill(&pcm,&work,480,9u*65536u,&missing));
    assert(missing==1 && !pcm.valid_past && pcm.used==0 && pcm.generation!=generation);
    assert(pcm.fraction==634880); // A missing lookahead retires raw matching, not FIR phase.
    assert(memcmp(pcm.history,work.samples+432*2,sizeof(pcm.history))==0);
    assert(playback_pcm_reset(&pcm,1024,false));
    generation=pcm.generation;
    playback_pcm_push_float(&pcm,input,100);
    assert(playback_pcm_prepare_resample_fill(&pcm,&work,480,655360,&missing));
    assert(missing==380 && !pcm.valid_past && !pcm.used && pcm.read==100 && pcm.generation!=generation);
    assert(!pcm.fraction && memcmp(pcm.history,work.samples+480*2,sizeof(pcm.history))==0);
    assert(playback_pcm_reset(&pcm,8192,true));
    generation=pcm.generation;
    playback_pcm_push_float(&pcm,input,24);
    assert(playback_pcm_read_q31(&pcm,output,16)==0 && !pcm.valid_past && pcm.used==24);
    assert(pcm.generation==generation); // Deliberate priming is not a source shortage.
}

static void raw_shortage_retires_history(void) {
    float input[32];
    int32_t output[32];
    for(unsigned i=0;i<16;++i) {input[2*i]=(i+1)/32.0f;input[2*i+1]=-input[2*i];}
    assert(playback_pcm_reset(&pcm,64,false));
    uint32_t generation=pcm.generation;
    playback_pcm_push_float(&pcm,input,16);
    assert(playback_pcm_read_q31(&pcm,output,8)==8 && pcm.valid_past==8 && pcm.generation==generation);
    assert(playback_pcm_read_q31(&pcm,output,16)==8 && !pcm.valid_past && !pcm.used && pcm.generation!=generation);
    for(unsigned i=0;i<8;++i)assert(output[2*i]==(int32_t)(i+9)*67108864 && output[2*i+1]==-output[2*i]);
    for(unsigned i=16;i<32;++i)assert(!output[i]);
    assert(memcmp(pcm.history+(PLAYBACK_FILTER_TAPS-16)*2,input,sizeof(input))==0);
    generation=pcm.generation;
    playback_pcm_push_float(&pcm,input,16);
    assert(playback_pcm_read_q31(&pcm,output,8)==8 && pcm.valid_past==8 && pcm.generation==generation);
}

int main(void) {
    numeric_boundaries();rounding_neighbors();ring_and_channels();fractional_precision();inspect_keeps_its_q15_contract();
    retained_raw_history();resampled_raw_history();raw_shortage_retires_history();
    puts("Q31 PCM: precision, stereo, priming, FIR, scratch bound and retained raw history passed");
    return 0;
}
