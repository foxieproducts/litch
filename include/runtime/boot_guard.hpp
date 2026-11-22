#pragma once

#include <cstdint>

namespace runtime {

class BootGuard {
  public:
    struct Snapshot {
        bool safeModeActive = false;
        bool retryRebootArmed = false;
        uint32_t bootCount = 0;
        uint32_t earlyCrashCount = 0;
        uint32_t lastResetReason = 0;
    };

    // Call as early as possible during setup().
    void OnBootStart();

    // Call once the firmware has been stable for a while.
    void MarkBootHealthy();

    // Call immediately before a deliberate software restart. The next boot
    // consumes the intent without erasing prior crash counts or the latch;
    // panic/watchdog resets remain crashes even if this was armed.
    void PrepareForIntentionalReboot();

    bool IsSafeModeActive() const { return m_safeModeActive; }

    // True for panic/watchdog or an unmarked software reset.
    bool IsLastResetCrashy() const;

    // True only for a fresh power-on reset. Recovery gestures that can erase
    // data must not arm after software, watchdog, brownout, or wake resets.
    bool IsColdPowerOnReset() const;

    // Clears the safe-mode latch and counters. Intended for manual recovery.
    void Clear();

    // Arms one later ordinary reboot to become a non-destructive retry without
    // mutating RTC crash/latch evidence while the current recovery process
    // remains alive.
    void ArmRetryReboot();

    // Returns whether a later ordinary reboot was armed, and consumes that
    // volatile intent so it must be re-armed after any cancellation/failure.
    bool ConsumeRetryRebootArm();

    // Clears only the latch/counters needed for an explicitly requested normal
    // boot retry. Persisted settings and the broader boot diagnostics remain.
    void PrepareForRetryReboot();

    Snapshot GetSnapshot() const;

  private:
    bool m_safeModeActive = false;
    bool m_retryRebootArmed = false;
    bool m_lastResetWasIntentional = false;
    uint32_t m_lastResetReason = 0;
};

} // namespace runtime
