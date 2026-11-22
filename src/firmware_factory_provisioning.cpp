#include "firmware/factory_provisioning.hpp"

#ifdef ARDUINO

#include <nvs.h>

#include "foxie_embedded/elapsed_time.hpp"
#include "logger.hpp"
#include "modules/board_identity_persistence.hpp"
#include "modules/hardware_test_record.hpp"

namespace firmware {
namespace {

constexpr char NVS_NAMESPACE[] = "litch_factory";
constexpr char NVS_KEY[] = "provision_v1";
constexpr uint32_t FLASH_ACCESS_TIMEOUT_MS = 3000U;

bool HasCompletedRecord(const JsonDocument& settings,
                        const hardware::BoardRevision revision) {
    modules::HardwareTestRecord record;
    const char* const encoded =
        settings[modules::HARDWARE_TEST_RECORD_SETTING_KEY] | "";
    return modules::DecodeHardwareTestRecord(
               encoded != nullptr ? std::string_view(encoded) :
                                    std::string_view{}, record) &&
        record.revision == revision && modules::IsVerifiedFactoryPass(record);
}

} // namespace

void FactoryProvisioning::Begin(
    const hardware::BoardRevision revision,
    const JsonDocument& settings,
    hardware::RmtFlashArbiter& rmtFlashArbiter) {
    m_revision = revision;
    m_rmtFlashArbiter = &rmtFlashArbiter;
    modules::BoardIdentity identity;
    const bool hasInstalledIdentity =
        modules::ReadBoardIdentity(settings, identity) ==
        modules::BoardIdentityRead::Current;
    m_bootInitialization = true;
    m_policy.Begin(
        revision,
        hasInstalledIdentity,
        HasCompletedRecord(settings, revision),
        &ReadState,
        &WriteState,
        this);
    m_bootInitialization = false;
    logger::Info(
        "factory: provisioning_state=%u auto_test=%u",
        static_cast<unsigned>(m_policy.State()),
        static_cast<unsigned>(m_policy.ShouldAutoEnterHardwareTest()));
}

void FactoryProvisioning::OnDurableSettingsSnapshot(
    const JsonDocument& settings, const uint32_t receipt) {
    modules::HardwareTestRecord record;
    const char* encoded = settings[modules::HARDWARE_TEST_RECORD_SETTING_KEY] | "";
    if (!modules::DecodeHardwareTestRecord(encoded, record) || record.revision != m_revision) return;
    const bool overrideResult = record.radioResult == modules::ManufacturingTestRadioResult::Override;
    const bool fresh = receipt != 0U &&
        receipt == m_expectedReceipt.load(std::memory_order_acquire) &&
        !m_expectedResult.View().empty() && m_expectedResult.View() == std::string_view(encoded);
    (void)m_policy.OnDurableHardwareTestRecord(
        modules::IsVerifiedFactoryPass(record) || overrideResult, overrideResult, fresh);
}

modules::FactoryProvisioningState FactoryProvisioning::ReadState(
    void* const context) {
    auto* const self = static_cast<FactoryProvisioning*>(context);
    if (self == nullptr ||
        !hardware::SupportsProductionHardwareTest(self->m_revision)) {
        return modules::FactoryProvisioningState::Unavailable;
    }
    nvs_handle_t handle = 0U;
    const esp_err_t open = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (open == ESP_ERR_NVS_NOT_FOUND) {
        return modules::FactoryProvisioningState::Absent;
    }
    if (open != ESP_OK) {
        return modules::FactoryProvisioningState::Unavailable;
    }
    modules::FactoryProvisioningRecord record{};
    size_t size = record.size();
    const esp_err_t read = nvs_get_blob(handle, NVS_KEY, record.data(), &size);
    nvs_close(handle);
    if (read == ESP_ERR_NVS_NOT_FOUND) {
        return modules::FactoryProvisioningState::Absent;
    }
    if (read != ESP_OK) {
        return modules::FactoryProvisioningState::Unavailable;
    }
    if (size != record.size()) {
        return modules::FactoryProvisioningState::Invalid;
    }
    return modules::DecodeFactoryProvisioningRecord(record);
}

bool FactoryProvisioning::WriteState(
    void* const context,
    const modules::FactoryProvisioningState state) {
    auto* const self = static_cast<FactoryProvisioning*>(context);
    return self != nullptr && self->WriteState(state);
}

bool FactoryProvisioning::WriteState(
    const modules::FactoryProvisioningState state) {
    modules::FactoryProvisioningRecord record{};
    if (!modules::EncodeFactoryProvisioningRecord(state, record)) {
        return false;
    }
    bool flashHeld = false;
    if (!m_bootInitialization) {
        if (m_rmtFlashArbiter == nullptr) {
            return false;
        }
        const auto elapsed = fxe::ElapsedTime::StartNow();
        m_rmtFlashArbiter->RequestFlash();
        while (!m_rmtFlashArbiter->TryBeginRequestedFlash()) {
            if (elapsed.HasElapsed(FLASH_ACCESS_TIMEOUT_MS)) {
                m_rmtFlashArbiter->CancelFlashRequest();
                logger::Error("factory: NVS promotion timed out waiting for LED flash exclusion");
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1U));
        }
        flashHeld = true;
    }

    nvs_handle_t handle = 0U;
    const esp_err_t open = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    esp_err_t result = open;
    if (open == ESP_OK) {
        result = nvs_set_blob(handle, NVS_KEY, record.data(), record.size());
        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (flashHeld) {
        m_rmtFlashArbiter->EndFlash();
    }
    if (result != ESP_OK) {
        logger::Error("factory: NVS provisioning write failed err=%d",
                      static_cast<int>(result));
        return false;
    }
    return true;
}

} // namespace firmware

#endif
