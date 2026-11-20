#include "firmware/periodic_task_cadence.hpp"

namespace firmware_internal {

void PeriodicTaskCadence::EnsureInitialized() {
    if (!initialized) {
        sinceLast.Reset();
        initialized = true;
    }
}

bool PeriodicTaskCadence::IsDue() const {
    if (!initialized) {
        return true;
    }

    return sinceLast.HasElapsed(periodMs);
}

void PeriodicTaskCadence::RescheduleFromNow() {
    sinceLast.Reset();
}

uint32_t PeriodicTaskCadence::RemainingMs() const {
    if (!initialized) {
        return 0;
    }

    return sinceLast.RemainingMs(periodMs);
}

} // namespace firmware_internal

#ifdef ARDUINO

#include "firmware/firmware_internal.hpp"
#include "modules/takeback_grace_setting.hpp"

namespace firmware_internal {

bool GetFlipSidesSetting(const fxe::SettingsStore& settings) {
    const auto& doc = settings.Doc();
    if (doc.containsKey("BOARD_ROTATE")) {
        return settings.ReadValue("BOARD_ROTATE", static_cast<uint32_t>(0U)) != 0U;
    }
    return false;
}

bool GetFirstPlayerIsP2Setting(const fxe::SettingsStore& settings) {
    return settings.ReadValue("GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U;
}

bool GetVsCpuPlayerIsP2Setting(const fxe::SettingsStore& settings) {
    return settings.ReadValue("VS_CPU_PLAYER", static_cast<uint32_t>(2U)) == 2U;
}

FirmwareSystem& GetSystem() {
    static FirmwareSystem sys;
    return sys;
}

runtime::SystemContext& GetSystemContext() {
    auto& sys = GetSystem();

    if (!sys.ctx.settings) {
        sys.ctx.settings = &sys.settings;
        sys.ctx.telemetry = &sys.telemetry;
    }

    return sys.ctx;
}

#ifdef LITCH_TRACE
trace::TraceRecorder& GetTraceRecorder() {
    return GetSystem().recorder;
}

trace::TraceSettingsSnapshot GetTraceSettingsSnapshot(const fxe::SettingsStore& settings) {
    trace::TraceSettingsSnapshot s;

    const auto& doc = settings.Doc();

    s.brightness = settings.ReadValue("BRIGHTNESS", s.brightness);

    if (doc.containsKey("BOARD_ROTATE")) {
        s.gameFlipSides = settings.ReadValue("BOARD_ROTATE", s.gameFlipSides);
    }

    const uint32_t firstPlayer = settings.ReadValue("GAME_FIRST_PLAYER", static_cast<uint32_t>(1U));
    s.gameFirstPlayer = (firstPlayer == 2U) ? 2U : 1U;

    if (doc.containsKey("VS_CPU_LEVEL")) {
        s.vsCpuLevel = settings.ReadValue("VS_CPU_LEVEL", s.vsCpuLevel);
    }

    s.vsCpuPlayer = settings.ReadValue("VS_CPU_PLAYER", s.vsCpuPlayer);

    s.timer = settings.ReadValue("TIMER", s.timer);
    s.takebackGraceSeconds =
        modules::takeback_grace_setting::ReadSeconds(settings);

    return s;
}
#endif

} // namespace firmware_internal

#endif
