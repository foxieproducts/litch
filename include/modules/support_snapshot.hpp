#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "modules/support_runtime_stats.hpp"
#include "runtime/progress_watchdog.hpp"
#include "trace/support_action_history.hpp"

namespace modules::support_snapshot {
constexpr uint8_t PRIVATE_OPCODE = 0x44U;
constexpr uint8_t PUBLIC_OPCODE = 0x45U;
constexpr size_t CAPACITY = 500U;
constexpr size_t ACTION_BYTES = 20U;
constexpr size_t HISTORY_HEADER_BYTES = 80U;
constexpr size_t PUBLIC_BYTES = 106U;
static_assert(HISTORY_HEADER_BYTES + trace::SupportActionHistory::CAPACITY * ACTION_BYTES <= CAPACITY);

struct PublicHealth {
    uint8_t validity = 0U; // health=1, hardware=2, uptime=4, storage=8
    uint16_t healthRevision = 0U;
    uint8_t healthState = 0U;
    uint16_t healthCauses = 0U;
    uint8_t resetReason = 0U;
    uint8_t storageError = 0U;
    uint8_t bootLoadError = 0U;
    uint64_t uptimeMs = 0U;
    uint8_t hardwareMajor = 0U;
    uint8_t hardwareMinor = 0U;
    uint8_t digestAvailability = 0U;
    std::array<uint8_t, 32U> digest{};
    const char* releaseLabel = nullptr;
    uint8_t recoveryMode = 0U;
    uint8_t recoveryCause = 0U;
    uint8_t recoveryState = 0U;
    uint8_t recoveryFailure = 0U;
    uint32_t boots = 0U;
    uint32_t earlyCrashes = 0U;
    uint8_t updater = 0U; // 0 unknown, 1 unavailable, 2 ready, 3 active
    uint8_t storageAccess = 0U; // 0 not attempted, 1 suppressed in recovery, 2 attempted
    uint8_t priorStage = 0U;
    uint32_t priorStageAtMs = 0U;
    bool priorStageValid = false;
};
size_t EncodePublic(uint8_t* out, size_t capacity, uint32_t request,
                    const PublicHealth& health);
size_t EncodeHistory(uint8_t* out, size_t capacity, uint32_t request,
    const SupportRuntimeSnapshot& stats, uint32_t saveTotal,
    uint32_t savesThisBoot, uint32_t errorsThisBoot,
    const trace::SupportActionHistory& actions);
size_t EncodeIncident(uint8_t* out, size_t capacity, uint32_t request,
    const runtime::ProgressSnapshot& progress, uint32_t savesThisBoot,
    uint32_t errorsThisBoot, uint8_t bootError);
} // namespace modules::support_snapshot
