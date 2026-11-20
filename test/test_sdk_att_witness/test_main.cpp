#include <cstdio>

#include "modules/sdk_att_witness.hpp"

int main() {
    LitchSdkAttSnapshot before{}, after{};
    LitchSdkAttRead(&before);
    const uint32_t connectionCid = (7U << 16U) | 4U;
    const uint32_t opcodeHandle = (0x12U << 16U) | 29U;
    LitchSdkAttObserve(LITCH_ATT_ENTER, connectionCid, opcodeHandle, 0, 12, 3);
    LitchSdkAttAllocation(0, 12, 0);
    LitchSdkAttObserve(LITCH_ATT_WRITE_BUILD, connectionCid, opcodeHandle, 6, 12, 0);
    LitchSdkAttObserve(LITCH_ATT_REPLY_RETURN, connectionCid, opcodeHandle, 6, 12, 2);
    LitchSdkAttObserve(LITCH_ATT_RETURN, connectionCid, opcodeHandle, 6, 12, 2);
    LitchSdkAttObserve(LITCH_ATT_ENTER, (8U << 16U) | 4U, 0x040001U, 0, 12, 5);
    LitchSdkAttRead(&after);
    bool passed = sizeof(after) == 92U &&
        after.counts[LITCH_ATT_ENTER] == before.counts[LITCH_ATT_ENTER] + 2U &&
        after.allocationFailures == before.allocationFailures + 1U &&
        after.allocationAttempts == before.allocationAttempts + 1U &&
        after.minimumObservedFree == 0U && after.poolFree == 5U &&
        after.failedConnectionCid == connectionCid &&
        after.failedOpcodeHandle == opcodeHandle &&
        after.failedPhase == LITCH_ATT_WRITE_BUILD && after.failedStatus == 6;
    LitchSdkAttObserve(LITCH_ATT_PHASE_COUNT, 0, 0, 1, 0, 0);
    LitchSdkAttSnapshot invalid{};
    LitchSdkAttRead(&invalid);
    passed = passed && invalid.poolFree == after.poolFree &&
        invalid.phase == after.phase && invalid.failedStatus == after.failedStatus;
    LitchSdkAttRead(nullptr);
    LitchSdkAclSnapshot aclBefore{}, aclAfter{};
    LitchSdkAclRead(&aclBefore);
    LitchSdkAclObserve(LITCH_ACL_TRANSPORT_RETURN, 0x00041234U, 7, 12, 0x00030002U, 0);
    LitchSdkAclObserve(LITCH_ACL_NOW_RETURN, 0x1234U, 7, 8, 0x00030002U, 1);
    LitchSdkAclObserve(LITCH_ACL_CREDIT_ADD, UINT16_MAX, 0, 3, 0xFFFF0005U, 1);
    LitchSdkAclObserve(LITCH_ACL_NOW_RETURN, 0x5678U, 0, 0, 0x00000005U, 1);
    LitchSdkAclRead(&aclAfter);
    passed = passed && sizeof(aclAfter) == 92U &&
        aclAfter.counts[LITCH_ACL_TRANSPORT_RETURN] == aclBefore.counts[LITCH_ACL_TRANSPORT_RETURN] + 1U &&
        aclAfter.failedConnectionFlags == 0x00041234U && aclAfter.failedPhase == LITCH_ACL_TRANSPORT_RETURN &&
        aclAfter.failedStatus == 7 && aclAfter.failedLengthOrDelta == 12U &&
        aclAfter.failedCreditsOutstanding == 0x00030002U && aclAfter.failedFragmentPoolFree == 0U;
    LitchSdkAclObserve(LITCH_ACL_PHASE_COUNT, 0, 1, 0, 0, 0);
    LitchSdkAclSnapshot aclInvalid{};
    LitchSdkAclRead(&aclInvalid);
    passed = passed && aclInvalid.phase == aclAfter.phase &&
        aclInvalid.failedStatus == aclAfter.failedStatus && aclInvalid.fragmentPoolFree == aclAfter.fragmentPoolFree;
    LitchSdkAclRead(nullptr);
    std::printf("SDK ATT witness failure retention: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
