#ifndef AIRPODS_PLAYBACK_INSPECT_H
#define AIRPODS_PLAYBACK_INSPECT_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define PLAYBACK_INSPECT_SIZE 400u
#define PLAYBACK_INSPECT_AU 320u
typedef struct {
    uint32_t stats[14],bad_packets;
    uint8_t au[PLAYBACK_INSPECT_AU];
} playback_inspect_t;
static inline void playback_inspect_samples(playback_inspect_t *s,const void *data,
    uint32_t count,bool floating,bool input,uint32_t us) {
    const uint8_t *p=data;
    unsigned base=input?0:4;
    uint32_t peak=0,nonzero=0;
    for(uint32_t i=0;i<count;++i) {
        uint32_t magnitude;
        if(floating) {
            float v;memcpy(&v,p+4*i,4);
            if(!isfinite(v))v=0;
            if(v!=0)++nonzero;
            if(v<0)v=-v;
            magnitude=v>=1?32768u:(uint32_t)(v*32768.0f);
        } else {
            int16_t v;memcpy(&v,p+2*i,2);
            if(v)++nonzero;
            magnitude=v<0?(uint32_t)-(int32_t)v:(uint32_t)v;
        }
        if(magnitude>peak)peak=magnitude;
    }
    s->stats[base]+=count;s->stats[base+1]+=nonzero;s->stats[base+2]=peak;
    if(nonzero)s->stats[base+3]=us;
    if(input)s->stats[13]=us;
}
static inline void playback_inspect_samples_q31(playback_inspect_t *s,const void *data,
    uint32_t count,uint32_t us) {
    uint32_t peak=0,nonzero=0;
    for(uint32_t i=0;i<count;++i) {
        int32_t sample;memcpy(&sample,(const uint8_t *)data+4u*i,sizeof(sample));
        int64_t value=sample;
        if(value)++nonzero;
        uint32_t magnitude=(uint32_t)((value<0?-value:value)>>16);
        if(magnitude>peak)peak=magnitude;
    }
    s->stats[4]+=count;s->stats[5]+=nonzero;s->stats[6]=peak;
    if(nonzero)s->stats[7]=us;
}
// Cache one AU only after its containing packet was accepted by the transport.
static inline bool playback_inspect_media(playback_inspect_t *s,const uint8_t *p,
    uint32_t size,uint32_t us) {
    uint32_t offset=0,last=0,length=0,sequence=0,count=0;
    while(offset<size) {
        if(size-offset<4 || (p[offset]&0xf0)!=0xb0 || (p[offset+2]&0xf8)!=0x10)goto bad;
        uint32_t n=((p[offset+2]&7u)<<8)|p[offset+3];
        if(!n || n>size-offset-4)goto bad;
        sequence=((p[offset]&15u)<<8)|p[offset+1];
        last=offset+4;length=n;++count;offset+=4+n;
    }
    if(!count)goto bad;
    s->stats[8]+=count;s->stats[9]=us;s->stats[10]=length;s->stats[12]=sequence;
    memset(s->au,0,sizeof(s->au));
    memcpy(s->au,p+last,length>sizeof(s->au)?sizeof(s->au):length);
    return true;
bad:
    ++s->bad_packets;return false;
}
static inline void playback_inspect_put32(uint8_t *p,uint32_t v) {
    for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));
}
static inline uint16_t playback_inspect_pack(const playback_inspect_t *s,uint8_t *out,
    uint16_t capacity,uint32_t us,uint32_t rtp,const uint8_t *asc,unsigned asc_size) {
    if(!out || capacity<PLAYBACK_INSPECT_SIZE || !asc || !asc_size || asc_size>8)return 0;
    memset(out,0,PLAYBACK_INSPECT_SIZE);out[0]=1;out[1]=3;
    out[2]=s->stats[10]>PLAYBACK_INSPECT_AU;out[3]=(uint8_t)asc_size;
    playback_inspect_put32(out+4,us);
    for(unsigned i=0;i<14;++i)playback_inspect_put32(out+8+4*i,s->stats[i]);
    playback_inspect_put32(out+64,rtp);memcpy(out+68,asc,asc_size);
    playback_inspect_put32(out+76,s->bad_packets);memcpy(out+80,s->au,sizeof(s->au));
    return PLAYBACK_INSPECT_SIZE;
}
#endif
