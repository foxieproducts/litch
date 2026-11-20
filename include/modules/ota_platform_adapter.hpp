#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/ota_image_identity.hpp"
#include "modules/ota_post_boot_verifier.hpp"

namespace modules {

/** Latest SDK observation in this boot; age must be reported by the reader. */
struct OtaBootEvidence {
    uint32_t sample = 0U;
    uint32_t capturedAtMs = 0U;
    bool available = false;
    OtaPostBootSnapshot snapshot;
    OtaPostBootRunningState runningState = OtaPostBootRunningState::Unknown;
};

enum class OtaAdmissionPhase : uint8_t {
    None = 0U,
    Receiving,
    Activated,
    Aborted,
    Failed,
};

/** RAM-only evidence of this boot's latest platform Begin, never authority. */
struct OtaAdmissionEvidence {
    uint32_t attempt = 0U;
    uint32_t sourceSequence = 0U;
    uint32_t expectedSequence = 0U;
    OtaPostBootSlotIdentity source;
    OtaPostBootSlotIdentity target;
    OtaAdmissionPhase phase = OtaAdmissionPhase::None;
};

/** BLE-facing boundary that owns the complete platform update transaction. */
class OtaPlatformAdapter {
  public:
    virtual ~OtaPlatformAdapter() = default;
    virtual bool Begin(uint32_t expectedSize) = 0;
    virtual bool Write(const uint8_t* data, size_t len) = 0;
    virtual bool End() = 0;
    virtual void Abort() = 0;
    /** Nonblocking cached copy; unsupported adapters provide no evidence. */
    virtual bool ReadAdmissionEvidence(OtaAdmissionEvidence&) const {
        return false;
    }
    /** Nonblocking RAM-only SDK cache copy; never reads flash or hashes. */
    virtual bool ReadBootEvidence(OtaBootEvidence&) const { return false; }
};

struct OtaPlatformTransactionOps {
    void* context = nullptr;
    uint16_t expectedChipId = 0U;
    bool (*acquireFlash)(void*) = nullptr;
    void (*releaseFlash)(void*) = nullptr;
    bool (*runningImageCanBeReplaced)(void*) = nullptr;
    bool (*captureTarget)(void*,
                          uint32_t,
                          OtaPostBootSlotIdentity&) = nullptr;
    bool (*updateBegin)(void*, uint32_t) = nullptr;
    size_t (*updateWrite)(void*, const uint8_t*, size_t) = nullptr;
    bool (*targetCanBeActivated)(void*,
                           const OtaPostBootSlotIdentity&,
                           const OtaImageIdentity&) = nullptr;
    bool (*updateEnd)(void*) = nullptr;
    void (*updateAbort)(void*) = nullptr;
};

/**
 * Pure transaction coordinator used by the S3 adapter and native fakes.
 *
 * Target capture and every Update operation run under a flash lease. The
 * complete source-image verification happens before the begin lease. Final
 * activation requires epoch-2 integrity and a fresh target/source guard under
 * the same lease as updateEnd. SDK OTA metadata is the only boot authority.
 */
class OtaPlatformTransaction final : public OtaPlatformAdapter {
  public:
    explicit OtaPlatformTransaction(OtaPlatformTransactionOps ops)
        : m_ops(ops) {}

    bool Begin(uint32_t expectedSize) override;
    bool Write(const uint8_t* data, size_t len) override;
    bool End() override;
    void Abort() override;

    [[nodiscard]] bool IsActive() const { return m_active; }

  private:
    [[nodiscard]] bool OpsComplete() const;
    void ResetOwnedState();

    OtaPlatformTransactionOps m_ops;
    OtaImageIdentityCollector m_identity;
    OtaPostBootSlotIdentity m_target{};
    bool m_active = false;
};

} // namespace modules
