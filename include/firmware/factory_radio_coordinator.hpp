#pragma once

#ifdef ARDUINO

#include <cstdint>

#include "modules/ble_service.hpp"
#include "modules/factory_radio_role_gate.hpp"
#include "modules/manufacturing_test_session.hpp"

namespace firmware {

#if defined(LITCH_HW_VARIANT_S3)

class FactoryRadioCoordinator final {
  public:
    void Update(bool hardwareTestActive,
                bool hardwareTestRadioActive,
                modules::ManufacturingTestStage hardwareTestStage,
                modules::BleService& ble,
                uint32_t nowMs, uint32_t runId = 0U);
    bool IsAutomaticVerifier() const { return m_roleGate.IsVerifier(); }
    uint32_t DiscoveryDeadlineMs() const { return m_roleGate.DiscoveryDeadlineMs(); }
    bool HasPeerCandidate() const { return m_peerCandidate; }
    bool BleEndpointStartFailed() const { return m_bleEndpointStartFailed; }
    bool ConsumeDutBleVerified();
    bool ConsumeVerifierBleVerified();
    [[nodiscard]] bool IsVerifierBleProofActive() const { return HasPendingTerminalReply(); }
    [[nodiscard]] bool HasPendingTerminalReply() const { return m_terminalReplyPending; }
    void PollTerminalReply(modules::BleService& ble, uint32_t nowMs);

  private:
    enum class DutState : uint8_t { Idle, Searching, Complete };

    void StartDut();
    void UpdateDut(modules::BleService& ble, uint32_t nowMs);
    void UpdateVerifier(modules::BleService& ble, uint32_t nowMs);
    void ResetRun();

    modules::FactoryRadioRoleGate m_roleGate;
    uint32_t m_activeRunId = 0U;
    bool m_hardwareTestActive = false;
    bool m_bleJobPending = false;
    bool m_peerCandidate = false;
    bool m_bleEndpointStartFailed = false;
    uint32_t m_bleAttempt = 0U;
    uint32_t m_bleRetryMs = 1000U;
    uint32_t m_lastEndpointAttemptMs = 0U;
    uint32_t m_lastBleAttemptMs = 0U;
    DutState m_dutState = DutState::Idle;
    modules::FactoryBleProof m_bleProof;
    bool m_dutBleVerifiedPending = false;
    bool m_verifierBleVerifiedPending = false;
    bool m_terminalReplyPending = false;
    uint32_t m_terminalReplyStartedMs = 0U;
};

#else

// The V1 local production test does not link the factory central role.
class FactoryRadioCoordinator final {
  public:
    void Update(bool, bool, modules::ManufacturingTestStage,
                modules::BleService&, uint32_t, uint32_t = 0U) {}
    bool IsAutomaticVerifier() const { return false; }
    uint32_t DiscoveryDeadlineMs() const { return 0U; }
    bool HasPeerCandidate() const { return false; }
    bool BleEndpointStartFailed() const { return false; }
    [[nodiscard]] bool ConsumeDutBleVerified() { return false; }
    [[nodiscard]] bool ConsumeVerifierBleVerified() { return false; }
    [[nodiscard]] bool IsVerifierBleProofActive() const { return false; }
    [[nodiscard]] bool HasPendingTerminalReply() const { return false; }
    void PollTerminalReply(modules::BleService&, uint32_t) {}
};

#endif

} // namespace firmware

#endif
