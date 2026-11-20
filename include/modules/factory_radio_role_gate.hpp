#pragma once

#include <array>

#include "modules/manufacturing_test_session.hpp"

namespace modules {

inline constexpr bool IsFactoryRadioStageReady(const ManufacturingTestStage stage) {
    return stage == ManufacturingTestStage::Radio ||
        stage == ManufacturingTestStage::Persisting || stage == ManufacturingTestStage::Passed;
}

// One bounded policy for the production coordinator and deterministic journeys.
// Discovery is a routing hint only: callers report proof after the v3 BLE exchange.
class FactoryRadioRoleGate final {
  public:
    static constexpr uint32_t DISCOVERY_MS = 1500U;
    enum class Role : uint8_t { Inactive, Discovering, Verifier, Complete };

    void Begin(uint32_t nowMs, uint32_t randomValue = 1500U) {
        m_role = Role::Discovering;
        m_searchStartedMs = nowMs;
        m_discoveryMs = randomValue % (DISCOVERY_MS + 1U);
    }
    void Stop() { m_role = Role::Inactive; }
    void ObserveEligiblePeer(uint32_t nowMs) {
        if (CanSearch()) {
            m_searchStartedMs = nowMs;
        }
    }
    void Tick(uint32_t nowMs) {
        if (m_role == Role::Discovering &&
            static_cast<uint32_t>(nowMs - m_searchStartedMs) >= m_discoveryMs) {
            m_role = Role::Verifier;
        }
    }
    void AcceptBleProof() {
        if (m_role != Role::Inactive && m_role != Role::Complete) {
            m_role = Role::Complete;
        }
    }
    uint32_t DiscoveryDeadlineMs() const { return m_discoveryMs; }
    Role GetRole() const { return m_role; }
    bool IsVerifier() const { return m_role == Role::Verifier; }
    bool IsActive() const { return m_role != Role::Inactive; }
    bool CanSearch() const {
        return m_role == Role::Discovering || m_role == Role::Verifier;
    }

    // Stable public controller address is election identity, never evidence.
    // Strict ordering gives at most N-1 downward links for N simultaneous units.
    static bool PeerEligible(uint64_t self, uint64_t peer, bool verifier) {
        return peer != 0U && peer != self && (!verifier || peer < self);
    }
    static uint32_t RetryDelayMs(uint64_t self, uint32_t attempt) {
        // Rotate all six address bytes: adjacent boards do not retry in lockstep.
        const uint8_t slot = static_cast<uint8_t>(self >> ((attempt % 6U) * 8U));
        return 1000U + static_cast<uint32_t>(slot) * 4U;
    }

  private:
    Role m_role = Role::Inactive;
    uint32_t m_searchStartedMs = 0U;
    uint32_t m_discoveryMs = DISCOVERY_MS;
};

// Failed advertisements cannot monopolize a batch. Eight entries match the
// bounded scan result set; expiry is wrap-safe and replacement chooses oldest.
class FactoryRadioPeerAttempts final {
  public:
    static constexpr uint32_t RETRY_AFTER_MS = 6000U;
    bool CanTry(uint64_t peer, uint32_t nowMs) const {
        for (const auto& failure : m_failures) {
            if (failure.peer == peer && peer != 0U &&
                static_cast<uint32_t>(nowMs - failure.atMs) < RETRY_AFTER_MS) {
                return false;
            }
        }
        return true;
    }
    void Failed(uint64_t peer, uint32_t nowMs) {
        if (peer == 0U) { return; }
        auto* target = &m_failures[0];
        for (auto& failure : m_failures) {
            if (failure.peer == 0U || failure.peer == peer) {
                target = &failure;
                break;
            }
            if (static_cast<uint32_t>(nowMs - failure.atMs) >
                static_cast<uint32_t>(nowMs - target->atMs)) {
                target = &failure;
            }
        }
        *target = Failure{peer, nowMs};
    }
  private:
    struct Failure { uint64_t peer = 0U; uint32_t atMs = 0U; };
    std::array<Failure, 8U> m_failures{};
};

} // namespace modules
