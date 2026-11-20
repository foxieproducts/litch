#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum LitchSdkAttPhase {
    LITCH_ATT_ENTER,
    LITCH_ATT_DISPATCH,
    LITCH_ATT_RETURN,
    LITCH_ATT_WRITE_BUILD,
    LITCH_ATT_WRITE_ACCESS_ENTER,
    LITCH_ATT_WRITE_ACCESS_RETURN,
    LITCH_ATT_REPLY_ENTER,
    LITCH_ATT_TX_ENTER,
    LITCH_ATT_TX_RETURN,
    LITCH_ATT_REPLY_RETURN,
    LITCH_ATT_PHASE_COUNT
};

// Packed identifiers contain no payload: connection/cid and opcode/attribute.
struct LitchSdkAttSnapshot {
    uint32_t counts[LITCH_ATT_PHASE_COUNT];
    uint32_t connectionCid;
    uint32_t opcodeHandle;
    uint32_t phase;
    int32_t status;
    uint32_t failedConnectionCid;
    uint32_t failedOpcodeHandle;
    uint32_t failedPhase;
    int32_t failedStatus;
    uint32_t poolBlocks;
    uint32_t poolFree;
    uint32_t minimumObservedFree;
    uint32_t allocationAttempts;
    uint32_t allocationFailures;
};

// SDK boundary only; all identifiers are supplied by the actual call site.
void LitchSdkAttObserve(unsigned phase, uint32_t connectionCid,
                       uint32_t opcodeHandle, int status,
                       unsigned poolBlocks, unsigned poolFree);
void LitchSdkAttAllocation(int succeeded, unsigned poolBlocks, unsigned poolFree);
// Sequential scalar observations, not a transactionally coherent snapshot.
void LitchSdkAttRead(struct LitchSdkAttSnapshot* output);


// ACL handoff observations; no CID/opcode is inferred from partial fragments.
enum LitchSdkAclPhase {
    LITCH_ACL_NOW_ENTER,
    LITCH_ACL_QUEUE_BLOCKED,
    LITCH_ACL_FRAGMENT_NULL,
    LITCH_ACL_HEADER_NULL,
    LITCH_ACL_TRANSPORT_ENTER,
    LITCH_ACL_TRANSPORT_RETURN,
    LITCH_ACL_CREDIT_ACCOUNTED,
    LITCH_ACL_REMAINDER,
    LITCH_ACL_NOW_RETURN,
    LITCH_ACL_BUFFER_INIT,
    LITCH_ACL_CREDIT_ADD,
    LITCH_ACL_PHASE_COUNT
};

struct LitchSdkAclSnapshot {
    uint32_t counts[LITCH_ACL_PHASE_COUNT];
    uint32_t connectionFlags; // Connection low 16 bits; SDK flags high 16 bits.
    uint32_t phase;
    int32_t status;
    uint32_t lengthOrDelta; // Packet/fragment length, initial size, or credit delta.
    uint32_t creditsOutstanding; // Available low 16 bits; outstanding high 16 bits.
    uint32_t fragmentPoolFree;
    uint32_t failedConnectionFlags;
    uint32_t failedPhase;
    int32_t failedStatus;
    uint32_t failedLengthOrDelta;
    uint32_t failedCreditsOutstanding;
    uint32_t failedFragmentPoolFree;
};

void LitchSdkAclObserve(unsigned phase, uint32_t connectionFlags, int status,
                       unsigned lengthOrDelta, uint32_t creditsOutstanding,
                       unsigned fragmentPoolFree);
// Sequential scalar observations, not a transactionally coherent snapshot.
void LitchSdkAclRead(struct LitchSdkAclSnapshot* output);

#ifdef __cplusplus
}
#if defined(LITCH_ESP32S3)
namespace fxe { class Console; }
namespace modules {
void PrintSdkAttDiagnostics(fxe::Console& console);
}
#endif
#endif
