// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_DONGLE_PLAYBACK_PCM_H
#define AIRPODS_DONGLE_PLAYBACK_PCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "playback_filter.h"

static inline float playback_pcm_finite(float sample) {
    return isfinite(sample)?sample:0.0f;
}
static inline int16_t playback_pcm_quantize(float sample) {
    if(!isfinite(sample))return 0;
    if(sample>=32767.0f/32768.0f)return INT16_MAX;
    if(sample<= -1.0f)return INT16_MIN;
    float scaled=sample*32768.0f;
    return (int16_t)(scaled+(scaled<0?-0.5f:0.5f));
}

// One bounded stereo pool shared by USB input and the encoder. Callers serialize it.
#define PLAYBACK_PCM_FRAMES 8192u
#define PLAYBACK_PRIMING_FRAMES 4800u
#define PLAYBACK_USB_MIN_FRAMES 1440u
typedef struct {
    float samples[PLAYBACK_PCM_FRAMES * 2];
    uint32_t read;
    uint32_t used;
    uint32_t capacity;
    uint32_t dropped_pending;
    uint32_t priming;
    uint32_t fraction;
    float history[PLAYBACK_FILTER_TAPS*2];
    bool primed_on_wire;
} playback_pcm_t;

static inline bool playback_pcm_reset(playback_pcm_t *pcm, uint32_t capacity,
                                      bool prime) {
    if (capacity == 0 || capacity > PLAYBACK_PCM_FRAMES ||
        (prime && capacity < PLAYBACK_PRIMING_FRAMES)) return false;
    pcm->read = pcm->used = pcm->dropped_pending = 0;
    pcm->fraction = 0;
    memset(pcm->history,0,sizeof(pcm->history));
    pcm->capacity = capacity;
    pcm->priming = prime ? PLAYBACK_PRIMING_FRAMES : 0;
    pcm->primed_on_wire = false;
    return true;
}

// Changing input discards its PCM, without restarting the AirPods' warm stream.
static inline void playback_pcm_clear(playback_pcm_t *pcm, uint32_t capacity) {
    if (capacity == 0 || capacity > PLAYBACK_PCM_FRAMES) return;
    pcm->read = pcm->used = pcm->dropped_pending = 0;
    pcm->fraction = 0;
    memset(pcm->history,0,sizeof(pcm->history));
    pcm->capacity = capacity;
}

static inline void playback_pcm_sent(playback_pcm_t *pcm) {
    if (pcm->priming == 0 && !pcm->primed_on_wire) {
        // Startup losses must not become overdue work in the active stream.
        pcm->dropped_pending = 0;
        pcm->primed_on_wire = true;
    }
}

static inline void playback_pcm_push_data(playback_pcm_t *pcm, const void *data,
                                          uint32_t frames, bool floating) {
    const uint8_t *src=data;
    if (pcm->capacity == 0 || src == NULL) return;
    if (frames > pcm->capacity - pcm->used) {
        uint32_t dropped = frames - (pcm->capacity - pcm->used);
        // Track overwritten frames so overdue work is not trimmed twice.
        // This accounting must never cancel healthy or future output deadlines.
        pcm->dropped_pending = dropped > UINT32_MAX - pcm->dropped_pending ?
            UINT32_MAX : pcm->dropped_pending + dropped;
    }
    // Overflow keeps the newest whole stereo frames, never stale audio after a stall.
    if (frames > pcm->capacity) {
        src += (size_t)(frames - pcm->capacity)*2*(floating?sizeof(float):sizeof(int16_t));
        frames = pcm->capacity;
    }
    if (frames > pcm->capacity - pcm->used) {
        uint32_t drop = frames - (pcm->capacity - pcm->used);
        pcm->read += drop;
        if (pcm->read >= pcm->capacity) pcm->read -= pcm->capacity;
        pcm->used -= drop;
    }
    uint32_t write = pcm->read + pcm->used;
    if (write >= pcm->capacity) write -= pcm->capacity;
    for(uint32_t frame=0;frame<frames;++frame) {
        uint32_t index=write+frame;
        if(index>=pcm->capacity)index-=pcm->capacity;
        for(unsigned channel=0;channel<2;++channel) {
            float value;
            size_t sample=2u*frame+channel;
            if(floating)memcpy(&value,src+sample*sizeof(float),sizeof(value));
            else {
                int16_t integer;
                memcpy(&integer,src+sample*sizeof(integer),sizeof(integer));
                value=(float)integer/32768.0f;
            }
            pcm->samples[2u*index+channel]=playback_pcm_finite(value);
        }
    }
    pcm->used += frames;
}
static inline void playback_pcm_push(playback_pcm_t *pcm,const int16_t *src,uint32_t frames) {
    playback_pcm_push_data(pcm,src,frames,false);
}
static inline void playback_pcm_push_float(playback_pcm_t *pcm,const float *src,uint32_t frames) {
    playback_pcm_push_data(pcm,src,frames,true);
}

