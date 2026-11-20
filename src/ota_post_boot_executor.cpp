#include "modules/ota_post_boot_executor.hpp"

namespace modules {
namespace {

class FlashLease final {
  public:
    explicit FlashLease(const OtaPostBootExecutionOps& ops)
        : m_ops(ops), m_held(ops.acquireFlash(ops.context)) {}
    ~FlashLease() {
        if (m_held) m_ops.releaseFlash(m_ops.context);
    }
    FlashLease(const FlashLease&) = delete;
    FlashLease& operator=(const FlashLease&) = delete;
    [[nodiscard]] bool Held() const { return m_held; }

  private:
    const OtaPostBootExecutionOps& m_ops;
    bool m_held;
};

bool EvidenceMatches(const OtaPostBootImageEvidence& image,
                     const OtaPostBootSlotIdentity& slot) {
    if (image.slot.address != slot.address || image.slot.subtype != slot.subtype ||
        image.buildIdLength == 0U ||
        image.buildIdLength > image.buildId.size()) return false;
    bool buildNonzero = false;
    bool digestNonzero = false;
    for (size_t index = 0U; index < image.buildIdLength; ++index) {
        buildNonzero = buildNonzero || image.buildId[index] != 0U;
    }
    for (const uint8_t byte : image.imageSha256) {
        digestNonzero = digestNonzero || byte != 0U;
    }
    return buildNonzero && digestNonzero;
}

OtaPostBootExecutionResult FinishVerifiedImage(
    const OtaPostBootExecutionOps& ops, const OtaPostBootSnapshot& observed,
    const OtaPostBootHealth health) {
    const auto evaluation = EvaluateOtaPostBootVerification(observed, health);
    const bool rollback = evaluation.action == OtaPostBootAction::Rollback;
    FlashLease lease(ops);
    OtaPostBootSnapshot fresh;
    if (!lease.Held() || !ops.readSnapshot(ops.context, fresh)) {
        return OtaPostBootExecutionResult::SnapshotUnavailable;
    }
    if (!OtaPostBootSnapshotsMatch(observed, fresh) ||
        EvaluateOtaPostBootVerification(fresh, health).action != evaluation.action) {
        return OtaPostBootExecutionResult::EvidenceChanged;
    }
    if (rollback) {
        return ops.markInvalidAndReboot(ops.context)
                   ? OtaPostBootExecutionResult::RollbackRequested
                   : OtaPostBootExecutionResult::RollbackFailed;
    }
    if (evaluation.action == OtaPostBootAction::AcceptValid) {
        return OtaPostBootExecutionResult::Confirmed;
    }
    if (!ops.markValid(ops.context)) {
        return OtaPostBootExecutionResult::ConfirmFailed;
    }
    OtaPostBootSnapshot readback;
    if (!ops.readSnapshot(ops.context, readback) ||
        !IsOtaPostBootConfirmationReadback(fresh, readback)) {
        return OtaPostBootExecutionResult::ConfirmReadbackFailed;
    }
    return OtaPostBootExecutionResult::Confirmed;
}

} // namespace

OtaPostBootExecutionResult PrepareOtaPostBootRunningImage(
    const OtaPostBootExecutionOps& ops, OtaPostBootPreparedImage& outPrepared) {
    outPrepared = {};
    if (ops.acquireFlash == nullptr || ops.releaseFlash == nullptr ||
        ops.readSnapshot == nullptr || ops.readImage == nullptr) {
        return OtaPostBootExecutionResult::Unsupported;
    }
    {
        FlashLease lease(ops);
        if (!lease.Held() || !ops.readSnapshot(ops.context, outPrepared.snapshot)) {
            outPrepared = {};
            return OtaPostBootExecutionResult::SnapshotUnavailable;
        }
    }
    const auto evaluation = EvaluateOtaPostBootVerification(
        outPrepared.snapshot, OtaPostBootHealth::NotReported);
    if (evaluation.runningState == OtaPostBootRunningState::Unknown) {
        outPrepared = {};
        return OtaPostBootExecutionResult::NoAction;
    }
    if (!ops.readImage(ops.context, outPrepared.snapshot.running, outPrepared.image) ||
        !EvidenceMatches(outPrepared.image, outPrepared.snapshot.running)) {
        outPrepared = {};
        return OtaPostBootExecutionResult::ImageEvidenceUnavailable;
    }
    return OtaPostBootExecutionResult::AwaitingHealth;
}

OtaPostBootExecutionResult FinishOtaPostBootRunningImage(
    const OtaPostBootExecutionOps& ops, const OtaPostBootPreparedImage& prepared,
    const OtaPostBootHealth health) {
    if (health != OtaPostBootHealth::Passed) {
        return OtaPostBootExecutionResult::AwaitingHealth;
    }
    if (ops.acquireFlash == nullptr || ops.releaseFlash == nullptr ||
        ops.readSnapshot == nullptr || ops.markValid == nullptr) {
        return OtaPostBootExecutionResult::Unsupported;
    }
    const auto evaluation = EvaluateOtaPostBootVerification(prepared.snapshot, health);
    if (evaluation.action != OtaPostBootAction::Confirm &&
        evaluation.action != OtaPostBootAction::AcceptValid) {
        return OtaPostBootExecutionResult::NoAction;
    }
    if (!EvidenceMatches(prepared.image, prepared.snapshot.running)) {
        return OtaPostBootExecutionResult::ImageEvidenceUnavailable;
    }
    return FinishVerifiedImage(ops, prepared.snapshot, health);
}

OtaPostBootExecutionResult ExecuteOtaPostBootVerification(
    const OtaPostBootExecutionOps& ops, const OtaPostBootHealth health) {
    if (ops.acquireFlash == nullptr || ops.releaseFlash == nullptr ||
        ops.readSnapshot == nullptr || ops.readImage == nullptr ||
        ops.markValid == nullptr || ops.markInvalidAndReboot == nullptr) {
        return OtaPostBootExecutionResult::Unsupported;
    }
    OtaPostBootSnapshot observed;
    {
        FlashLease lease(ops);
        if (!lease.Held() || !ops.readSnapshot(ops.context, observed)) {
            return OtaPostBootExecutionResult::SnapshotUnavailable;
        }
    }
    const auto evaluation = EvaluateOtaPostBootVerification(observed, health);
    if (evaluation.action == OtaPostBootAction::AwaitHealth) {
        return OtaPostBootExecutionResult::AwaitingHealth;
    }
    if (evaluation.decision == OtaPostBootDecision::NoValidFallback) {
        return OtaPostBootExecutionResult::NoValidFallback;
    }
    if (evaluation.action == OtaPostBootAction::None) {
        return OtaPostBootExecutionResult::NoAction;
    }
    const bool rollback = evaluation.action == OtaPostBootAction::Rollback;
    const auto& slot = rollback ? evaluation.fallback : observed.running;
    OtaPostBootImageEvidence image;
    if (!ops.readImage(ops.context, slot, image) || !EvidenceMatches(image, slot)) {
        return OtaPostBootExecutionResult::ImageEvidenceUnavailable;
    }
    return FinishVerifiedImage(ops, observed, health);
}

} // namespace modules
