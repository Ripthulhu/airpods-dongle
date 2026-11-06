#ifndef SCO_OUTBOUND_H
#define SCO_OUTBOUND_H
#include <stdint.h>
#include <stddef.h>
enum { SCO_OUT_SENT=0, SCO_OUT_RETRY=1, SCO_OUT_DROP=2, SCO_OUT_SCRATCH=260 };
struct sco_out_state {
    uint32_t pending_message,pending_pipe,pending_handle,pending_epoch,pending_length;
    uint32_t sent,bytes,retry_full,retry_partial,dropped,empty,short_read,commit_retry,canceled;
};
struct sco_out_env {
    uint8_t *endpoints;
    volatile uint16_t *handles;
    volatile uint8_t *host_limit,*stream_limit;
    volatile uint32_t *epoch;
    volatile uint32_t *reset_blocked;
    uint8_t *scratch;
    struct sco_out_state *state;
    uint32_t (*lock)(void);
    void (*unlock)(uint32_t);
    uint32_t (*read_bytes)(void *,void *,uint32_t);
    uint32_t (*packet_flags)(void *,uint32_t);
    void (*rearm)(void *);
    int (*preflight)(uint32_t);
    int (*commit)(const void *,uint32_t);
    void (*wake)(void);
    void (*drop_notify)(void);
};
extern uint32_t sco_out_transport_context;
/* Caller owns metadata throughout: native4406a frees on0/2, requeues on1. */
int sco_outbound_send(const uint8_t metadata[12],struct sco_out_env *env);
/* Cancels staging only. Native pending queue still owns/frees every message. */
void sco_outbound_cancel(struct sco_out_env *env);
int sco_native_b2h_preflight(uint32_t payload_length);
int sco_native_b2h_commit(const void *payload,uint32_t payload_length);
void sco_native_b2h_wake(void);
#endif
