#include "runtime/boot_guard.hpp"

#ifdef ARDUINO

#include <esp_attr.h>
#include <esp_system.h>

#endif

namespace runtime {

namespace {

#ifdef ARDUINO

// RTC no-init memory survives software resets and deep sleep.
// This gives us a cheap way to detect crash loops (e.g. panic -> SW reset)
// and fall back into a minimal safe mode.
RTC_NOINIT_ATTR uint32_t g_bootGuardMagic = 0;
RTC_NOINIT_ATTR uint32_t g_bootInProgress = 0;
RTC_NOINIT_ATTR uint32_t g_bootCount = 0;
RTC_NOINIT_ATTR uint32_t g_earlyCrashCount = 0;
RTC_NOINIT_ATTR uint32_t g_safeModeLatch = 0;
RTC_NOINIT_ATTR uint32_t g_lastResetReason = 0;

static constexpr uint32_t EARLY_CRASH_THRESHOLD = 3;
static constexpr uint32_t BOOT_GUARD_MAGIC = 0x4C495443U; // 'LITC'
// Reuse the existing retained word: 0 is healthy, 1 is unfinished. Older
// firmware conservatively treats the new nonzero intent as unfinished.
static constexpr uint32_t BOOT_IN_PROGRESS = 1U;
static constexpr uint32_t INTENTIONAL_REBOOT = 2U;

uint32_t GetResetReasonU32() {
    return static_cast<uint32_t>(esp_reset_reason());
}

void EnsureRtcStateInitialized() {
    // RTC_NOINIT memory is not guaranteed to start zeroed on first boot.
    // Guard all counters/latches with a magic value so random RTC contents
    // don't look like giant boot counts or cause spurious safe-mode entry.
    if (g_bootGuardMagic == BOOT_GUARD_MAGIC) {
        return;
    }

    g_bootGuardMagic = BOOT_GUARD_MAGIC;
    g_bootInProgress = 0U;
    g_bootCount = 0U;
    g_earlyCrashCount = 0U;
    g_safeModeLatch = 0U;
    g_lastResetReason = 0U;
}

bool IsCrashyResetReason(const uint32_t reasonU32) {
    // The boot loop we saw was rst:0xc (RTC_SW_CPU_RST) which maps to SW reset.
    // Treat software resets and watchdogs as "crashy" for the purposes of safe mode.
    switch (static_cast<esp_reset_reason_t>(reasonU32)) {
        case ESP_RST_SW:
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return true;
        default:
            return false;
    }
}

bool IsHardResetReason(const uint32_t reasonU32) {
    // If we truly lost power (or browned out), we should treat this as a fresh start.
    // RTC no-init memory can occasionally survive brief disconnects, so we must not
    // incorrectly carry safe-mode latches across power cycles.
    switch (static_cast<esp_reset_reason_t>(reasonU32)) {
        case ESP_RST_POWERON:
        case ESP_RST_BROWNOUT:
        case ESP_RST_EXT:
            return true;
        default:
            return false;
    }
}

#else

static constexpr uint32_t EARLY_CRASH_THRESHOLD = 3;

#endif

} // namespace

void BootGuard::OnBootStart() {
    m_retryRebootArmed = false;
    m_lastResetWasIntentional = false;
#ifdef ARDUINO
    EnsureRtcStateInitialized();

    const uint32_t resetReason = GetResetReasonU32();
    m_lastResetReason = resetReason;
    g_lastResetReason = resetReason;

    if (IsHardResetReason(resetReason)) {
        // Power-cycle: clear any stale latch/counters that might survive briefly.
        g_bootGuardMagic = BOOT_GUARD_MAGIC;
        g_bootInProgress = 0U;
        g_earlyCrashCount = 0U;
        g_safeModeLatch = 0U;
    }

    const bool lastBootDidNotComplete = (g_bootInProgress != 0U);
    m_lastResetWasIntentional = g_bootInProgress == INTENTIONAL_REBOOT &&
        resetReason == static_cast<uint32_t>(ESP_RST_SW);
    // Consume any intent before this boot's code can fail.
    g_bootInProgress = BOOT_IN_PROGRESS;
    g_bootCount++;

    if (lastBootDidNotComplete && IsCrashyResetReason(resetReason) &&
        !m_lastResetWasIntentional) {
        g_earlyCrashCount++;
    } else if (!lastBootDidNotComplete) {
        // If we previously reached a "healthy" point, decay the crash counter.
        g_earlyCrashCount = 0;
    }

    if (g_safeModeLatch != 0U || g_earlyCrashCount >= EARLY_CRASH_THRESHOLD) {
        g_safeModeLatch = 1U;
        m_safeModeActive = true;
    } else {
        m_safeModeActive = false;
    }
#else
    (void)EARLY_CRASH_THRESHOLD;
    m_safeModeActive = false;
    m_lastResetReason = 0;
#endif
}

void BootGuard::MarkBootHealthy() {
#ifdef ARDUINO
    g_bootInProgress = 0U;
    g_earlyCrashCount = 0U;
    // Note: we intentionally do not clear g_safeModeLatch here.
    // Safe mode is a manual-recovery latch, so we don't oscillate.
#endif
}

void BootGuard::PrepareForIntentionalReboot() {
#ifdef ARDUINO
    // A healthy boot must stay healthy if the SDK restart teardown fails.
    if (g_bootInProgress != 0U) {
        g_bootInProgress = INTENTIONAL_REBOOT;
    }
#endif
}

void BootGuard::Clear() {
#ifdef ARDUINO
    g_bootInProgress = 0U;
    g_earlyCrashCount = 0U;
    g_safeModeLatch = 0U;
#endif
    m_safeModeActive = false;
    m_retryRebootArmed = false;
}

void BootGuard::ArmRetryReboot() {
    m_retryRebootArmed = true;
}

bool BootGuard::ConsumeRetryRebootArm() {
    const bool armed = m_retryRebootArmed;
    m_retryRebootArmed = false;
    return armed;
}

void BootGuard::PrepareForRetryReboot() {
#ifdef ARDUINO
    g_bootInProgress = 0U;
    g_earlyCrashCount = 0U;
    g_safeModeLatch = 0U;
#endif
    m_safeModeActive = false;
    m_retryRebootArmed = false;
}

bool BootGuard::IsLastResetCrashy() const {
#ifdef ARDUINO
    return !m_lastResetWasIntentional && IsCrashyResetReason(g_lastResetReason);
#else
    return false;
#endif
}

bool BootGuard::IsColdPowerOnReset() const {
#ifdef ARDUINO
    return static_cast<esp_reset_reason_t>(g_lastResetReason) ==
           ESP_RST_POWERON;
#else
    return false;
#endif
}

BootGuard::Snapshot BootGuard::GetSnapshot() const {
    Snapshot s;
#ifdef ARDUINO
    s.safeModeActive = (g_safeModeLatch != 0U) || m_safeModeActive;
    s.retryRebootArmed = m_retryRebootArmed;
    s.bootCount = g_bootCount;
    s.earlyCrashCount = g_earlyCrashCount;
    s.lastResetReason = g_lastResetReason;
#else
    s.safeModeActive = m_safeModeActive;
    s.retryRebootArmed = m_retryRebootArmed;
    s.bootCount = 0;
    s.earlyCrashCount = 0;
    s.lastResetReason = m_lastResetReason;
#endif
    return s;
}

} // namespace runtime
