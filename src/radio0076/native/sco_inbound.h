#ifndef SCO_INBOUND_H
#define SCO_INBOUND_H
#include <stdint.h>
/* Enable only before SCO endpoint activation. Failure must abort bridge startup. */
uint32_t sco_inbound_enable_cpu_mode(void);
/* Peek only a bounded command header; root glue owns Reset cancellation. */
void sco_inbound_command(const uint8_t *body, uint32_t length,
                         uint32_t wrapped, const uint8_t *base,
                         const uint8_t *last);
void sco_bridge_reset(void);
/* Return 1 to consume the entire native ring frame, or 0 only for reentrancy. */
uint32_t sco_inbound_process(const uint8_t *body, uint32_t length,
                            uint32_t wrapped, const uint8_t *base,
                            const uint8_t *last);
struct sco_inbound_counters {
    uint32_t frames, malformed, invalid_handle, inactive, unsafe_pipe;
    uint32_t cpu_mode_mismatch, native_submissions, native_bytes;
    uint32_t unavailable, busy_retries, init_success, init_rejected;
};
extern volatile struct sco_inbound_counters sco_inbound_stats;
#endif
