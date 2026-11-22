#pragma once

#include <array>
#include <cstddef>

#include "firmware/recovery_policy.hpp"
#include "runtime/boot_guard.hpp"

namespace firmware {

const char* RecoveryCauseToken(RecoveryCause cause);
const char* RecoveryStateToken(RecoveryState state);
const char* RecoveryOwnerToken(RecoveryOwner owner);
const char* RecoveryActionToken(RecoveryAction action);
const char* RecoveryActionFailureToken(RecoveryActionFailure failure);

struct RecoveryStatusLines {
    std::array<char, 64U> safemodeSummary{};
    std::array<char, 96U> bootGuard{};
    std::array<char, 160U> recovery{};
};

RecoveryStatusLines ComposeRecoveryStatusLines(
    const runtime::BootGuard::Snapshot& bootGuard,
    bool systemSafeModeActive,
    const RecoverySnapshot& recovery);

} // namespace firmware
