#pragma once
#include <array>
#include "foxie_embedded/fixed_string.hpp"
#include "modules/home_state_notification.hpp"
#include "runtime/runtime_health.hpp"
#include "runtime/progress_watchdog.hpp"
namespace fxe { class Console; }
namespace modules {
using BoardHealthLine = fxe::FixedString<192>;
struct BoardHealthConnectionState final {
    std::array<uint8_t, 6U> runtime{};
    std::array<uint8_t, 10U> context{};
};
using SupportIncidentLine = fxe::FixedString<1024>;
BoardHealthLine FormatBoardHealth(const runtime::RuntimeHealthSnapshot& health,
    PersistenceDegradation persistence, const runtime::ProgressSnapshot& progress, bool sessionOnly = false);
BoardHealthConnectionState EncodeBoardHealthConnectionState(
    const runtime::RuntimeHealthSnapshot& health,
    PersistenceDegradation persistence,
    const runtime::ProgressSnapshot& progress,
    bool sessionOnly = false);
SupportIncidentLine FormatSupportIncident(const runtime::ProgressSnapshot& progress,
    uint32_t saveCount, uint32_t failureCount, uint8_t bootStorageError);
void WriteSupportIncident(fxe::Console& console, const SupportIncidentLine& line);
}
