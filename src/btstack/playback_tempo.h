// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PLAYBACK_TEMPO_H
#define AIRPODS_PLAYBACK_TEMPO_H
#include "playback_pcm.h"

#define PLAYBACK_TEMPO_HOP 240u
#define PLAYBACK_TEMPO_FRAMES 480u
#define PLAYBACK_TEMPO_MIN_LAG (-2160)
#define PLAYBACK_TEMPO_MAX_LAG 240
#define PLAYBACK_TEMPO_PAST 2240u
#define PLAYBACK_TEMPO_FUTURE 768u
#define PLAYBACK_TEMPO_ANALYSIS_VALUES 3008u
#define PLAYBACK_TEMPO_SELECTED_FRAMES 1074u

typedef const float *(*playback_tempo_reader_t)(void *,int32_t);
typedef struct {uint32_t fraction;int16_t offset;bool active;float history[64];} playback_tempo_state_t;
typedef struct {
  union {
    struct {
        int16_t samples[PLAYBACK_TEMPO_ANALYSIS_VALUES];
        uint32_t energy[2][377];
    } analysis;
    float selected[PLAYBACK_TEMPO_SELECTED_FRAMES*2];
  };
  float history[64];
} playback_tempo_work_t;
_Static_assert(sizeof(playback_tempo_work_t)<=sizeof(playback_resample_t),"Tempo must reuse the existing resampler workspace");
typedef struct {
    uint32_t fraction;
    uint16_t consumed,valid_past,future;
    int16_t previous_offset,coarse[2],selected[2],base[3];
    uint16_t count[3];
    int8_t fine_lo[2],fine_hi[2];
    uint8_t interval;
    bool entry,finish,searched,chosen,rendered;
} playback_tempo_job_t;

