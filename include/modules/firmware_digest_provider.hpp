#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(LITCH_ESP32)
#include <esp_err.h>
#if !defined(LITCH_ESP32S3)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
#endif

namespace hardware { class RmtFlashArbiter; }

namespace modules {

enum class FirmwareDigestState : uint8_t {
    Unavailable = 0,
    Pending,
    Ready,
    Error,
};

class FirmwareDigestProvider {
  public:
    static constexpr size_t DIGEST_BYTES = 32U;

    explicit FirmwareDigestProvider(hardware::RmtFlashArbiter* flashArbiter = nullptr)
        : m_flashArbiter(flashArbiter) {}
    [[nodiscard]] hardware::RmtFlashArbiter* FlashArbiter() const {
        return m_flashArbiter;
    }

    [[nodiscard]] FirmwareDigestState State() const;
    bool TryBegin();
    bool TryBeginStandalone(uint32_t nowMs) {
        if (!TryBegin()) return false;
        m_standaloneStartedMs = nowMs; // Published with the worker handoff.
        return true;
    }
    uint32_t StandaloneStartedMs() const { return m_standaloneStartedMs; }
    void Complete(const uint8_t* digest);
    void CompleteStandalone(const uint8_t* digest, uint32_t timeoutMs);
    void Fail();
    bool CopyDigest(uint8_t* outDigest, size_t outLen) const;

  private:
    bool CopyPendingDigest(const uint8_t* digest);
    hardware::RmtFlashArbiter* const m_flashArbiter;
    std::atomic<FirmwareDigestState> m_state = FirmwareDigestState::Unavailable;
    uint32_t m_standaloneStartedMs = 0U;
    uint8_t m_digest[DIGEST_BYTES] = {};
#if defined(LITCH_ESP32) && !defined(LITCH_ESP32S3)
    // Targets without a post-boot worker keep one lazily started digest task.
    friend bool StartEsp32FirmwareDigest(FirmwareDigestProvider& provider);
    static constexpr uint32_t TASK_STACK_BYTES = 4096U;
    static_assert(TASK_STACK_BYTES % sizeof(StackType_t) == 0U);
    alignas(16) StackType_t m_taskStack[TASK_STACK_BYTES / sizeof(StackType_t)]{};
    StaticTask_t m_taskControl{};
    TaskHandle_t m_task = nullptr;
#endif
};

bool StartEsp32FirmwareDigest(FirmwareDigestProvider& provider);
#if defined(LITCH_ESP32)
/** Worker only. An urgent boot request defers the optional read without publishing a partial digest. */
esp_err_t ReadEsp32FirmwareDigest(FirmwareDigestProvider& provider, uint8_t* digest,
                                  const std::atomic<bool>* urgentRequest);
#endif

} // namespace modules
