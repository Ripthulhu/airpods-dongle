#include "sco_outbound.h"
static uint32_t word(const void *p,unsigned o){return *(volatile const uint32_t *)((const uint8_t *)p+o);}
static uint16_t half(const void *p,unsigned o){return *(volatile const uint16_t *)((const uint8_t *)p+o);}
static uint8_t byte(const void *p,unsigned o){return *((volatile const uint8_t *)p+o);}
static int ram(uint32_t p,uint32_t n){return !(p&3u) && p>=0x200000u && p<=0x220000u-n;}
static int finish(struct sco_out_env *e,uint32_t key,int result){
    e->unlock(key);
    if(result==SCO_OUT_DROP && e->drop_notify)e->drop_notify();
    return result;
}
static void clear_pending(struct sco_out_state *s){s->pending_message=0;s->pending_length=0;}
static void arm(void *ep){*(volatile uint16_t *)((uint8_t *)ep+0x1e)=half(ep,0x1e)|0x10u;}
static int endpoint(struct sco_out_env *e,const uint8_t *m,uint8_t **ep,uint8_t **pipe,uint16_t *handle){
    uint32_t index=(uint32_t)m[9]|((uint32_t)m[10]<<8);
    if(index>=3 || !ram((uint32_t)(uintptr_t)e->endpoints,120))return 0;
    *ep=e->endpoints+40*index;
    uint32_t flags=half(*ep,0x1e),group=(flags>>2)&3u;
    if((flags&3u)!=1 || byte(*ep,0x24)!=1 || group>=3 || !half(*ep,0x1c))return 0;
    *handle=e->handles[group];
    if(*handle>=0xfffu)return 0;
    uint32_t p=word(*ep,0x14);
    if(!ram(p,0x78))return 0;
    *pipe=(uint8_t *)(uintptr_t)p;
    if(word(*pipe,0x1c)!=(uint32_t)(uintptr_t)*ep || word(*ep,0)!=0x662f5u)return 0;
    if(!ram(word(*pipe,0x18),40))return 0;
    if(!half(*pipe,0x50) || half(*pipe,0x4e)>half(*pipe,0x50))return 0;
    return 1;
}
static int send_pending(struct sco_out_env *e,uint32_t key){
    struct sco_out_state *s=e->state;
    int status=e->preflight(s->pending_length);
    if(status==1){++s->retry_full;return finish(e,key,SCO_OUT_RETRY);}
    if(status!=0){++s->dropped;clear_pending(s);return finish(e,key,SCO_OUT_DROP);}
    if(e->commit(e->scratch,s->pending_length)!=0){++s->commit_retry;return finish(e,key,SCO_OUT_RETRY);}
    ++s->sent;s->bytes+=s->pending_length-3;clear_pending(s);e->wake();
    return finish(e,key,SCO_OUT_SENT);
}
int sco_outbound_send(const uint8_t m[12],struct sco_out_env *e){
    struct sco_out_state *s=e->state;
    uint32_t key=e->lock(),message=(uint32_t)(uintptr_t)m;
    if(*e->reset_blocked){clear_pending(s);++s->dropped;return finish(e,key,SCO_OUT_DROP);}
    if(!ram(message,12) || ((uint32_t)m[4]|((uint32_t)m[5]<<8))!=0x7074u){++s->dropped;return finish(e,key,SCO_OUT_DROP);}
    if(s->pending_message && s->pending_epoch!=*e->epoch){
        uint32_t old=s->pending_message;clear_pending(s);++s->canceled;
        if(old==message)return finish(e,key,SCO_OUT_DROP);
    }
    if(s->pending_message && s->pending_message!=message)return finish(e,key,SCO_OUT_RETRY);
    uint8_t *ep,*pipe;uint16_t handle;
    if(!endpoint(e,m,&ep,&pipe,&handle)){
        ++s->dropped;if(s->pending_message==message)clear_pending(s);
        return finish(e,key,SCO_OUT_DROP);
    }
    if(s->pending_message){
        if(s->pending_pipe!=(uint32_t)(uintptr_t)pipe || s->pending_handle!=handle){++s->canceled;clear_pending(s);return finish(e,key,SCO_OUT_DROP);}
        return send_pending(e,key);
    }
    uint32_t available=half(pipe,0x4e),block=word(pipe,8),current=word(pipe,0x20),offset=word(pipe,0x28),packet=half(pipe,0x4c);
    if(!available){++s->empty;e->rearm(ep);return finish(e,key,SCO_OUT_DROP);}
    if(block==(uint32_t)(uintptr_t)(pipe+8)){
        /* A partial producer block is counted but not queued. Arm the NEXT producer commit, not an immediate callback loop. */
        arm(ep);++s->retry_partial;return finish(e,key,SCO_OUT_RETRY);
    }
    if(!packet || packet>0x1000u || offset>=packet || !ram(block,packet+16) || (current && current!=block) || (!current && offset) || word((void *)(uintptr_t)block,4)!=(uint32_t)(uintptr_t)(pipe+8)){
        ++s->dropped;return finish(e,key,SCO_OUT_DROP);
    }
    uint32_t next=word((void *)(uintptr_t)block,0),freefirst=word(pipe,0x10),freelast=word(pipe,0x14);
    if((next!=(uint32_t)(uintptr_t)(pipe+8) && (!ram(next,8) || word((void *)(uintptr_t)next,4)!=block)) || (freefirst!=(uint32_t)(uintptr_t)(pipe+0x10) && !ram(freefirst,8)) || (freelast!=(uint32_t)(uintptr_t)(pipe+0x10) && !ram(freelast,8))){++s->dropped;return finish(e,key,SCO_OUT_DROP);}
    uint32_t n=*e->host_limit,stream=*e->stream_limit;
    if(n>stream)n=stream;
    if(n>available)n=available;
    if(n>packet-offset)n=packet-offset;
    if(!n){++s->dropped;return finish(e,key,SCO_OUT_DROP);}
    int ready=e->preflight(n+3);
    if(ready==1){++s->retry_full;return finish(e,key,SCO_OUT_RETRY);}
    if(ready!=0){++s->dropped;return finish(e,key,SCO_OUT_DROP);}
    uint32_t flags=(half(ep,0x1e)&0x20u)?e->packet_flags(pipe,n):0;
    uint32_t got=e->read_bytes(pipe,e->scratch+3,n)&0xfffu;
    if(!got){arm(ep);++s->retry_partial;return finish(e,key,SCO_OUT_RETRY);}
    if(got>n){++s->dropped;return finish(e,key,SCO_OUT_DROP);}
    if(got!=n)++s->short_read;
    uint16_t wire=(uint16_t)(handle|flags);
    e->scratch[0]=(uint8_t)wire;e->scratch[1]=(uint8_t)(wire>>8);e->scratch[2]=(uint8_t)got;
    s->pending_message=message;s->pending_pipe=(uint32_t)(uintptr_t)pipe;s->pending_handle=handle;s->pending_epoch=*e->epoch;s->pending_length=got+3;
    int result=e->commit(e->scratch,s->pending_length);
    /* readBytes owns the native block release. Rearm once per consumption, including a staged commit retry. */
    e->rearm(ep);
    if(result!=0){++s->commit_retry;return finish(e,key,SCO_OUT_RETRY);}
    ++s->sent;s->bytes+=got;clear_pending(s);e->wake();return finish(e,key,SCO_OUT_SENT);
}
void sco_outbound_cancel(struct sco_out_env *e){uint32_t key=e->lock();if(e->state->pending_message)++e->state->canceled;clear_pending(e->state);e->unlock(key);}
