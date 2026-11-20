#include "modules/support_runtime_stats.hpp"

#include "foxie_embedded/millis.hpp"
#if defined(ARDUINO) && defined(ESP32)
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_timer.h>
#endif

namespace modules {
namespace {
uint64_t Add(const uint64_t a, const uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
uint64_t Read(JsonObjectConst object, const char* key) {
    return object[key].is<uint64_t>() ? object[key].as<uint64_t>() : 0U;
}
struct SleepConfirmation {
    uint32_t magic;
    uint32_t kind;
    uint32_t confirmed;
    uint32_t check;
};
#if defined(ARDUINO) && defined(ESP32)
// SDK/RTC boundary only; never a flash journal or user-action history.
RTC_NOINIT_ATTR SleepConfirmation sleepConfirmation;
#else
SleepConfirmation sleepConfirmation{};
#endif
constexpr uint32_t SLEEP_MAGIC = 0x53555031U;
uint32_t Check(const SleepConfirmation& receipt) {
    return receipt.magic ^ receipt.kind ^ receipt.confirmed ^ 0xA71D6E09U;
}
bool ConsumeSleep(SupportSleepKind& kind) {
    const auto receipt = sleepConfirmation;
    sleepConfirmation = {};
    bool wakeConfirmed = receipt.confirmed == 1U;
#if defined(ARDUINO) && defined(ESP32)
    wakeConfirmed = (wakeConfirmed && esp_reset_reason() == ESP_RST_SW) ||
                    esp_reset_reason() == ESP_RST_DEEPSLEEP;
#endif
    if (receipt.magic != SLEEP_MAGIC || receipt.check != Check(receipt) ||
        receipt.kind > static_cast<uint32_t>(SupportSleepKind::Other) ||
        !wakeConfirmed) return false;
    kind = static_cast<SupportSleepKind>(receipt.kind);
    return true;
}
} // namespace

uint64_t SupportUptimeMs() {
#if defined(ARDUINO) && defined(ESP32)
    return static_cast<uint64_t>(esp_timer_get_time()) / 1000U;
#else
    return fxe::Millis();
#endif
}

void SupportRuntimeStats::BeginBoot(const JsonDocument& document) {
    if (m_started) return;
    m_started = true;
    m_base.available = true;
    const JsonObjectConst stats = document[KEY].as<JsonObjectConst>();
    if (document.containsKey(KEY) && (stats.isNull() || stats["v"] != 1U)) {
        // Unknown stored metrics must not block an ordinary settings save or
        // be silently downgraded. Preserve their opaque owning object.
        m_canWrite = false;
        m_base.available = false;
    }
    if (stats["v"] == 1U) {
        m_base.boots = Read(stats, "boots");
        m_base.priorRuntimeMs = Add(Read(stats, "prior_runtime_ms"),
                                     Read(stats, "boot_checkpoint_ms"));
        m_base.automaticSleeps = Read(stats, "sleep_auto");
        m_base.manualSleeps = Read(stats, "sleep_manual");
        m_base.otherSleeps = Read(stats, "sleep_other");
    }
    m_base.boots = Add(m_base.boots, 1U);
    SupportSleepKind sleepKind{};
    if (ConsumeSleep(sleepKind)) {
        uint64_t* count = sleepKind == SupportSleepKind::Automatic
            ? &m_base.automaticSleeps : sleepKind == SupportSleepKind::Manual
            ? &m_base.manualSleeps : &m_base.otherSleeps;
        *count = Add(*count, 1U);
    }
}

bool SupportRuntimeStats::PrepareSave(JsonDocument& document,
                                      const uint64_t uptimeMs) const {
    if (!m_started || !m_canWrite) return !document.overflowed();
    JsonObject stats = document[KEY].as<JsonObject>();
    if (stats.isNull()) stats = document.createNestedObject(KEY);
    stats["v"] = 1U;
    stats["boots"] = m_base.boots;
    stats["prior_runtime_ms"] = m_base.priorRuntimeMs;
    stats["boot_checkpoint_ms"] = uptimeMs;
    stats["sleep_auto"] = m_base.automaticSleeps;
    stats["sleep_manual"] = m_base.manualSleeps;
    stats["sleep_other"] = m_base.otherSleeps;
    return !document.overflowed();
}

void SupportRuntimeStats::NoteSaved(const JsonDocument& document) {
    if (!m_started) return;
    const JsonObjectConst stats = document[KEY].as<JsonObjectConst>();
    // Only this boot's verified completion may publish its checkpoint.
    if (stats["boots"] != m_base.boots ||
        stats["prior_runtime_ms"] != m_base.priorRuntimeMs) return;
    const uint64_t uptimeMs = Read(stats, "boot_checkpoint_ms");
    const uint64_t previous = Snapshot(0U).savedCheckpointMs;
    if (uptimeMs < previous) return;
    m_checkpointRevision.fetch_add(1U, std::memory_order_acq_rel);
    m_checkpointLow.store(static_cast<uint32_t>(uptimeMs), std::memory_order_relaxed);
    m_checkpointHigh.store(static_cast<uint32_t>(uptimeMs >> 32U), std::memory_order_relaxed);
    m_checkpointRevision.fetch_add(1U, std::memory_order_release);
}

SupportRuntimeSnapshot SupportRuntimeStats::Snapshot(const uint64_t uptimeMs) const {
    auto result = m_base;
    result.uptimeMs = uptimeMs;
    // Bounded snapshot; a concurrent publication reports no confirmed checkpoint
    // instead of waiting on the cooperative input/render task.
    const uint32_t before = m_checkpointRevision.load(std::memory_order_acquire);
    const uint32_t low = m_checkpointLow.load(std::memory_order_relaxed);
    const uint32_t high = m_checkpointHigh.load(std::memory_order_relaxed);
    const uint32_t after = m_checkpointRevision.load(std::memory_order_acquire);
    if ((before & 1U) == 0U && before == after) {
        result.savedCheckpointMs = (uint64_t{high} << 32U) | low;
    }
    return result;
}

void SupportRuntimeStats::ArmSleepConfirmation(const SupportSleepKind kind) {
    sleepConfirmation = {SLEEP_MAGIC, static_cast<uint32_t>(kind), 0U, 0U};
    sleepConfirmation.check = Check(sleepConfirmation);
}
void SupportRuntimeStats::CancelSleepConfirmation() { sleepConfirmation = {}; }
void SupportRuntimeStats::ConfirmLightSleep() {
    if (sleepConfirmation.magic != SLEEP_MAGIC ||
        sleepConfirmation.check != Check(sleepConfirmation)) return;
    sleepConfirmation.confirmed = 1U;
    sleepConfirmation.check = Check(sleepConfirmation);
}
} // namespace modules
