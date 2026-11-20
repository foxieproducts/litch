#include "firmware/factory_radio_coordinator.hpp"

#if defined(ARDUINO) && defined(LITCH_HW_VARIANT_S3)

#include <esp_system.h>

#include "logger.hpp"

namespace firmware {
namespace {

constexpr uint32_t BLE_RETRY_MS = 1000U;
constexpr uint32_t BLE_RECOVERY_SCAN_INTERVAL = 4U;
constexpr uint32_t TERMINAL_REPLY_GRACE_MS = 1000U;

uint32_t NonzeroRandom() {
    const uint32_t value = esp_random();
    return value == 0U ? 1U : value;
}

bool HasElapsed(const uint32_t nowMs, const uint32_t sinceMs,
                const uint32_t durationMs) {
    return static_cast<uint32_t>(nowMs - sinceMs) >= durationMs;
}

bool IsValidProof(const modules::FactoryBleProof& proof) {
    return proof.sessionId != 0U && proof.dutNonce != 0U &&
        proof.verifierNonce != 0U && proof.dutNonce != proof.verifierNonce;
}

} // namespace

void FactoryRadioCoordinator::StartDut() {
    m_dutState = DutState::Searching;
    m_bleProof = modules::FactoryBleProof{NonzeroRandom(), NonzeroRandom(), 0U};
    if (m_bleProof.sessionId == m_bleProof.dutNonce) {
        m_bleProof.dutNonce ^= 0xA5A5A5A5U;
        if (m_bleProof.dutNonce == 0U) m_bleProof.dutNonce = 1U;
    }
}

void FactoryRadioCoordinator::ResetRun() {
    m_roleGate.Stop();
    m_dutState = DutState::Idle;
    m_bleJobPending = false;
    m_peerCandidate = false;
    m_dutBleVerifiedPending = false;
    m_verifierBleVerifiedPending = false;
    m_terminalReplyPending = false;
}

void FactoryRadioCoordinator::UpdateDut(
    modules::BleService& ble, const uint32_t nowMs) {
    if (m_dutState == DutState::Complete ||
        (!m_bleJobPending && !HasElapsed(nowMs, m_lastBleAttemptMs, m_bleRetryMs))) {
        return;
    }
    if (m_dutState == DutState::Idle) StartDut();
    modules::FactoryBleProof proof;
    // A saved verifier never searches back. Periodically allow a higher
    // address so missing it during initial discovery cannot strand a DUT.
    const bool recoveryScan = m_bleAttempt % BLE_RECOVERY_SCAN_INTERVAL == BLE_RECOVERY_SCAN_INTERVAL - 1U;
    const auto result = ble.PollFactoryBleDut(
        m_bleProof.sessionId, m_bleProof.dutNonce, m_roleGate.IsVerifier() && !recoveryScan, proof);
    m_bleJobPending = result == modules::FactoryBleDutResult::Pending;
    m_peerCandidate = ble.HasFactoryBleCandidate();
    if (m_peerCandidate) m_roleGate.ObserveEligiblePeer(nowMs);
    if (m_bleJobPending) return;

    m_lastBleAttemptMs = nowMs;
    m_bleRetryMs = modules::FactoryRadioRoleGate::RetryDelayMs(
        ble.FactoryRadioIdentity(), m_bleAttempt++);
    if (result != modules::FactoryBleDutResult::Verified || !IsValidProof(proof) ||
        proof.sessionId != m_bleProof.sessionId || proof.dutNonce != m_bleProof.dutNonce) {
        m_dutState = DutState::Idle;
        return;
    }
    // Poll publishes Verified only after the exact exchange and SDK cleanup.
    m_roleGate.AcceptBleProof();
    m_dutState = DutState::Complete;
    ble.SetFactoryVerifierConfiguredEnabled(false);
    m_dutBleVerifiedPending = true;
    logger::Info("hwtest: BLE bidirectional proof complete");
}

void FactoryRadioCoordinator::PollTerminalReply(
    modules::BleService& ble, const uint32_t nowMs) {
    if (!m_terminalReplyPending) return;
    // The terminal GATT value remains readable while new writes are fenced.
    ble.SetFactoryVerifierRuntimeAllowed(false);
    if (HasElapsed(nowMs, m_terminalReplyStartedMs, TERMINAL_REPLY_GRACE_MS)) {
        m_terminalReplyPending = false;
        logger::Info("factory verifier: terminal reply window complete");
    }
}

void FactoryRadioCoordinator::UpdateVerifier(
    modules::BleService& ble, const uint32_t nowMs) {
    if (m_terminalReplyPending) {
        PollTerminalReply(ble, nowMs);
        return;
    }
    modules::FactoryBleProof proof;
    // The service keeps the callback proof pending until client/task cleanup.
    if (!ble.ConsumeFactoryBleVerifierProof(proof) || !IsValidProof(proof)) return;
    m_bleJobPending = false;
    if (m_hardwareTestActive) m_dutState = DutState::Complete;
    m_verifierBleVerifiedPending = true;
    m_terminalReplyPending = true;
    m_terminalReplyStartedMs = nowMs;
    ble.SetFactoryVerifierRuntimeAllowed(false);
    logger::Info("factory verifier: BLE bidirectional proof complete");
}

void FactoryRadioCoordinator::Update(
    const bool hardwareTestActive, const bool hardwareTestRadioActive,
    const modules::ManufacturingTestStage hardwareTestStage,
    modules::BleService& ble, const uint32_t nowMs, const uint32_t runId) {
    const bool changedRun = hardwareTestActive != m_hardwareTestActive ||
        (hardwareTestActive && runId != m_activeRunId);
    const bool ready = hardwareTestActive && hardwareTestRadioActive &&
        modules::IsFactoryRadioStageReady(hardwareTestStage);
    if (changedRun || (hardwareTestActive && !ready)) {
        ble.SetFactoryVerifierRuntimeAllowed(false);
        // A temporary endpoint cannot acquire saved-verifier authority during
        // local reinspection, even while asynchronous cancellation is pending.
        ble.SetFactoryVerifierConfiguredEnabled(false);
        m_dutBleVerifiedPending = false;
        m_verifierBleVerifiedPending = false;
        if (!ble.QuiesceFactoryBleDut()) return;
        ResetRun();
        m_hardwareTestActive = hardwareTestActive;
        m_activeRunId = runId;
        if (!ready) return;
    }
    if (ready && !m_roleGate.IsActive()) {
        m_roleGate.Begin(nowMs, esp_random());
        m_bleAttempt = 0U;
        m_bleRetryMs = BLE_RETRY_MS;
        m_lastBleAttemptMs = nowMs - BLE_RETRY_MS;
        m_lastEndpointAttemptMs = nowMs - BLE_RETRY_MS;
        m_bleEndpointStartFailed = false;
    }
    if (ready) {
        m_peerCandidate = ble.HasFactoryBleCandidate();
        if (ble.IsFactoryBleDutRunning() && m_peerCandidate) {
            m_roleGate.ObserveEligiblePeer(nowMs);
        }
        m_roleGate.Tick(nowMs);
        if (m_roleGate.IsVerifier() && m_dutState != DutState::Complete &&
            !ble.IsFactoryVerifierConfiguredEnabled() &&
            HasElapsed(nowMs, m_lastEndpointAttemptMs, BLE_RETRY_MS)) {
            m_lastEndpointAttemptMs = nowMs;
            m_bleEndpointStartFailed = !ble.EnableTemporaryFactoryVerifierEndpoint();
        }
        if (m_roleGate.CanSearch() && !m_terminalReplyPending) UpdateDut(ble, nowMs);
    }
    const bool verifierAllowed = !hardwareTestActive
        ? ble.IsFactoryVerifierConfiguredEnabled()
        : ready && m_roleGate.IsVerifier() &&
            (m_dutState != DutState::Complete || m_terminalReplyPending);
    if (!verifierAllowed) {
        ble.SetFactoryVerifierRuntimeAllowed(false);
        return;
    }
    const bool busy = m_terminalReplyPending ||
        (ble.IsFactoryBleDutRunning() && ble.HasFactoryBleCandidate());
    ble.SetFactoryVerifierRuntimeAllowed(!busy);
    UpdateVerifier(ble, nowMs);
}

bool FactoryRadioCoordinator::ConsumeDutBleVerified() {
    const bool pending = m_dutBleVerifiedPending;
    m_dutBleVerifiedPending = false;
    return pending;
}

bool FactoryRadioCoordinator::ConsumeVerifierBleVerified() {
    const bool pending = m_verifierBleVerifiedPending;
    m_verifierBleVerifiedPending = false;
    return pending;
}

} // namespace firmware

#endif
