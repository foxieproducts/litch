#include "firmware/ota_platform_adapter.hpp"

#include <cstddef>
#include <cstdint>

#include "hardware/rmt_flash_arbiter.hpp"
#include "firmware/ota_runtime_evidence_internal.hpp"
#include "modules/ota_slot_policy.hpp"

#if defined(ARDUINO) && defined(LITCH_ESP32S3)
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace firmware {
namespace {

#if defined(ARDUINO) && defined(LITCH_ESP32S3)

constexpr uint16_t ESP32S3_IMAGE_CHIP_ID = 9U;
constexpr uint32_t FLASH_ACQUIRE_TIMEOUT_MS = 5000U;

bool AcquireFlash(hardware::RmtFlashArbiter& arbiter) {
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout =
        pdMS_TO_TICKS(FLASH_ACQUIRE_TIMEOUT_MS);
    arbiter.RequestFlash();
    while (!arbiter.TryBeginRequestedFlash()) {
        if (static_cast<TickType_t>(xTaskGetTickCount() - started) >= timeout) {
            arbiter.CancelFlashRequest();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    return true;
}

class Esp32S3OtaPlatformAdapter final : public modules::OtaPlatformAdapter {
  public:
    explicit Esp32S3OtaPlatformAdapter(hardware::RmtFlashArbiter& arbiter)
        : m_arbiter(arbiter),
          m_transaction({this,
                         ESP32S3_IMAGE_CHIP_ID,
                         &Acquire,
                         &Release,
                         &RunningImageCanBeReplaced,
                         &CaptureTarget,
                         &UpdateBegin,
                         &UpdateWrite,
                         &TargetCanBeActivated,
                         &UpdateEnd,
                         &UpdateAbort}) {}

    modules::OtaPlatformAdapter& Adapter() { return *this; }

    bool Begin(const uint32_t expectedSize) override {
        const uint32_t attempt = m_admission.attempt + 1U;
        m_admission = {};
        m_admission.attempt = attempt == 0U ? 1U : attempt;
        ota_evidence_internal::RecordAdmission(m_admission);
        const bool begun = m_transaction.Begin(expectedSize);
        if (begun) {
            m_admission.source = m_source.running;
            m_admission.target = m_target;
            m_admission.sourceSequence = m_source.selectedSequence;
            m_admission.expectedSequence = m_expectedSequence;
        }
        PublishAdmission(begun ? modules::OtaAdmissionPhase::Receiving
                               : modules::OtaAdmissionPhase::Failed);
        return begun;
    }

    bool Write(const uint8_t* data, const size_t len) override {
        const bool written = m_transaction.Write(data, len);
        if (!written) PublishAdmission(modules::OtaAdmissionPhase::Failed);
        return written;
    }

    bool End() override {
        const bool ended = m_transaction.End();
        PublishAdmission(ended ? modules::OtaAdmissionPhase::Activated
                               : modules::OtaAdmissionPhase::Failed);
        return ended;
    }

    void Abort() override {
        const bool owned = m_transaction.IsActive();
        m_transaction.Abort();
        if (owned) {
            PublishAdmission(m_transaction.IsActive()
                                 ? modules::OtaAdmissionPhase::Failed
                                 : modules::OtaAdmissionPhase::Aborted);
        }
    }

    bool ReadAdmissionEvidence(modules::OtaAdmissionEvidence& out) const override {
        return ota_evidence_internal::CopyAdmission(out);
    }

    bool ReadBootEvidence(modules::OtaBootEvidence& out) const override {
        return CopyEsp32OtaBootEvidence(out);
    }

  private:
    void PublishAdmission(const modules::OtaAdmissionPhase phase) {
        m_admission.phase = phase;
        ota_evidence_internal::RecordAdmission(m_admission);
    }

    bool ReadObservedSnapshot(modules::OtaPostBootSnapshot& out) {
        m_publishSnapshot = true;
        m_snapshotAvailable = ReadEsp32OtaBootSnapshotWithFlashLease(out);
        m_observedStamp = ota_evidence_internal::CaptureStamp();
        if (m_snapshotAvailable) m_observedSnapshot = out;
        return m_snapshotAvailable;
    }

    static Esp32S3OtaPlatformAdapter& Self(void* const context) {
        return *static_cast<Esp32S3OtaPlatformAdapter*>(context);
    }

    static bool Acquire(void* const context) {
        if (AcquireFlash(Self(context).m_arbiter)) return true;
        ota_evidence_internal::InvalidateSnapshot();
        return false;
    }

    static void Release(void* const context) {
        auto& self = Self(context);
        self.m_arbiter.EndFlash();
        if (self.m_publishSnapshot) {
            ota_evidence_internal::RecordSnapshot(
                self.m_snapshotAvailable ? &self.m_observedSnapshot : nullptr,
                self.m_observedStamp);
            self.m_publishSnapshot = false;
        }
    }

    static bool RunningImageCanBeReplaced(void* context) {
        auto& self = Self(context);
        self.m_sourceVerified = VerifyEsp32OtaUpdateSource(self.m_source);
        return self.m_sourceVerified;
    }

    static bool CaptureTarget(
        void* context,
        const uint32_t expectedSize,
        modules::OtaPostBootSlotIdentity& outTarget) {
        auto& self = Self(context);
        modules::OtaPostBootSnapshot fresh;
        if (!self.m_sourceVerified || !self.ReadObservedSnapshot(fresh) ||
            !modules::OtaPostBootSnapshotsMatch(self.m_source, fresh)) {
            return false;
        }
        const esp_partition_t* const target =
            esp_ota_get_next_update_partition(nullptr);
        const esp_partition_t* const running =
            esp_ota_get_running_partition();
        if (target == nullptr || running == nullptr ||
            target->type != ESP_PARTITION_TYPE_APP ||
            running->type != ESP_PARTITION_TYPE_APP ||
            target->address == 0U || running->address == 0U ||
            target->address == running->address ||
            expectedSize == 0U || expectedSize > target->size ||
            !modules::IsRollbackPreservingOtaRoute(
                running->subtype, target->subtype)) {
            return false;
        }
        // Pinned IDF chooses the first sequence >= the active one with the
        // target's parity. For this verified opposite-slot route that is the
        // next sequence. Keep the checked mapping explicit and reject wrap.
        if (fresh.selectedSequence >= UINT32_MAX - 2U) return false;
        uint32_t expectedSequence = fresh.selectedSequence + 1U;
        const uint32_t targetIndex = target->subtype - modules::OTA_SLOT_0_SUBTYPE;
        if ((expectedSequence - 1U) % 2U != targetIndex) ++expectedSequence;
        outTarget.address = target->address;
        outTarget.subtype = target->subtype;
        self.m_target = outTarget;
        self.m_expectedSequence = expectedSequence;
        return true;
    }

    static bool UpdateBegin(void*, const uint32_t expectedSize) {
        return ::Update.begin(expectedSize);
    }

    static size_t UpdateWrite(void*,
                              const uint8_t* const data,
                              const size_t len) {
        return ::Update.write(const_cast<uint8_t*>(data), len);
    }

    static bool TargetCanBeActivated(
        void* context,
        const modules::OtaPostBootSlotIdentity& target,
        const modules::OtaImageIdentity& identity) {
        auto& self = Self(context);
        modules::OtaPostBootSnapshot fresh;
        return self.m_sourceVerified && target.address == self.m_target.address &&
               target.subtype == self.m_target.subtype &&
               identity.bootPolicy.status == modules::OtaBootPolicyStatus::Known &&
               identity.bootPolicy.epoch == modules::OTA_BOOT_POLICY_SDK_EPOCH &&
               self.ReadObservedSnapshot(fresh) &&
               modules::OtaPostBootSnapshotsMatch(self.m_source, fresh);
    }

    static bool UpdateEnd(void* context) {
        auto& self = Self(context);
        // Any failed activation/readback leaves current SDK state unknown.
        self.m_publishSnapshot = true;
        self.m_snapshotAvailable = false;
        self.m_observedStamp = ota_evidence_internal::CaptureStamp();
        if (!::Update.end(false) || ::Update.hasError()) return false;
        modules::OtaPostBootSnapshot selected;
        if (!self.ReadObservedSnapshot(selected) ||
            selected.selected.address != self.m_target.address ||
            selected.selected.subtype != self.m_target.subtype ||
            selected.running.address != self.m_source.running.address ||
            selected.selectedSequence != self.m_expectedSequence ||
            selected.selectedEntry == self.m_source.selectedEntry ||
            selected.selectors[self.m_source.selectedEntry] !=
                self.m_source.selectors[self.m_source.selectedEntry]) {
            return false;
        }
        const auto& entry = selected.selectors[selected.selectedEntry];
        // SDK activation publishes NEW. Its raw record is the attempt identity;
        // the preceding VALID selector remains untouched as rollback.
        return entry[24U] == 0U && entry[25U] == 0U &&
               entry[26U] == 0U && entry[27U] == 0U;
    }

    static void UpdateAbort(void* context) {
        ::Update.abort();
        Self(context).m_sourceVerified = false;
    }

    hardware::RmtFlashArbiter& m_arbiter;
    modules::OtaPlatformTransaction m_transaction;
    modules::OtaPostBootSnapshot m_source{};
    modules::OtaPostBootSlotIdentity m_target{};
    modules::OtaPostBootSnapshot m_observedSnapshot{};
    modules::OtaAdmissionEvidence m_admission{};
    ota_evidence_internal::SnapshotStamp m_observedStamp{};
    uint32_t m_expectedSequence = 0U;
    bool m_sourceVerified = false;
    bool m_publishSnapshot = false;
    bool m_snapshotAvailable = false;
};

#endif

} // namespace

modules::OtaPlatformAdapter* InitializeEsp32OtaPlatformAdapter(
    hardware::RmtFlashArbiter& flashArbiter) {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    if (!InitializeEsp32OtaPostBootWorker(flashArbiter)) {
        return nullptr;
    }
    // Program-lifetime SDK adapter: BLE retains this non-owning pointer.
    static Esp32S3OtaPlatformAdapter adapter(flashArbiter);
    return &adapter.Adapter();
#else
    (void)flashArbiter;
    return nullptr;
#endif
}

} // namespace firmware