// Caller validates epoch/source identity at every locked stage. These never consume PCM.
static inline int playback_tempo_abs(int value) {return value<0?-value:value;}
static inline int playback_tempo_mod(int value,int divisor) {int r=value%divisor;return r<0?r+divisor:r;}
static inline int playback_tempo_min_lag(const playback_tempo_job_t *job,int nominal) {
    int minimum=32-(int)job->valid_past-nominal;
    return minimum>PLAYBACK_TEMPO_MIN_LAG?minimum:PLAYBACK_TEMPO_MIN_LAG;
}
static inline int playback_tempo_max_lag(const playback_tempo_job_t *job,int nominal,int length) {
    int maximum=(int)job->future-length-nominal;
    return maximum<PLAYBACK_TEMPO_MAX_LAG?maximum:PLAYBACK_TEMPO_MAX_LAG;
}
static inline bool playback_tempo_prepare(playback_tempo_work_t *work,playback_tempo_job_t *job,
    const playback_tempo_state_t *state,uint8_t interval,uint16_t consumed,uint32_t fraction,uint32_t valid_past,
    uint32_t future,playback_tempo_reader_t read,void *context) {
    if(!work || !job || !state || !read || (interval!=9 && interval!=10 && interval!=11) ||
       (interval==10?consumed!=480:consumed<384 || consumed>528 || (consumed&1)) ||
       fraction>=655360u || valid_past<32 || valid_past>PLAYBACK_PCM_FRAMES ||
       future<480 || future>PLAYBACK_PCM_FRAMES || (interval==10 && !state->active))return false;
    uint32_t needed=interval==10?480u:(uint32_t)consumed+PLAYBACK_TEMPO_MAX_LAG;
    if(state->active && state->fraction!=fraction)return false;
    int previous=state->active?state->offset:0;
    if(previous<PLAYBACK_TEMPO_MIN_LAG || previous>PLAYBACK_TEMPO_MAX_LAG ||
       previous-32<-(int)valid_past || previous+240>(int)future || future<needed)return false;
    *job=(playback_tempo_job_t){.fraction=fraction,.consumed=consumed,
        .valid_past=(uint16_t)(valid_past<PLAYBACK_TEMPO_PAST?valid_past:PLAYBACK_TEMPO_PAST),
        .future=(uint16_t)needed,.previous_offset=(int16_t)previous,.interval=interval,
        .entry=!state->active,.finish=interval==10};
    if(state->active)memcpy(work->history,state->history,sizeof(work->history));
    else for(unsigned frame=0;frame<32;++frame) {
        const float *stereo=read(context,(int)frame-32);
        work->history[2*frame]=playback_pcm_finite(stereo[0]);
        work->history[2*frame+1]=playback_pcm_finite(stereo[1]);
    }
    if(job->finish)return true;
    float peak=0;
    for(int position=-(int)PLAYBACK_TEMPO_PAST;position<(int)job->future;position+=4)
        for(unsigned pair=0;pair<2;++pair) {
            int p=position+(int)pair;
            if(p<-(int)job->valid_past || p>=(int)job->future)continue;
            const float *stereo=read(context,p);
            for(unsigned channel=0;channel<2;++channel) {
                float value=fabsf(playback_pcm_finite(stereo[channel]));
                if(value>peak)peak=value;
            }
        }
    int exponent=0;
    if(peak>0)frexpf(peak,&exponent);
    exponent=10-exponent;if(exponent>120)exponent=120;if(exponent< -120)exponent=-120;
    float gain=ldexpf(1.0f,exponent);
    // Keep adjacent full-rate samples, then skip two: pure 12 kHz is not invisible.
    for(int position=-(int)PLAYBACK_TEMPO_PAST;position<(int)PLAYBACK_TEMPO_FUTURE;position+=4)
        for(unsigned pair=0;pair<2;++pair) {
            int p=position+(int)pair;
            const float *stereo=p>=-(int)job->valid_past && p<(int)job->future?read(context,p):NULL;
            for(unsigned channel=0;channel<2;++channel) {
                float value=stereo?playback_pcm_finite(stereo[channel])*gain:0;
                if(value>1024)value=1024;
                if(value< -1024)value=-1024;
                unsigned index=(unsigned)(position+(int)PLAYBACK_TEMPO_PAST)+pair*2+channel;
                work->analysis.samples[index]=(int16_t)value;
            }
        }
    return true;
}
// Q10 correlation bounds each 120-term window below 2^28; only full prefixes need uint32.
static inline int32_t playback_tempo_dot_pair(const int16_t *a,const int16_t *b,int32_t sum) {
#if defined(__ARM_FEATURE_DSP)
    int32_t left,right;
    memcpy(&left,a,4);memcpy(&right,b,4);
    return __builtin_arm_smlad(left,right,sum);
#else
    return sum+(int32_t)a[0]*b[0]+(int32_t)a[1]*b[1];
#endif
}
static inline void playback_tempo_prefix(playback_tempo_work_t *work) {
    work->analysis.energy[0][0]=work->analysis.energy[1][0]=0;
    for(unsigned block=0;block<752;++block) {
        const int16_t *v=work->analysis.samples+4*block;
        uint32_t energy=(uint32_t)playback_tempo_dot_pair(v+2,v+2,playback_tempo_dot_pair(v,v,0));
        unsigned parity=block&1,index=block/2;
        work->analysis.energy[parity][index+1]=work->analysis.energy[parity][index]+energy;
    }
}
static inline uint32_t playback_tempo_energy(const playback_tempo_work_t *work,unsigned block) {
    const uint32_t *prefix=work->analysis.energy[block&1];
    return prefix[block/2+30]-prefix[block/2];
}
static inline float playback_tempo_score(float dot,float a,float b) {
    if(a<1e-20f || b<1e-20f)return a<1e-20f && b<1e-20f?1.0f:0.0f;
    return dot>0?dot*dot/(a*b):0;
}
static inline float playback_tempo_analysis_score(const playback_tempo_work_t *work,
    unsigned reference,unsigned candidate,uint32_t reference_energy) {
    int32_t dot=0;
    const int16_t *a=work->analysis.samples+4*reference;
    const int16_t *b=work->analysis.samples+4*candidate;
    for(unsigned i=0;i<30;++i) {
        dot=playback_tempo_dot_pair(a,b,dot);
        dot=playback_tempo_dot_pair(a+2,b+2,dot);
        a+=8;b+=8;
    }
    return playback_tempo_score((float)dot,(float)reference_energy,
        (float)playback_tempo_energy(work,candidate));
}
static inline void playback_tempo_dot_four(const int16_t *a,const int16_t *b,int32_t dots[4]) {
    int32_t d0=0,d1=0,d2=0,d3=0;
    for(unsigned i=0;i<30;++i) {
        d0=playback_tempo_dot_pair(a,b,d0);
        d1=playback_tempo_dot_pair(a,b+4,d1);
        d2=playback_tempo_dot_pair(a,b+8,d2);
        d3=playback_tempo_dot_pair(a,b+12,d3);
        d0=playback_tempo_dot_pair(a+2,b+2,d0);
        d1=playback_tempo_dot_pair(a+2,b+6,d1);
        d2=playback_tempo_dot_pair(a+2,b+10,d2);
        d3=playback_tempo_dot_pair(a+2,b+14,d3);
        a+=8;b+=8;
    }
    dots[0]=d0;dots[1]=d1;dots[2]=d2;dots[3]=d3;
}
static inline int playback_tempo_search_hop(const playback_tempo_work_t *work,
    const playback_tempo_job_t *job,int reference,int nominal,int length) {
    int lo=playback_tempo_min_lag(job,nominal),hi=playback_tempo_max_lag(job,nominal,length);
    int residue=playback_tempo_mod(reference-nominal,4);
    int first=lo+playback_tempo_mod(residue-lo,4),best=first;
    unsigned shift=(unsigned)playback_tempo_mod(-reference,4);
    unsigned reference_block=(unsigned)(reference+(int)shift+(int)PLAYBACK_TEMPO_PAST)/4;
    uint32_t reference_energy=playback_tempo_energy(work,reference_block);
    float score=-1;
    int lag=first;
    for(;lag+12<=hi;lag+=16) {
        unsigned block=(unsigned)(nominal+lag+(int)shift+(int)PLAYBACK_TEMPO_PAST)/4;
        int32_t dots[4];
        playback_tempo_dot_four(work->analysis.samples+4*reference_block,
            work->analysis.samples+4*block,dots);
        for(unsigned i=0;i<4;++i) {
            int candidate_lag=lag+(int)i*4;
            float candidate=playback_tempo_score((float)dots[i],(float)reference_energy,
                (float)playback_tempo_energy(work,block+i));
            if(candidate>score+1e-6f || (fabsf(candidate-score)<=1e-6f &&
               playback_tempo_abs(candidate_lag)<playback_tempo_abs(best)))
                {score=candidate;best=candidate_lag;}
        }
    }
    for(;lag<=hi;lag+=4) {
        float candidate=playback_tempo_analysis_score(work,reference_block,
            (unsigned)(nominal+lag+(int)shift+(int)PLAYBACK_TEMPO_PAST)/4,reference_energy);
        if(candidate>score+1e-6f || (fabsf(candidate-score)<=1e-6f && playback_tempo_abs(lag)<playback_tempo_abs(best)))
            {score=candidate;best=lag;}
    }
    return best;
}
static inline void playback_tempo_search(playback_tempo_work_t *work,playback_tempo_job_t *job) {
    int hop=(int)job->consumed/2;
    if(!job->finish) {
        playback_tempo_prefix(work);
        job->coarse[0]=(int16_t)(job->entry?240-hop:
            playback_tempo_search_hop(work,job,job->previous_offset,hop-240,480));
        job->coarse[1]=(int16_t)playback_tempo_search_hop(work,job,hop+job->coarse[0],2*hop-240,240);
    }
    job->searched=true;
}
static inline unsigned playback_tempo_piece(unsigned piece) {return piece==0?0:piece==1?272u:790u;}
static inline bool playback_tempo_select(playback_tempo_work_t *work,playback_tempo_job_t *job,
    playback_tempo_reader_t read,void *context) {
    if(!job->searched || !read)return false;
    int hop=(int)job->consumed/2;
    job->base[0]=job->previous_offset-32;job->count[0]=272;
    if(job->finish) {job->base[1]=-32;job->count[1]=512;job->count[2]=0;}
    else for(unsigned i=0;i<2;++i) {
        int nominal=(int)(i+1)*hop-240,margin=i?6:3;
        int lo=playback_tempo_min_lag(job,nominal)-job->coarse[i];
        int hi=playback_tempo_max_lag(job,nominal,i?240:480)-job->coarse[i];
        if(lo< -margin)lo=-margin;
        if(hi>margin)hi=margin;
        if(job->entry && i==0)lo=hi=0;
        if(lo>hi)return false;
        job->fine_lo[i]=(int8_t)lo;job->fine_hi[i]=(int8_t)hi;
        job->base[i+1]=(int16_t)(nominal+job->coarse[i]+lo-32);
        job->count[i+1]=(uint16_t)((i?240:480)+32+hi-lo);
    }
    for(unsigned i=0;i<3;++i) {
        if(job->base[i]<-(int)job->valid_past || job->base[i]+job->count[i]>(int)job->future)return false;
    }
    for(unsigned i=0;i<3;++i)for(unsigned frame=0;frame<job->count[i];++frame) {
        const float *stereo=read(context,job->base[i]+(int)frame);
        work->selected[2*(playback_tempo_piece(i)+frame)]=playback_pcm_finite(stereo[0]);
        work->selected[2*(playback_tempo_piece(i)+frame)+1]=playback_pcm_finite(stereo[1]);
    }
    job->chosen=true;
    return true;
}
static inline float playback_tempo_sample(const playback_tempo_work_t *work,const playback_tempo_job_t *job,
    unsigned piece,int position,unsigned channel) {
    return work->selected[2*(playback_tempo_piece(piece)+(unsigned)(position-job->base[piece]))+channel];
}
static inline float playback_tempo_float_score(const playback_tempo_work_t *work,const playback_tempo_job_t *job,
    unsigned reference_piece,int reference,unsigned candidate_piece,int candidate) {
    float dot=0,a=0,b=0;
    for(unsigned i=0;i<240;++i)for(unsigned channel=0;channel<2;++channel) {
        float x=playback_tempo_sample(work,job,reference_piece,reference+(int)i,channel);
        float y=playback_tempo_sample(work,job,candidate_piece,candidate+(int)i,channel);
        dot+=x*y;a+=x*x;b+=y*y;
    }
    return playback_tempo_score(dot,a,b);
}
static inline int playback_tempo_fine(const playback_tempo_work_t *work,const playback_tempo_job_t *job,
    unsigned reference_piece,int reference,unsigned candidate_piece,int nominal,unsigned index,int shift) {
    int lo=shift-3,hi=shift+3;
    if(lo<job->fine_lo[index])lo=job->fine_lo[index];
    if(hi>job->fine_hi[index])hi=job->fine_hi[index];
    int best=lo;float score=-1;
    for(int delta=lo;delta<=hi;++delta) {
        float candidate=playback_tempo_float_score(work,job,reference_piece,reference,candidate_piece,
            nominal+job->coarse[index]+delta);
        if(candidate>score+1e-6f || (fabsf(candidate-score)<=1e-6f &&
            playback_tempo_abs(job->coarse[index]+delta)<playback_tempo_abs(job->coarse[index]+best)))
            {score=candidate;best=delta;}
    }
    return job->coarse[index]+best;
}
// Assemble the time-scaled float stream first, then apply the source fractional FIR once.
// The raw 32-frame tail crosses job boundaries; no already-filtered sample is filtered again.
static inline void playback_tempo_render_q31(playback_tempo_work_t *work,playback_tempo_job_t *job,
    playback_encoder_pcm_t *stage) {
    if(!job->chosen)return;
    int hop=(int)job->consumed/2;
    if(!job->finish) {
        job->selected[0]=(int16_t)(job->entry?job->coarse[0]:
            playback_tempo_fine(work,job,0,job->previous_offset,1,hop-240,0,0));
        int shift=job->selected[0]-job->coarse[0];
        job->selected[1]=(int16_t)playback_tempo_fine(work,job,1,hop+job->selected[0],2,2*hop-240,1,shift);
    }
    memcpy(stage->f32,work->history,sizeof(work->history));
    for(unsigned block=0;block<2;++block)for(unsigned i=0;i<240;++i)for(unsigned channel=0;channel<2;++channel) {
        float x=(float)i/239.0f,weight=x*x*(3-2*x),a,b;
        if(job->finish) {
            b=playback_tempo_sample(work,job,1,(int)(block*240+i),channel);
            a=block?b:playback_tempo_sample(work,job,0,job->previous_offset+(int)i,channel);
        } else {
            a=block?playback_tempo_sample(work,job,1,hop+job->selected[0]+(int)i,channel):
                playback_tempo_sample(work,job,0,job->previous_offset+(int)i,channel);
            b=playback_tempo_sample(work,job,block+1,(int)(block+1)*hop-240+job->selected[block]+(int)i,channel);
        }
        stage->f32[2*(32+block*240+i)+channel]=a+(b-a)*weight;
    }
    memcpy(work->history,stage->f32+480*2,sizeof(work->history));
    float coefficients[33];
    if(job->fraction) {
        uint32_t phase=job->fraction*PLAYBACK_FILTER_PHASES;
        unsigned bin=phase/655360u;float blend=(float)(phase%655360u)/655360u;
        for(unsigned tap=0;tap<33;++tap)coefficients[tap]=playback_filter[0][bin][tap]+blend*
            (playback_filter[0][bin+1][tap]-playback_filter[0][bin][tap]);
    }
    // Output writes trail every still-needed raw tap, so the 4096-byte staging bank is safe in-place.
    for(unsigned frame=0;frame<480;++frame) {
        float left=stage->f32[2*(frame+16)],right=stage->f32[2*(frame+16)+1];
        if(job->fraction) {
            left=right=0;
            for(unsigned tap=0;tap<33;++tap) {
                float coefficient=coefficients[tap];unsigned index=2*(frame+32-tap);
                left+=coefficient*stage->f32[index];right+=coefficient*stage->f32[index+1];
            }
        }
        stage->q31[2*frame]=playback_pcm_quantize_q31(left);
        stage->q31[2*frame+1]=playback_pcm_quantize_q31(right);
    }
    job->rendered=true;
}
static inline void playback_tempo_commit(playback_tempo_state_t *state,const playback_tempo_job_t *job,
    const playback_tempo_work_t *work) {
    if(job->rendered) {
        state->fraction=job->fraction;state->offset=job->finish?0:job->selected[1];state->active=!job->finish;
        memcpy(state->history,work->history,sizeof(state->history));
    }
}

#endif
