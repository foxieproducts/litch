#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "hardware/board_revision.hpp"

namespace modules {

enum class FactoryProvisioningState : uint8_t {
    Absent = 0U,
    Pending,
    Passed,
    Invalid,
    Unavailable,
    LegacyExempt,
    Override,
    Required, // Explicit clear/override re-entry; old records cannot exempt it.
};

inline constexpr uint8_t FACTORY_PROVISIONING_RECORD_VERSION = 1U;
using FactoryProvisioningRecord = std::array<uint8_t, 8U>;

bool EncodeFactoryProvisioningRecord(FactoryProvisioningState state,
                                     FactoryProvisioningRecord& out);
FactoryProvisioningState DecodeFactoryProvisioningRecord(
    const FactoryProvisioningRecord& record);

class FactoryProvisioningPolicy final {
  public:
    using ReadState = FactoryProvisioningState (*)(void*);
    using WriteState = bool (*)(void*, FactoryProvisioningState);

    void Begin(hardware::BoardRevision revision,
               bool hasInstalledIdentity,
               bool hasCompletedHardwareTest,
               ReadState readState,
               WriteState writeState,
               void* context);

    bool OnDurableHardwareTestRecord(bool recordMatchesRevision, bool overrideResult = false,
                                     bool freshFactoryResult = false);
    bool RequireFactoryTest();
    bool AllowsPhysicalUpEntry() const {
        return hardware::SupportsProductionHardwareTest(m_revision);
    }
    bool ShouldAutoEnterHardwareTest() const {
        return m_autoEnter.load(std::memory_order_acquire);
    }
    bool AllowsCompletedHardwareTestResult() const {
        const auto state = m_state.load(std::memory_order_acquire);
        return state == FactoryProvisioningState::Passed ||
               state == FactoryProvisioningState::LegacyExempt;
    }
    bool AllowsNewBoardIdentity() const {
        const auto state = m_state.load(std::memory_order_acquire);
        return state == FactoryProvisioningState::Override ||
               state == FactoryProvisioningState::Required ||
               state == FactoryProvisioningState::Pending ||
               state == FactoryProvisioningState::Passed ||
               state == FactoryProvisioningState::LegacyExempt;
    }
    FactoryProvisioningState State() const {
        return m_state.load(std::memory_order_acquire);
    }

  private:
    bool Write(FactoryProvisioningState state);

    hardware::BoardRevision m_revision = hardware::BoardRevision::Unknown;
    std::atomic<FactoryProvisioningState> m_state{
        FactoryProvisioningState::Unavailable};
    WriteState m_writeState = nullptr;
    void* m_context = nullptr;
    std::atomic<bool> m_autoEnter{false};
};

} // namespace modules