// Priming and a disconnected sink must not leave a permanent live-audio backlog.
static inline uint32_t playback_pcm_limit(playback_pcm_t *pcm, uint32_t frames) {
    if (pcm->used <= frames) return 0;
    uint32_t drop = pcm->used - frames;
    pcm->read += drop;
    if (pcm->read >= pcm->capacity) pcm->read -= pcm->capacity;
    pcm->used = frames;
    return drop;
}

static inline void playback_pcm_history(playback_pcm_t *pcm,uint32_t frames) {
    if(!frames)return;
    uint32_t keep=frames<PLAYBACK_FILTER_TAPS?frames:PLAYBACK_FILTER_TAPS;
    if(keep<PLAYBACK_FILTER_TAPS)
        memmove(pcm->history,pcm->history+keep*2,(PLAYBACK_FILTER_TAPS-keep)*2*sizeof(float));
    for(uint32_t i=0;i<keep;++i) {
        uint32_t index=pcm->read+frames-keep+i;
        if(index>=pcm->capacity)index-=pcm->capacity;
        pcm->history[2*(PLAYBACK_FILTER_TAPS-keep+i)]=pcm->samples[2*index];
        pcm->history[2*(PLAYBACK_FILTER_TAPS-keep+i)+1]=pcm->samples[2*index+1];
    }
}

static inline uint32_t playback_pcm_read(playback_pcm_t *pcm, int16_t *dst,
                                         uint32_t frames) {
    uint32_t prefix = pcm->priming < frames ? pcm->priming : frames;
    pcm->priming -= prefix;
    if (prefix) memset(dst, 0, (size_t)prefix * 2 * sizeof(*dst));
    uint32_t copied = frames - prefix;
    if (copied > pcm->used) copied = pcm->used;
    if (copied) {
        for(uint32_t frame=0;frame<copied;++frame) {
            uint32_t index=pcm->read+frame;
            if(index>=pcm->capacity)index-=pcm->capacity;
            for(unsigned channel=0;channel<2;++channel)
                dst[2u*(prefix+frame)+channel]=playback_pcm_quantize(pcm->samples[2u*index+channel]);
        }
        playback_pcm_history(pcm,copied);
        pcm->read += copied;
        if (pcm->read >= pcm->capacity) pcm->read -= pcm->capacity;
    }
    uint32_t filled = prefix + copied;
    if (filled < frames)
        memset(dst + filled * 2, 0, (size_t)(frames - filled) * 2 * sizeof(*dst));
    pcm->used -= copied;
    return copied;
}

