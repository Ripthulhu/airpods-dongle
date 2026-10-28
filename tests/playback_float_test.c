#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../src/btstack/playback_pcm.h"
static playback_pcm_t pcm;
static void spectral_continuity(void) {
    float input[4096*2];int16_t output[480*2];
    const double pi=3.14159265358979323846;
    const unsigned tones[]={1000,12000,18000,20000};
    for(unsigned tone=0;tone<4;++tone) {
        assert(playback_pcm_reset(&pcm,8192,false));
        double omega=2*pi*tones[tone]/48000.0;
        for(unsigned i=0;i<4096;++i) {input[2*i]=(float)(.25*sin(omega*i));input[2*i+1]=-input[2*i];}
        playback_pcm_push_float(&pcm,input,4096);pcm.fraction=327680;
        double error=0,reference=0;
        for(unsigned block=0;block<6;++block) {
            assert(playback_pcm_read_scaled(&pcm,output,480,655360));
            for(unsigned i=64;i<480;++i) {
                double expected=.25*sin(omega*(block*480+i-PLAYBACK_FILTER_DELAY+.5));
                double actual=(double)output[2*i]/32768;
                error+=(actual-expected)*(actual-expected);reference+=expected*expected;
                assert(output[2*i+1]==-output[2*i]);
            }
        }
        assert(10*log10(reference/error)>50); // Linear interpolation fails this at high frequencies.
    }
}
int main(void) {
    for(int i=INT16_MIN;i<=INT16_MAX;++i)
        assert(playback_pcm_quantize((float)i/32768.0f)==i);
    assert(playback_pcm_quantize(NAN)==0 && playback_pcm_quantize(INFINITY)==0);
    assert(playback_pcm_quantize(-INFINITY)==0);
    assert(playback_pcm_quantize(2.0f)==INT16_MAX && playback_pcm_quantize(-2.0f)==INT16_MIN);
    assert(playback_pcm_quantize(0.5f/32768)==1 && playback_pcm_quantize(-0.5f/32768)==-1);
    assert(playback_pcm_reset(&pcm,16,false));
    float input[]={0.12345678f,-0.23456789f,NAN,INFINITY,2.0f,-2.0f,0.0f,-0.0f};
    playback_pcm_push_float(&pcm,input,4);
    assert(pcm.samples[0]==input[0] && pcm.samples[1]==input[1]);
    assert(pcm.samples[0]!=(float)playback_pcm_quantize(input[0])/32768.0f);
    assert(pcm.samples[2]==0 && pcm.samples[3]==0 && pcm.samples[4]==2.0f);
    int16_t output[8];assert(playback_pcm_read(&pcm,output,4)==4);
    assert(output[0]==playback_pcm_quantize(input[0]) && output[1]==playback_pcm_quantize(input[1]));
    assert(output[4]==INT16_MAX && output[5]==INT16_MIN && !output[6] && !output[7]);
    spectral_continuity();
    puts("float PCM: full S16 roundtrip, precision retention, nonfinite input, rounding and saturation passed");
}
