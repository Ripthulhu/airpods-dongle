#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../src/btstack/playback_pcm.h"
static playback_pcm_t pcm;

// Keep the original scalar convolution as an oracle for the unity-rate fast path.
static void unity_reference(const playback_resample_t *work,int16_t *dst) {
    const uint32_t denominator=655360u;
    for(uint32_t frame=0;frame<work->frames;++frame) {
        uint32_t position=work->fraction+frame*work->rate;
        uint32_t phase=(position%denominator)*PLAYBACK_FILTER_PHASES;
        unsigned bin=phase/denominator;
        float blend=(float)(phase%denominator)/(float)denominator;
        uint32_t center=PLAYBACK_FILTER_TAPS+position/denominator;
        float left=0,right=0;
        if(!phase) {
            left=work->samples[2*(center-PLAYBACK_FILTER_DELAY)];
            right=work->samples[2*(center-PLAYBACK_FILTER_DELAY)+1];
        } else for(unsigned tap=0;tap<PLAYBACK_FILTER_TAPS;++tap) {
            float c=playback_filter[0][bin][tap]+blend*
                (playback_filter[0][bin+1][tap]-playback_filter[0][bin][tap]);
            left+=c*work->samples[2*(center-tap)];
            right+=c*work->samples[2*(center-tap)+1];
        }
        dst[2*frame]=playback_pcm_quantize(left);dst[2*frame+1]=playback_pcm_quantize(right);
    }
}

static void unity_exact_output(void) {
    playback_resample_t work={.frames=2,.rate=655360u};
    int16_t expected[2048],actual[2048];
    for(unsigned i=0;i<sizeof(work.samples)/sizeof(work.samples[0]);++i)
        work.samples[i]=(float)((int)(i*7411u%131071u)-65536)/32768;
    for(work.fraction=0;work.fraction<655360u;++work.fraction) {
        unity_reference(&work,expected);playback_pcm_render(&work,actual);
        assert(!memcmp(expected,actual,work.frames*2*sizeof(int16_t)));
    }
    const unsigned sizes[]={1,128,480,1024},phases[]={0,1,327679,327680,655359};
    for(unsigned size=0;size<4;++size)for(unsigned phase=0;phase<5;++phase) {
        work.frames=sizes[size];work.fraction=phases[phase];
        unity_reference(&work,expected);playback_pcm_render(&work,actual);
        assert(!memcmp(expected,actual,work.frames*2*sizeof(int16_t)));
    }
    const float special[]={NAN,INFINITY,-INFINITY,0,-0.0f,1,-1,0x1p127f,-0x1p127f,0x1p-149f};
    for(unsigned i=0;i<sizeof(work.samples)/sizeof(work.samples[0]);++i)
        work.samples[i]=special[i%10];
    for(unsigned phase=0;phase<5;++phase) {
        work.fraction=phases[phase];
        unity_reference(&work,expected);playback_pcm_render(&work,actual);
        assert(!memcmp(expected,actual,work.frames*2*sizeof(int16_t)));
    }
}

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
    unity_exact_output();
    spectral_continuity();
    puts("float PCM: S16 roundtrip, precision, unity-phase equivalence, spectrum and numeric boundaries passed");
}
