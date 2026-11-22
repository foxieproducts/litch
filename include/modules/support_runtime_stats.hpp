#pragma once

#include <atomic>
#include <cstdint>
#include <ArduinoJson.h>

namespace modules {

enum class SupportSleepKind : uint8_t { Automatic = 0U, Manual = 1U, Other = 2U };

struct SupportRuntimeSnapshot {
    bool available = false;
    uint64_t boots = 0U;
    uint64_t priorRuntimeMs = 0U;
    uint64_t uptimeMs = 0U;
    uint64_t savedCheckpointMs = 0U;
    uint64_t automaticSleeps = 0U;
    uint64_t manualSleeps = 0U;
    uint64_t otherSleeps = 0U;
};

// One boot owner, then immutable boot bases. Only the settings worker publishes
// saved checkpoints; the loop reads them without touching the worker's JSON.
class SupportRuntimeStats final {
  public:
    static constexpr const char* KEY = "SUPPORT_RUNTIME_STATS";
    void BeginBoot(const JsonDocument& document);
    bool PrepareSave(JsonDocument& document, uint64_t uptimeMs) const;
    void NoteSaved(const JsonDocument& document);
    SupportRuntimeSnapshot Snapshot(uint64_t uptimeMs) const;

    // RTC evidence is armed immediately before the SDK entry call. A returning
    // refusal clears it. Deep-sleep wake (or successful light sleep) confirms it;
    // BeginBoot consumes it once without writing any file.
    static void ArmSleepConfirmation(SupportSleepKind kind);
    static void CancelSleepConfirmation();
    static void ConfirmLightSleep();

  private:
    bool m_started = false;
    bool m_canWrite = true;
    SupportRuntimeSnapshot m_base{};
    std::atomic<uint32_t> m_checkpointRevision{0U};
    std::atomic<uint32_t> m_checkpointLow{0U};
    std::atomic<uint32_t> m_checkpointHigh{0U};
};

// Same full-width ESP timer used by the existing uptime Console command.
uint64_t SupportUptimeMs();

} // namespace modules