// Fixed USB ingress needs matching resampling while the receiver fills or drains.
// Whole 480-frame ELD units keep phase integral across 0.9/1.0/1.1 transitions.
// Copy under the producer lock; filtering runs afterwards with USB IRQs enabled.
#define PLAYBACK_RESAMPLE_WINDOW 1128u
typedef struct {
    float samples[(PLAYBACK_RESAMPLE_WINDOW+PLAYBACK_FILTER_TAPS)*2];
    uint32_t frames,rate,fraction;
} playback_resample_t;
// With missing!=NULL, preserve the FIR timeline through an input shortage.
// missing counts padded required-window frames, including a possible single
// lookahead frame which is not consumed into the next history yet.
static inline bool playback_pcm_prepare_resample_fill(playback_pcm_t *pcm,playback_resample_t *work,
                                                 uint32_t frames,uint32_t rate,uint32_t *missing) {
    const uint32_t denominator=655360u;
    if(!frames || frames>1024 || !rate || rate>denominator*11u/10u || pcm->priming ||
       !pcm->capacity)return false;
    uint32_t end=pcm->fraction+frames*rate,last=end-rate;
    uint32_t consumed=end/denominator,needed=last/denominator+1u;
    if(needed<consumed)needed=consumed;
    if(needed>PLAYBACK_RESAMPLE_WINDOW || (!missing && pcm->used<needed))return false;
    uint32_t available=pcm->used<needed?pcm->used:needed;
    if(missing)*missing=needed-available;
    work->frames=frames;work->rate=rate;work->fraction=pcm->fraction;
    memcpy(work->samples,pcm->history,sizeof(pcm->history));
    uint32_t first=pcm->capacity-pcm->read;
    if(first>available)first=available;
    memcpy(work->samples+PLAYBACK_FILTER_TAPS*2,pcm->samples+pcm->read*2,first*2*sizeof(float));
    if(available>first)memcpy(work->samples+(PLAYBACK_FILTER_TAPS+first)*2,pcm->samples,(available-first)*2*sizeof(float));
    if(needed>available)memset(work->samples+(PLAYBACK_FILTER_TAPS+available)*2,0,(needed-available)*2*sizeof(float));
    // Keep the same FIR delay and fractional phase even at an input shortage.
    // The copied window contains history followed by actual/zero-filled input.
    memcpy(pcm->history,work->samples+consumed*2,sizeof(pcm->history));
    uint32_t copied=consumed<pcm->used?consumed:pcm->used;
    pcm->read+=copied;
    if(pcm->read>=pcm->capacity)pcm->read-=pcm->capacity;
    pcm->used-=copied;pcm->fraction=end%denominator;
    return true;
}
static inline bool playback_pcm_prepare_resample(playback_pcm_t *pcm,playback_resample_t *work,
                                                 uint32_t frames,uint32_t rate) {
    return playback_pcm_prepare_resample_fill(pcm,work,frames,rate,NULL);
}
static inline void playback_pcm_render(const playback_resample_t *work,int16_t *dst) {
    const uint32_t denominator=655360u;
    // At unity rate the fractional phase and its filter are constant for the block.
    if(work->rate==denominator) {
        if(!work->fraction) {
            for(uint32_t frame=0;frame<work->frames;++frame) {
                uint32_t index=PLAYBACK_FILTER_TAPS+frame-PLAYBACK_FILTER_DELAY;
                dst[2*frame]=playback_pcm_quantize(work->samples[2*index]);
                dst[2*frame+1]=playback_pcm_quantize(work->samples[2*index+1]);
            }
        } else {
            uint32_t phase=work->fraction*PLAYBACK_FILTER_PHASES;
            unsigned bin=phase/denominator;
            float blend=(float)(phase%denominator)/(float)denominator;
            float coefficients[PLAYBACK_FILTER_TAPS];
            for(unsigned tap=0;tap<PLAYBACK_FILTER_TAPS;++tap)
                coefficients[tap]=playback_filter[0][bin][tap]+blend*
                    (playback_filter[0][bin+1][tap]-playback_filter[0][bin][tap]);
            for(uint32_t frame=0;frame<work->frames;++frame) {
                uint32_t center=PLAYBACK_FILTER_TAPS+frame;
                float left=0,right=0;
                for(unsigned tap=0;tap<PLAYBACK_FILTER_TAPS;++tap) {
                    left+=coefficients[tap]*work->samples[2*(center-tap)];
                    right+=coefficients[tap]*work->samples[2*(center-tap)+1];
                }
                dst[2*frame]=playback_pcm_quantize(left);dst[2*frame+1]=playback_pcm_quantize(right);
            }
        }
        return;
    }
    unsigned filter=work->rate>denominator+denominator/20u?1:0;
    for(uint32_t frame=0;frame<work->frames;++frame) {
        uint32_t position=work->fraction+frame*work->rate;
        uint32_t phase=(position%denominator)*PLAYBACK_FILTER_PHASES;
        unsigned bin=phase/denominator;
        float blend=(float)(phase%denominator)/(float)denominator;
        uint32_t center=PLAYBACK_FILTER_TAPS+position/denominator;
        float left=0,right=0;
        if(!filter && !phase) {
            left=work->samples[2*(center-PLAYBACK_FILTER_DELAY)];
            right=work->samples[2*(center-PLAYBACK_FILTER_DELAY)+1];
        } else for(unsigned tap=0;tap<PLAYBACK_FILTER_TAPS;++tap) {
            float c=playback_filter[filter][bin][tap]+blend*
                (playback_filter[filter][bin+1][tap]-playback_filter[filter][bin][tap]);
            left+=c*work->samples[2*(center-tap)];
            right+=c*work->samples[2*(center-tap)+1];
        }
        dst[2*frame]=playback_pcm_quantize(left);dst[2*frame+1]=playback_pcm_quantize(right);
    }
}
static inline bool playback_pcm_read_scaled(playback_pcm_t *pcm,int16_t *dst,
                                            uint32_t frames,uint32_t rate) {
    playback_resample_t work;
    if(!playback_pcm_prepare_resample(pcm,&work,frames,rate))return false;
    playback_pcm_render(&work,dst);return true;
}

static inline bool playback_pcm_read_adjusted(playback_pcm_t *pcm, int16_t *dst,
                                              uint32_t frames, uint8_t interval) {
    if(frames!=480 || (interval!=9 && interval!=11))return false;
    return playback_pcm_read_scaled(pcm,dst,frames,(uint32_t)interval*65536u);
}

#endif
