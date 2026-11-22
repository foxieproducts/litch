#include "runtime/progress_watchdog.hpp"
#include <cstring>

namespace runtime {
namespace {
constexpr uint32_t MAGIC = 0x4C575431U; // LWT1
uint32_t Checksum(const RetainedProgress& state) {
    // Explicit bytes include initialized padding; BeginProgressBoot zeros the
    // complete record before first use. Magic/checksum are excluded.
    const auto* bytes = reinterpret_cast<const unsigned char*>(&state);
    uint32_t hash = 2166136261U;
    for (size_t i = 2U * sizeof(uint32_t); i < sizeof(state); ++i) {
        hash = (hash ^ bytes[i]) * 16777619U;
    }
    return hash;
}
void Seal(RetainedProgress& state) { state.checksum = Checksum(state); }
}
bool IsWatchdogReset(const uint32_t reason) {
    // ESP-IDF reset reason ABI: interrupt/task/other watchdog.
    return reason >= 5U && reason <= 7U;
}
bool ValidateRetainedProgress(const RetainedProgress& state) {
    return state.magic == MAGIC && state.checksum == Checksum(state);
}
void BeginProgressBoot(RetainedProgress& state, const uint32_t resetReason) {
    const bool retain = ValidateRetainedProgress(state) &&
        resetReason != 1U && resetReason != 2U && resetReason != 9U;
    if (!retain) {
        std::memset(&state, 0, sizeof(state));
        state.magic = MAGIC;
    }
    state.previous = state.current;
    state.current = ProgressRecord{};
    state.current.stages[1] = static_cast<uint8_t>(ProgressStage::Idle);
    state.current.stages[2] = static_cast<uint8_t>(ProgressStage::Idle);
    state.current.stages[3] = static_cast<uint8_t>(ProgressStage::Idle);
    ++state.bootCount;
    if (IsWatchdogReset(resetReason)) { ++state.watchdogRestarts; }
    state.resetReason = resetReason;
    Seal(state);
}
void RecordProgress(RetainedProgress& state, const uint8_t lane,
                    const ProgressStage stage, const uint32_t nowMs) {
    if (lane >= 4U) { return; }
    auto& current = state.current;
    const auto code = static_cast<uint8_t>(stage);
    if (current.stages[lane] != code) {
        current.recentStages[current.nextStage++ % 8U] = code;
        if (current.recentStageCount < 8U) ++current.recentStageCount;
    }
    current.stages[lane] = code;
    current.stageAtMs[lane] = nowMs;
    Seal(state);
}
void RecordIncidentContext(RetainedProgress& state, const IncidentContext& context) {
    state.current.context = context;
    Seal(state);
}
ProgressSnapshot SnapshotProgress(const RetainedProgress& state) {
    if (!ValidateRetainedProgress(state)) { return {}; }
    return {state.bootCount, state.watchdogRestarts, state.resetReason,
            state.bootCount > 1U, state.previous, state.current};
}
}
