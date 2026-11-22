#include <stdint.h>

#include "modules/sdk_att_witness.hpp"

// SDK observation hooks are installed only for S3; host builds exercise them.
#if !defined(ARDUINO) || defined(LITCH_ESP32S3)
#if defined(LITCH_ESP32S3)
#include "console.hpp"
#endif

static_assert(sizeof(LitchSdkAttSnapshot) == 92, "ATT witness storage budget");
static_assert(__atomic_always_lock_free(4, nullptr), "ATT scalar atomics must be lock free");

namespace {
// SDK boundary storage; concurrent hook writers/readers use only scalar atomics.
LitchSdkAttSnapshot g_snapshot{{}, 0, 0, LITCH_ATT_PHASE_COUNT, 0,
                             0, 0, LITCH_ATT_PHASE_COUNT, 0, 0, 0, UINT32_MAX, 0, 0};

uint32_t Load(const uint32_t* value) {
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

void Store(uint32_t* target, uint32_t value) {
    __atomic_store_n(target, value, __ATOMIC_RELAXED);
}

void SamplePool(unsigned blocks, unsigned freeBlocks) {
    Store(&g_snapshot.poolBlocks, blocks);
    Store(&g_snapshot.poolFree, freeBlocks);
    uint32_t minimum = Load(&g_snapshot.minimumObservedFree);
    while (freeBlocks < minimum &&
           !__atomic_compare_exchange_n(&g_snapshot.minimumObservedFree, &minimum,
                                        freeBlocks, true, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {}
}
} // namespace

extern "C" void LitchSdkAttObserve(unsigned phase, uint32_t connectionCid,
                                    uint32_t opcodeHandle, int status,
                                    unsigned poolBlocks, unsigned poolFree) {
    if (phase >= LITCH_ATT_PHASE_COUNT) return;
    __atomic_fetch_add(&g_snapshot.counts[phase], 1U, __ATOMIC_RELAXED);
    Store(&g_snapshot.connectionCid, connectionCid);
    Store(&g_snapshot.opcodeHandle, opcodeHandle);
    Store(&g_snapshot.phase, phase);
    __atomic_store_n(&g_snapshot.status, status, __ATOMIC_RELAXED);
    // Terminal status repeats the same error; retain the failed inner stage.
    if (status != 0 && (phase == LITCH_ATT_WRITE_BUILD ||
                        phase == LITCH_ATT_WRITE_ACCESS_RETURN ||
                        phase == LITCH_ATT_TX_RETURN)) {
        Store(&g_snapshot.failedConnectionCid, connectionCid);
        Store(&g_snapshot.failedOpcodeHandle, opcodeHandle);
        Store(&g_snapshot.failedPhase, phase);
        __atomic_store_n(&g_snapshot.failedStatus, status, __ATOMIC_RELAXED);
    }
    SamplePool(poolBlocks, poolFree);
}

extern "C" void LitchSdkAttAllocation(int succeeded, unsigned poolBlocks,
                                       unsigned poolFree) {
    __atomic_fetch_add(&g_snapshot.allocationAttempts, 1U, __ATOMIC_RELAXED);
    if (!succeeded) {
        __atomic_fetch_add(&g_snapshot.allocationFailures, 1U, __ATOMIC_RELAXED);
    }
    SamplePool(poolBlocks, poolFree);
}

extern "C" void LitchSdkAttRead(LitchSdkAttSnapshot* output) {
    if (output == nullptr) return;
    for (unsigned index = 0; index < LITCH_ATT_PHASE_COUNT; ++index) {
        output->counts[index] = Load(&g_snapshot.counts[index]);
    }
    output->connectionCid = Load(&g_snapshot.connectionCid);
    output->opcodeHandle = Load(&g_snapshot.opcodeHandle);
    output->phase = Load(&g_snapshot.phase);
    output->status = __atomic_load_n(&g_snapshot.status, __ATOMIC_RELAXED);
    output->failedConnectionCid = Load(&g_snapshot.failedConnectionCid);
    output->failedOpcodeHandle = Load(&g_snapshot.failedOpcodeHandle);
    output->failedPhase = Load(&g_snapshot.failedPhase);
    output->failedStatus = __atomic_load_n(&g_snapshot.failedStatus, __ATOMIC_RELAXED);
    output->poolBlocks = Load(&g_snapshot.poolBlocks);
    output->poolFree = Load(&g_snapshot.poolFree);
    output->minimumObservedFree = Load(&g_snapshot.minimumObservedFree);
    output->allocationAttempts = Load(&g_snapshot.allocationAttempts);
    output->allocationFailures = Load(&g_snapshot.allocationFailures);
}


static_assert(sizeof(LitchSdkAclSnapshot) == 92, "ACL witness storage budget");
namespace {
LitchSdkAclSnapshot g_aclSnapshot{{}, 0, LITCH_ACL_PHASE_COUNT, 0, 0, 0, 0,
                                0, LITCH_ACL_PHASE_COUNT, 0, 0, 0, 0};
} // namespace

extern "C" void LitchSdkAclObserve(unsigned phase, uint32_t connectionFlags,
                                    int status, unsigned lengthOrDelta,
                                    uint32_t creditsOutstanding,
                                    unsigned fragmentPoolFree) {
    if (phase >= LITCH_ACL_PHASE_COUNT) return;
    __atomic_fetch_add(&g_aclSnapshot.counts[phase], 1U, __ATOMIC_RELAXED);
    Store(&g_aclSnapshot.connectionFlags, connectionFlags);
    Store(&g_aclSnapshot.phase, phase);
    __atomic_store_n(&g_aclSnapshot.status, status, __ATOMIC_RELAXED);
    Store(&g_aclSnapshot.lengthOrDelta, lengthOrDelta);
    Store(&g_aclSnapshot.creditsOutstanding, creditsOutstanding);
    Store(&g_aclSnapshot.fragmentPoolFree, fragmentPoolFree);
    // Retain inner failure context across terminal and later credit observations.
    if (status != 0 && phase != LITCH_ACL_NOW_RETURN) {
        Store(&g_aclSnapshot.failedConnectionFlags, connectionFlags);
        Store(&g_aclSnapshot.failedPhase, phase);
        __atomic_store_n(&g_aclSnapshot.failedStatus, status, __ATOMIC_RELAXED);
        Store(&g_aclSnapshot.failedLengthOrDelta, lengthOrDelta);
        Store(&g_aclSnapshot.failedCreditsOutstanding, creditsOutstanding);
        Store(&g_aclSnapshot.failedFragmentPoolFree, fragmentPoolFree);
    }
}

extern "C" void LitchSdkAclRead(LitchSdkAclSnapshot* output) {
    if (output == nullptr) return;
    for (unsigned index = 0; index < LITCH_ACL_PHASE_COUNT; ++index) {
        output->counts[index] = Load(&g_aclSnapshot.counts[index]);
    }
    output->connectionFlags = Load(&g_aclSnapshot.connectionFlags);
    output->phase = Load(&g_aclSnapshot.phase);
    output->status = __atomic_load_n(&g_aclSnapshot.status, __ATOMIC_RELAXED);
    output->lengthOrDelta = Load(&g_aclSnapshot.lengthOrDelta);
    output->creditsOutstanding = Load(&g_aclSnapshot.creditsOutstanding);
    output->fragmentPoolFree = Load(&g_aclSnapshot.fragmentPoolFree);
    output->failedConnectionFlags = Load(&g_aclSnapshot.failedConnectionFlags);
    output->failedPhase = Load(&g_aclSnapshot.failedPhase);
    output->failedStatus = __atomic_load_n(&g_aclSnapshot.failedStatus, __ATOMIC_RELAXED);
    output->failedLengthOrDelta = Load(&g_aclSnapshot.failedLengthOrDelta);
    output->failedCreditsOutstanding = Load(&g_aclSnapshot.failedCreditsOutstanding);
    output->failedFragmentPoolFree = Load(&g_aclSnapshot.failedFragmentPoolFree);
}

#if defined(LITCH_ESP32S3)
namespace modules {
void PrintSdkAttDiagnostics(fxe::Console& console) {
    LitchSdkAttSnapshot snapshot{};
    LitchSdkAttRead(&snapshot);
    console.PrintLn(
        "ble_att: counts=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu conn=%04x cid=%04x op=%02x handle=%04x phase=%u rc=%ld",
        static_cast<unsigned long>(snapshot.counts[0]),
        static_cast<unsigned long>(snapshot.counts[1]),
        static_cast<unsigned long>(snapshot.counts[2]),
        static_cast<unsigned long>(snapshot.counts[3]),
        static_cast<unsigned long>(snapshot.counts[4]),
        static_cast<unsigned long>(snapshot.counts[5]),
        static_cast<unsigned long>(snapshot.counts[6]),
        static_cast<unsigned long>(snapshot.counts[7]),
        static_cast<unsigned long>(snapshot.counts[8]),
        static_cast<unsigned long>(snapshot.counts[9]),
        static_cast<unsigned>(snapshot.connectionCid >> 16U),
        static_cast<unsigned>(snapshot.connectionCid & 0xFFFFU),
        static_cast<unsigned>(snapshot.opcodeHandle >> 16U),
        static_cast<unsigned>(snapshot.opcodeHandle & 0xFFFFU),
        static_cast<unsigned>(snapshot.phase), static_cast<long>(snapshot.status));
    console.PrintLn(
        "ble_att_pool: blocks=%lu free=%lu min_observed=%lu allocations=%lu failed=%lu fail_conn=%04x fail_cid=%04x fail_op=%02x fail_handle=%04x fail_phase=%u fail_rc=%ld",
        static_cast<unsigned long>(snapshot.poolBlocks),
        static_cast<unsigned long>(snapshot.poolFree),
        static_cast<unsigned long>(snapshot.minimumObservedFree),
        static_cast<unsigned long>(snapshot.allocationAttempts),
        static_cast<unsigned long>(snapshot.allocationFailures),
        static_cast<unsigned>(snapshot.failedConnectionCid >> 16U),
        static_cast<unsigned>(snapshot.failedConnectionCid & 0xFFFFU),
        static_cast<unsigned>(snapshot.failedOpcodeHandle >> 16U),
        static_cast<unsigned>(snapshot.failedOpcodeHandle & 0xFFFFU),
        static_cast<unsigned>(snapshot.failedPhase), static_cast<long>(snapshot.failedStatus));
    LitchSdkAclSnapshot acl{};
    LitchSdkAclRead(&acl);
    console.PrintLn(
        "ble_acl: c=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu conn=%04x flags=%04x p=%u rc=%ld n=%lu avail=%u out=%u free=%lu",
        static_cast<unsigned long>(acl.counts[0]), static_cast<unsigned long>(acl.counts[1]),
        static_cast<unsigned long>(acl.counts[2]), static_cast<unsigned long>(acl.counts[3]),
        static_cast<unsigned long>(acl.counts[4]), static_cast<unsigned long>(acl.counts[5]),
        static_cast<unsigned long>(acl.counts[6]), static_cast<unsigned long>(acl.counts[7]),
        static_cast<unsigned long>(acl.counts[8]), static_cast<unsigned long>(acl.counts[9]),
        static_cast<unsigned long>(acl.counts[10]),
        static_cast<unsigned>(acl.connectionFlags & 0xFFFFU),
        static_cast<unsigned>(acl.connectionFlags >> 16U), static_cast<unsigned>(acl.phase),
        static_cast<long>(acl.status), static_cast<unsigned long>(acl.lengthOrDelta),
        static_cast<unsigned>(acl.creditsOutstanding & 0xFFFFU),
        static_cast<unsigned>(acl.creditsOutstanding >> 16U),
        static_cast<unsigned long>(acl.fragmentPoolFree));
    console.PrintLn(
        "ble_acl_fail: conn=%04x flags=%04x p=%u rc=%ld n=%lu avail=%u out=%u free=%lu",
        static_cast<unsigned>(acl.failedConnectionFlags & 0xFFFFU),
        static_cast<unsigned>(acl.failedConnectionFlags >> 16U),
        static_cast<unsigned>(acl.failedPhase), static_cast<long>(acl.failedStatus),
        static_cast<unsigned long>(acl.failedLengthOrDelta),
        static_cast<unsigned>(acl.failedCreditsOutstanding & 0xFFFFU),
        static_cast<unsigned>(acl.failedCreditsOutstanding >> 16U),
        static_cast<unsigned long>(acl.failedFragmentPoolFree));

}
} // namespace modules
#endif
#endif
