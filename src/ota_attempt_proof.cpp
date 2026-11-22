#include "modules/ota_attempt_proof.hpp"

#include <array>
#include <cstdio>

#include "foxie_embedded/millis.hpp"

namespace modules {
namespace {

bool SlotsMatch(const OtaPostBootSlotIdentity& left,
                const OtaPostBootSlotIdentity& right) {
    return left.address == right.address && left.subtype == right.subtype;
}

uint8_t SlotIndex(const OtaPostBootSnapshot& snapshot,
                  const OtaPostBootSlotIdentity& slot) {
    for (uint8_t index = 0U; index < snapshot.slots.size(); ++index) {
        if (SlotsMatch(slot, snapshot.slots[index])) return index;
    }
    return OTA_POST_BOOT_NO_ENTRY;
}

bool AdmissionsMatch(const OtaAdmissionEvidence& left,
                     const OtaAdmissionEvidence& right) {
    return left.attempt == right.attempt && left.phase == right.phase &&
           left.sourceSequence == right.sourceSequence &&
           left.expectedSequence == right.expectedSequence &&
           SlotsMatch(left.source, right.source) &&
           SlotsMatch(left.target, right.target);
}

bool AdmissionIsCoherent(
    const OtaAdmissionEvidence& admission, const OtaPostBootSnapshot& snapshot,
    const std::array<OtaPostBootSelectorEvidence, 2U>& selectors) {
    if (admission.phase == OtaAdmissionPhase::None) {
        return admission.attempt == 0U && admission.sourceSequence == 0U &&
               admission.expectedSequence == 0U &&
               SlotsMatch(admission.source, {}) && SlotsMatch(admission.target, {});
    }
    const uint8_t source = SlotIndex(snapshot, admission.source);
    const uint8_t target = SlotIndex(snapshot, admission.target);
    if (admission.phase > OtaAdmissionPhase::Failed || admission.attempt == 0U ||
        source > 1U || target > 1U || source == target ||
        admission.sourceSequence == 0U ||
        admission.sourceSequence >= UINT32_MAX - 2U ||
        (admission.sourceSequence - 1U) % 2U != source ||
        admission.expectedSequence != admission.sourceSequence + 1U ||
        (admission.expectedSequence - 1U) % 2U != target ||
        !SlotsMatch(snapshot.running, admission.source)) return false;

    // The source is immutable throughout this boot's admission. In Receiving,
    // it must still be the SDK-selected VALID source, not merely an old cache
    // with plausible target parity. Activated may select only this exact target.
    bool sourceValid = false;
    for (const auto& selector : selectors) {
        sourceValid = sourceValid ||
            (selector.crcValid && selector.sequence == admission.sourceSequence &&
             selector.state == 2U);
    }
    if (!sourceValid) return false;
    const bool selectedSource = SlotsMatch(snapshot.selected, admission.source) &&
        snapshot.selectedSequence == admission.sourceSequence &&
        selectors[snapshot.selectedEntry].state == 2U;
    const bool selectedTarget = SlotsMatch(snapshot.selected, admission.target) &&
                                snapshot.selectedSequence == admission.expectedSequence;
    if (admission.phase == OtaAdmissionPhase::Receiving ||
        admission.phase == OtaAdmissionPhase::Aborted) return selectedSource;
    if (admission.phase == OtaAdmissionPhase::Activated) return selectedTarget;
    return selectedSource || selectedTarget;
}

bool CopyCoherentEvidence(OtaPlatformAdapter* adapter, OtaBootEvidence& boot,
                          OtaAdmissionEvidence& admission,
                          std::array<OtaPostBootSelectorEvidence, 2U>& selectors) {
    OtaAdmissionEvidence after;
    if (adapter == nullptr || !adapter->ReadAdmissionEvidence(admission) ||
        !adapter->ReadBootEvidence(boot) ||
        !adapter->ReadAdmissionEvidence(after) ||
        !AdmissionsMatch(admission, after) || !boot.available || boot.sample == 0U ||
        !DecodeCoherentOtaPostBootSnapshot(boot.snapshot, selectors) ||
        boot.runningState != EvaluateOtaPostBootVerification(
            boot.snapshot, OtaPostBootHealth::NotReported).runningState ||
        !AdmissionIsCoherent(admission, boot.snapshot, selectors)) return false;
    for (const auto& selector : selectors) {
        if (selector.state > 4U && selector.state != UINT32_MAX) return false;
    }
    return true;
}

} // namespace

bool FormatOtaAttemptProof(OtaPlatformAdapter* adapter,
                          const OtaAttemptProofRequest& request,
                          char* const out, const size_t capacity) {
    if (out == nullptr || capacity == 0U) return false;
    out[0U] = '\0';
    if (request.requestId == 0U) return false;
    OtaBootEvidence boot;
    OtaAdmissionEvidence admission;
    std::array<OtaPostBootSelectorEvidence, 2U> selectors{};
    int length = 0;
    if (request.bootNonce == 0U ||
        !CopyCoherentEvidence(adapter, boot, admission, selectors)) {
        length = std::snprintf(out, capacity,
            "{\"op\":1,\"q\":%lu,\"e\":\"unavailable\"}",
            static_cast<unsigned long>(request.requestId));
    } else {
        const auto& snapshot = boot.snapshot;
        length = std::snprintf(out, capacity,
            "{\"op\":1,\"q\":%lu,\"n\":\"%08lx%08lx\",\"v\":%u,"
            "\"s\":[%lu,%lu,%u,%u,%u,%lu,%u,%lu,%lu,%u,%lu,%lu,%u],"
            "\"a\":[%lu,%u,%u,%u,%lu,%lu]}",
            static_cast<unsigned long>(request.requestId),
            static_cast<unsigned long>(request.bootNonce >> 32U),
            static_cast<unsigned long>(request.bootNonce & UINT32_MAX),
            request.runtimeAccepted ? 1U : 0U,
            static_cast<unsigned long>(boot.sample),
            static_cast<unsigned long>(fxe::Millis() - boot.capturedAtMs),
            static_cast<unsigned>(SlotIndex(snapshot, snapshot.running)),
            static_cast<unsigned>(SlotIndex(snapshot, snapshot.selected)),
            static_cast<unsigned>(snapshot.selectedEntry),
            static_cast<unsigned long>(snapshot.selectedSequence),
            static_cast<unsigned>(boot.runningState),
            static_cast<unsigned long>(selectors[0U].sequence),
            static_cast<unsigned long>(selectors[0U].state),
            selectors[0U].crcValid ? 1U : 0U,
            static_cast<unsigned long>(selectors[1U].sequence),
            static_cast<unsigned long>(selectors[1U].state),
            selectors[1U].crcValid ? 1U : 0U,
            static_cast<unsigned long>(admission.attempt),
            static_cast<unsigned>(admission.phase),
            static_cast<unsigned>(SlotIndex(snapshot, admission.source)),
            static_cast<unsigned>(SlotIndex(snapshot, admission.target)),
            static_cast<unsigned long>(admission.sourceSequence),
            static_cast<unsigned long>(admission.expectedSequence));
    }
    if (length < 0 || static_cast<size_t>(length) >= capacity) {
        out[0U] = '\0';
        return false;
    }
    return true;
}

} // namespace modules
