/* Correct only the packet-verified .0076 vendor caller's fixed60 CVSD ledger. */
#include <stdint.h>

#define V8(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define V16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define V32(a) (*(volatile uint32_t *)(uintptr_t)(a))
struct sco_credit_context { uint32_t connection, host, packet, previous, caller, padding; };
volatile uint32_t sco_credit_vendor_stats[10]
    __attribute__((section(".sco_bridge_state"), aligned(4)));

uint32_t sco_credit_vendor_recount(uint32_t handle, uint32_t count,
                                  uint32_t threshold,
                                  const struct sco_credit_context *context) {
    ++sco_credit_vendor_stats[0];
    sco_credit_vendor_stats[3] = context->caller;
    sco_credit_vendor_stats[4] = handle;
    sco_credit_vendor_stats[5] = count;
    sco_credit_vendor_stats[6] = count;
    sco_credit_vendor_stats[7] = 0;
    sco_credit_vendor_stats[8] = 0;
    if (count != 1u || threshold != 1u || V32(0x2038d0u) != 2u ||
        V8(0x202e7du) != 1u || context->caller != 0xd1b61u) goto unchanged;
    uint32_t h = handle & 0xfffu;
    if (h == 0xfffu) goto unchanged;
    uintptr_t connection = ((uintptr_t (*)(uint32_t, uint32_t))0xf4a7u)(h, 2);
    if (connection < 0x2066ccu || connection > 0x20696cu ||
        (connection - 0x2066ccu) % 0x150u || connection != context->connection)
        goto unchanged;
    uint32_t group = (V32(connection + 0xe4u) >> 18) & 3u;
    if (group >= 3u || V16(0x202e84u + 2u * group) != h ||
        (V16(connection + 0xd2u) & 0xfffu) != h) goto unchanged;
    uintptr_t producer = 0x2063e4u + 40u * group;
    uintptr_t consumer = 0x20636cu + 40u * group;
    uintptr_t pipe = connection + 0x5cu;
    /* shortcut: fixed60-byte frames only; variable lengths need a boundary ledger. */
    if (V32(consumer + 0x14u) != pipe || V32(producer + 0x14u) != pipe ||
        V32(pipe + 0x18u) != producer || V32(pipe + 0x1cu) != consumer ||
        V8(producer + 0x24u) != 1u || V32(producer) != 0x662e1u ||
        V16(pipe + 0x4cu) != 120u || context->packet != 120u ||
        V32(connection + 0xecu) != 60u || context->host != 60u)
        goto unchanged;
    if (context->previous > UINT32_MAX - context->packet) goto unchanged;
    uint32_t total = context->previous + context->packet;
    uint32_t remaining = V32(connection + 0xf0u);
    sco_credit_vendor_stats[7] = remaining;
    sco_credit_vendor_stats[8] = remaining;
    /* The signed caller already chose its format conversion and subtracted
     * exactly one effective unit. Infer that actual unit; never query it twice.
     * Only the observed unscaled60-byte CVSD case is qualified here. */
    if (remaining > total || total - remaining != 60u) goto unchanged;
    uint32_t corrected = 1u + remaining / 60u;
    if (corrected > 255u) goto unchanged;
    uintptr_t report = V32(0x202dbcu);
    if (!report || report < 0x200000u || report >= 0x220000u ||
        V8(report) + corrected > 255u) goto unchanged;
    V32(connection + 0xf0u) = remaining - (corrected - 1u) * 60u;
    ++sco_credit_vendor_stats[1];
    sco_credit_vendor_stats[2] += corrected - count;
    sco_credit_vendor_stats[6] = corrected;
    sco_credit_vendor_stats[8] = V32(connection + 0xf0u);
    return corrected;
unchanged:
    ++sco_credit_vendor_stats[9];
    return count;
}
