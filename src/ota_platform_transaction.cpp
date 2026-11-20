#include "modules/ota_platform_adapter.hpp"

namespace modules {

bool OtaPlatformTransaction::OpsComplete() const {
    return m_ops.expectedChipId != 0U && m_ops.acquireFlash != nullptr &&
           m_ops.releaseFlash != nullptr && m_ops.captureTarget != nullptr &&
           m_ops.runningImageCanBeReplaced != nullptr &&
           m_ops.updateBegin != nullptr && m_ops.updateWrite != nullptr &&
           m_ops.targetCanBeActivated != nullptr && m_ops.updateEnd != nullptr &&
           m_ops.updateAbort != nullptr;
}

void OtaPlatformTransaction::ResetOwnedState() {
    m_identity.Abort();
    m_target = {};
    m_active = false;
}

bool OtaPlatformTransaction::Begin(const uint32_t expectedSize) {
    if (m_active) {
        Abort();
        if (m_active) {
            return false;
        }
    }
    ResetOwnedState();
    if (!OpsComplete() ||
        !m_identity.Begin(expectedSize, m_ops.expectedChipId) ||
        !m_ops.runningImageCanBeReplaced(m_ops.context) ||
        !m_ops.acquireFlash(m_ops.context)) {
        ResetOwnedState();
        return false;
    }

    OtaPostBootSlotIdentity target;
    const bool captured =
        m_ops.captureTarget(m_ops.context, expectedSize, target) &&
        target.address != 0U;
    const bool begun = captured &&
                       m_ops.updateBegin(m_ops.context, expectedSize);
    if (captured && !begun) {
        m_ops.updateAbort(m_ops.context);
    }
    m_ops.releaseFlash(m_ops.context);
    if (!begun) {
        ResetOwnedState();
        return false;
    }
    m_target = target;
    m_active = true;
    return true;
}

bool OtaPlatformTransaction::Write(const uint8_t* const data,
                                   const size_t len) {
    if (!m_active || !m_identity.Append(data, len)) {
        return false;
    }
    if (!m_ops.acquireFlash(m_ops.context)) {
        return false;
    }
    const size_t written = m_ops.updateWrite(m_ops.context, data, len);
    m_ops.releaseFlash(m_ops.context);
    return written == len;
}

bool OtaPlatformTransaction::End() {
    if (!m_active) {
        return false;
    }
    OtaImageIdentity identity;
    if (!m_identity.Finish(identity) ||
        identity.bootPolicy.status != OtaBootPolicyStatus::Known ||
        identity.bootPolicy.epoch != OTA_BOOT_POLICY_SDK_EPOCH) {
        Abort();
        return false;
    }
    if (!m_ops.acquireFlash(m_ops.context)) {
        Abort();
        return false;
    }
    const bool ended = m_ops.targetCanBeActivated(m_ops.context, m_target, identity) &&
                       m_ops.updateEnd(m_ops.context);
    if (!ended) {
        m_ops.updateAbort(m_ops.context);
    }
    m_ops.releaseFlash(m_ops.context);
    ResetOwnedState();
    return ended;
}

void OtaPlatformTransaction::Abort() {
    if (!m_active) {
        ResetOwnedState();
        return;
    }
    // Retain ownership if the lease is unavailable. A later abort or begin
    // must retry cleanup instead of orphaning Arduino's global Update state.
    if (!OpsComplete() || !m_ops.acquireFlash(m_ops.context)) {
        return;
    }
    m_ops.updateAbort(m_ops.context);
    m_ops.releaseFlash(m_ops.context);
    ResetOwnedState();
}

} // namespace modules
