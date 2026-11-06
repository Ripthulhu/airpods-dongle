// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include "../src/btstack/playback_tempo.h"

static playback_tempo_work_t work;
static playback_encoder_pcm_t stage;
#define output stage.q31
static playback_resample_t oracle_work;
static playback_q31_t oracle[480*2];
static double total_us[4],maximum_us[4];
static unsigned timing_calls;
static void timing(unsigned stage,clock_t before,clock_t after) {
    double us=(double)(after-before)*1000000/CLOCKS_PER_SEC;
    total_us[stage]+=us;if(us>maximum_us[stage])maximum_us[stage]=us;
}
static float benchmark_source[8192*2];
typedef struct {int64_t position;uint32_t past,future;double frequency,amplitude;unsigned reads;bool precomputed;} source_t;
static const float *source_read(void *context,int32_t relative) {
    static float stereo[2];
    source_t *source=context;
    assert(relative>=-(int)source->past && relative<(int)source->future);
    ++source->reads;
    if(source->precomputed)return benchmark_source+2*(unsigned)((source->position+relative+8192*32)%8192);
    double sample=source->frequency?source->amplitude*sin((source->position+relative)*6.2831853071795864769*source->frequency/48000.0):source->amplitude;
    stereo[0]=(float)sample;stereo[1]=(float)-sample;return stereo;
}
static void normal_oracle(source_t source,unsigned fraction) {
    oracle_work=(playback_resample_t){.frames=480,.rate=655360,.fraction=fraction};
    source.past=PLAYBACK_TEMPO_PAST;source.future=768;
    for(unsigned i=0;i<480+PLAYBACK_FILTER_TAPS;++i)for(unsigned channel=0;channel<2;++channel)
        oracle_work.samples[2*i+channel]=source_read(&source,(int)i-(int)PLAYBACK_FILTER_TAPS)[channel];
    playback_pcm_render_q31(&oracle_work,oracle);
}
static void run_frame_consumed(source_t *source,playback_tempo_state_t *state,unsigned interval,unsigned consumed,unsigned fraction) {
    playback_tempo_job_t job;
    playback_tempo_state_t saved=*state;
    source->future=consumed+240;
    clock_t t=clock();
    assert(playback_tempo_prepare(&work,&job,state,(uint8_t)interval,(uint16_t)consumed,fraction,source->past,
        source->future,source_read,source));
    clock_t next=clock();timing(0,t,next);t=next;
    playback_tempo_search(&work,&job);
    next=clock();timing(1,t,next);t=next;
    assert(playback_tempo_select(&work,&job,source_read,source));
    next=clock();timing(2,t,next);t=next;
    playback_tempo_render_q31(&work,&job,&stage);
    next=clock();timing(3,t,next);++timing_calls;
    assert(job.rendered && !memcmp(&saved,state,sizeof saved));
    if(!source->precomputed)for(unsigned i=0;i<480;++i)assert(output[2*i]==-output[2*i+1]);
    playback_tempo_commit(state,&job,&work);
    source->position+=job.consumed;
    source->past+=job.consumed;
    if(source->past>PLAYBACK_TEMPO_PAST)source->past=PLAYBACK_TEMPO_PAST;
}
static void run_frame(source_t *source,playback_tempo_state_t *state,unsigned interval,unsigned fraction) {
    run_frame_consumed(source,state,interval,48*interval,fraction);
}
static void tone_consumed(double hz,unsigned interval,unsigned consumed,unsigned fraction,double amplitude) {
    source_t source={.past=33,.frequency=hz,.amplitude=amplitude};
    playback_tempo_state_t state={0};
    double previous=0,first=0,last=0;unsigned crossings=0;
    for(unsigned block=0;block<240;++block) {
        source_t before=source;
        const unsigned debt_cases[]={384,446,450,528};
        run_frame_consumed(&source,&state,interval,consumed?consumed:debt_cases[block%4],fraction);
        if(!block) {
            normal_oracle(before,fraction);
            for(unsigned i=0;i<240*2;++i)assert(output[i]==oracle[i]);
        }
        for(unsigned i=0;i<480;++i) {
            double value=output[2*i];unsigned position=block*480+i;
            if(block>=4 && previous<=0 && value>0) {
                double x=position-1-previous/(value-previous);
                if(!crossings)first=x;
                last=x;++crossings;
            }
            previous=value;
        }
    }
    double measured=(crossings-1)*48000/(last-first);
    printf("tempo tone %.1f interval%u consumed%u fraction%u: %.6f Hz\n",hz,interval,consumed,fraction,measured);
    assert(crossings>10 && fabs(measured/hz-1)<0.004);
    source_t before=source;
    run_frame(&source,&state,10,fraction);
    assert(!state.active && !state.offset);
    normal_oracle(before,fraction);
    for(unsigned i=272;i<480;++i) {
        // After the 32-tap boundary clears, handoff exactly follows the normal FIR/fraction timeline.
        assert(output[2*i]==oracle[2*i] && output[2*i+1]==oracle[2*i+1]);
    }
}
static void tone(double hz,unsigned interval,unsigned fraction,double amplitude) {
    tone_consumed(hz,interval,48*interval,fraction,amplitude);
}
static void dc_precision(void) {
    for(unsigned interval=9;interval<=11;interval+=2) {
        source_t source={.past=33,.frequency=0,.amplitude=0.125+0x1p-20};
        playback_tempo_state_t state={0};
        for(unsigned block=0;block<24;++block) {
            run_frame(&source,&state,interval,0);
            for(unsigned i=0;i<480;++i)assert(output[2*i]==268437504 && output[2*i+1]==-268437504);
        }
    }
}
static void short_join(unsigned interval,unsigned blocks) {
    source_t source={.past=33,.frequency=20,.amplitude=0.25};
    playback_tempo_state_t state={0};
    for(unsigned i=0;i<blocks;++i)run_frame(&source,&state,interval,0);
    int offset=state.offset;
    double previous=(double)output[958]/2147483648.0;
    source_t before=source;
    run_frame(&source,&state,10,0);
    normal_oracle(before,0);
    double maximum_step=0,energy=0,normal_energy=0,peak=0;
    for(unsigned i=0;i<240;++i) {
        double value=(double)output[2*i]/2147483648.0;
        double normal=(double)oracle[2*i]/2147483648.0;
        double step=fabs(value-previous);if(step>maximum_step)maximum_step=step;
        if(fabs(value)>peak)peak=fabs(value);
        energy+=value*value;normal_energy+=normal*normal;previous=value;
    }
    assert(offset!=0 && maximum_step<0.006 && peak<=0.250001);
    assert(!state.active);
    for(unsigned i=272*2;i<480*2;++i)assert(output[i]==oracle[i]);
    double midpoint_gain=fabs(cos(3.14159265358979323846*20*offset/48000));
    printf("short join interval%u wall_ms%u offset%d: max_step %.8f rms_ratio %.6f midpoint_envelope %.6f (%.3f dB)\n",
        interval,blocks*interval,offset,maximum_step,sqrt(energy/normal_energy),midpoint_gain,20*log10(midpoint_gain));
}
static void host_benchmark(void) {
    for(unsigned i=0;i<8192;++i) {
        benchmark_source[2*i]=(float)(0.2*sin(i*6.2831853071795864769*997/48000));
        benchmark_source[2*i+1]=(float)(0.15*sin(i*6.2831853071795864769*40/48000));
    }
    memset(total_us,0,sizeof total_us);memset(maximum_us,0,sizeof maximum_us);timing_calls=0;
    source_t source={.past=PLAYBACK_TEMPO_PAST,.precomputed=true};playback_tempo_state_t state={0};
    for(unsigned i=0;i<200;++i)run_frame(&source,&state,11,0);
    for(unsigned i=0;i<4;++i)printf("host-only ring-reader stage%u mean_us%.3f max_us%.3f calls%u\n",
        i,total_us[i]/timing_calls,maximum_us[i],timing_calls);
}
static void guards(void) {
    source_t source={.past=33,.future=768,.frequency=1000,.amplitude=0.25};
    playback_tempo_state_t state={0};playback_tempo_job_t job={0};
    assert(!playback_tempo_prepare(&work,&job,&state,8,384,0,33,768,source_read,&source));
    assert(!playback_tempo_prepare(&work,&job,&state,11,528,655360,33,768,source_read,&source));
    assert(!playback_tempo_prepare(&work,&job,&state,11,528,0,31,768,source_read,&source));
    assert(!playback_tempo_prepare(&work,&job,&state,11,528,0,33,767,source_read,&source));
    assert(!playback_tempo_prepare(&work,&job,&state,11,528,0,UINT32_MAX,768,source_read,&source));
    assert(!playback_tempo_prepare(&work,&job,&state,11,528,0,0x80000000u,768,source_read,&source));
    assert(!playback_tempo_prepare(&work,&job,&state,11,528,0,33,UINT32_MAX,source_read,&source));
    const unsigned invalid[]={0,383,385,529,65535};
    for(unsigned i=0;i<sizeof invalid/sizeof *invalid;++i)
        assert(!playback_tempo_prepare(&work,&job,&state,9,(uint16_t)invalid[i],0,33,768,source_read,&source));
    assert(!playback_tempo_prepare(&work,&job,&state,10,446,0,33,768,source_read,&source));
    assert(!source.reads);
    state=(playback_tempo_state_t){.active=true,.fraction=1,.offset=0};
    assert(!playback_tempo_prepare(&work,&job,&state,11,528,0,33,768,source_read,&source));
    assert(sizeof(work)<=sizeof(playback_resample_t));
}
int main(void) {
    setbuf(stdout,NULL);guards();dc_precision();
    const double tones[]={20,40,80,100,1000,12000,16000,20000};
    for(unsigned interval=9;interval<=11;interval+=2)
        for(unsigned i=0;i<sizeof tones/sizeof *tones;++i)tone(tones[i],interval,0,0.25);
    tone(1000,11,634880,0.25);tone(80,9,634880,0.00001);
    const unsigned consumed_cases[]={384,446,450,528};
    for(unsigned c=0;c<sizeof consumed_cases/sizeof *consumed_cases;++c)
        for(unsigned i=0;i<sizeof tones/sizeof *tones;++i)
            tone_consumed(tones[i],9,consumed_cases[c],634880,0.25);
    tone_consumed(20,9,0,634880,0.25);tone_consumed(40,9,0,634880,0.25);
    tone_consumed(1000,9,0,634880,0.25);tone_consumed(20000,9,0,634880,0.25);
    short_join(11,80);short_join(9,90);
    host_benchmark();
    puts("Tempo bounds, state isolation, stereo, deep-bass/treble pitch and fractional handoff passed");
}
