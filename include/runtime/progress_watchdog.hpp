#pragma once
#include <cstdint>

namespace runtime {
// Stable bounded breadcrumb codes. No user text, identifiers or full game data.
enum class ProgressStage : uint8_t {
    Boot = 0, SettingsLoad, GameRestore, Loop, Console, Controls, Settings,
    Ble, Engine, Sensors, Render, Sleep, Storage, Ota, Idle,
    FactoryBleScan, FactoryBleGatt, FactoryBleClientRelease,
    FactoryBleTaskRelease,
};
struct IncidentContext {
    uint64_t expectedOccupancy;
    uint64_t observedOccupancy;
    uint32_t uptimeMs;
    uint16_t healthCauses;
    uint8_t interactionPhase;
    uint8_t variant;
    uint8_t storageError;
    uint8_t flags; // bit 0 connected, 1 OTA, 2 settings disabled, 3 safe mode
};
struct ProgressRecord {
    IncidentContext context;
    uint32_t stageAtMs[4];
    uint8_t stages[4];
    uint8_t recentStages[8];
    uint8_t nextStage;
    uint8_t recentStageCount;
};
struct RetainedProgress {
    uint32_t magic;
    uint32_t checksum;
    uint32_t bootCount;
    uint32_t watchdogRestarts;
    uint32_t resetReason;
    ProgressRecord previous;
    ProgressRecord current;
};
struct ProgressSnapshot {
    uint32_t bootCount = 0;
    uint32_t watchdogRestarts = 0;
    uint32_t resetReason = 0;
    bool previousValid = false;
    ProgressRecord previous{};
    ProgressRecord current{};
};
constexpr uint32_t WATCHDOG_TIMEOUT_SECONDS = 45U;
bool IsWatchdogReset(uint32_t reason);
bool ValidateRetainedProgress(const RetainedProgress& state);
void BeginProgressBoot(RetainedProgress& state, uint32_t resetReason);
void RecordProgress(RetainedProgress& state, uint8_t lane, ProgressStage stage,
                    uint32_t nowMs);
void RecordIncidentContext(RetainedProgress& state, const IncidentContext& context);
ProgressSnapshot SnapshotProgress(const RetainedProgress& state);

// SDK boundary: boot/loop plus active engine/storage operations subscribe to
// the hardware task watchdog. Waiting for user/BLE input never subscribes an
// idle worker. Only completed work or actual engine search progress feeds it.
void StartProgressWatchdog();
void WatchCurrentTask(uint8_t lane, ProgressStage stage);
void UnwatchCurrentTask(uint8_t lane);
void FeedProgressWatchdog();
void DelayDiagnosticSlice(uint32_t milliseconds);

// Existing blocking diagnostics may run longer than the watchdog timeout.
// The work must return before feeding. Split only the caller's deliberate
// pause; never poll or feed while waiting for the operation itself to finish.
// Callback parameters expose this same ordering to native regression tests.
template <typename Work>
void RunDiagnosticIteration(
    Work work,
    uint32_t pauseMs,
    void (*pause)(uint32_t) = DelayDiagnosticSlice,
    void (*feed)() = FeedProgressWatchdog) {
    work();
    feed();
    constexpr uint32_t MAX_PAUSE_SLICE_MS = 1000U;
    do {
        const uint32_t sliceMs =
            pauseMs > MAX_PAUSE_SLICE_MS ? MAX_PAUSE_SLICE_MS : pauseMs;
        // Even delay=0 yields a scheduler tick so idle-task supervision and
        // BLE remain live during sustained diagnostic throughput work.
        pause(sliceMs == 0U ? 1U : sliceMs);
        feed();
        pauseMs -= sliceMs;
    } while (pauseMs > 0U);
}
void NoteProgress(uint8_t lane, ProgressStage stage);
void UpdateIncidentContext(const IncidentContext& context);
ProgressSnapshot GetProgressSnapshot();
}
