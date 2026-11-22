#pragma once

#ifdef ARDUINO

#include <ArduinoJson.h>
#include <atomic>

#include "hardware/board_revision.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "modules/factory_provisioning_policy.hpp"
#include "modules/hardware_test_record.hpp"

namespace firmware {

class FactoryProvisioning final {
  public:
    void Begin(hardware::BoardRevision revision,
               const JsonDocument& settings,
               hardware::RmtFlashArbiter& rmtFlashArbiter);
    void OnDurableSettingsSnapshot(const JsonDocument& settings, uint32_t receipt = 0U);
    bool AllowsPhysicalUpEntry() const { return m_policy.AllowsPhysicalUpEntry(); }
    bool RequireFactoryTest() { return m_policy.RequireFactoryTest(); }
    void ExpectFactoryResult(const modules::HardwareTestRecord& record) {
        m_expectedReceipt.store(0U, std::memory_order_release);
        (void)modules::EncodeHardwareTestRecord(record, m_expectedResult);
    }
    std::atomic<uint32_t>* ExpectedReceiptPublication() { return &m_expectedReceipt; }
    void ForgetExpectedResult() {
        m_expectedReceipt.store(0U, std::memory_order_release);
        m_expectedResult.Clear();
    }

    bool ShouldAutoEnterHardwareTest() const {
        return m_policy.ShouldAutoEnterHardwareTest();
    }
    bool AllowsCompletedHardwareTestResult() const {
        return m_policy.AllowsCompletedHardwareTestResult();
    }
    bool AllowsNewBoardIdentity() const {
        return m_policy.AllowsNewBoardIdentity();
    }
    modules::FactoryProvisioningState State() const {
        return m_policy.State();
    }

  private:
    static modules::FactoryProvisioningState ReadState(void* context);
    static bool WriteState(void* context,
                           modules::FactoryProvisioningState state);
    bool WriteState(modules::FactoryProvisioningState state);

    modules::FactoryProvisioningPolicy m_policy;
    hardware::BoardRevision m_revision = hardware::BoardRevision::Unknown;
    hardware::RmtFlashArbiter* m_rmtFlashArbiter = nullptr;
    bool m_bootInitialization = false;
    // Set before EnqueueTracked; the queue publishes its exact generation
    // before the worker can take it. Immutable until that receipt completes.
    modules::EncodedHardwareTestRecord m_expectedResult;
    std::atomic<uint32_t> m_expectedReceipt{0U};
};

} // namespace firmware

#endif
