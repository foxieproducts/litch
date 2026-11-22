#ifdef ARDUINO

#include <Arduino.h>

#include "firmware/firmware_console_internal.hpp"

#include "firmware/firmware_internal.hpp"
#include "firmware/power_runtime_health.hpp"

#include "foxie_embedded/millis.hpp"

#include "hardware/gpio_diagnostics.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/orientation.hpp"
#include "runtime/square_mapping.hpp"
#include "runtime/progress_watchdog.hpp"

namespace firmware_internal::console_commands {

#if defined(CONFIG_IDF_TARGET_ESP32S3)
namespace {

bool InputCaptureCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();
    if (xPortGetCoreID() != 1 || sys.runtimeHardware.resolved.revision != hardware::BoardRevision::V3_3S3)
        return false;
    auto& input = sys.gpioUserInput;
    const auto mode = args.Has(2) ? args.Get(2) : std::string_view("status");
    if (args.argc > 3 || (mode != "status" && mode != "poll" && mode != "irq")) return false;
    const bool ok = mode == "status" || (input.AreCachedControlsReleased() &&
        input.SetButtonInterruptsEnabled(mode == "irq"));
    const auto status = input.GetButtonInterruptStatus();
    console.PrintLn("input capture: mode=%s raw=%lu pending=%u down=0x%03x overflows=%lu last_ms=%lu",
        status.enabled ? "irq" : "poll", static_cast<unsigned long>(status.rawCount),
        status.pending, status.downMask, static_cast<unsigned long>(status.overflows),
        static_cast<unsigned long>(status.lastEdgeAtMs));
    return ok;
}
} // namespace
#endif

bool InputCommand(Console& console, const Console::Args& args) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (args.Has(1) && args.Equals(1, "capture")) return InputCaptureCommand(console, args);
#endif
    if (args.argc < 2 || args.Get(1) != std::string_view("scan")) {
        console.PrintLn("Usage: input scan");
        return false;
    }

    struct PinInfo {
        uint8_t pin;
        const char* name;
    };

    console.PrintLn("Raw GPIO levels (1=HIGH, 0=LOW):");

    if (hardware::HW_IS_S3) {
        const hardware::BoardHardwareProfile& profile =
            GetSystem().runtimeHardware.resolved;
        const PinInfo pins[] = {
            {profile.p1.x, "P1X"},
            {profile.p1.o, "P1O"},
            {profile.p2.x, "P2X"},
            {profile.p2.o, "P2O"},
            {profile.p1.up, "P1_DPAD_UP"},
            {profile.p1.left, "P1_DPAD_LEFT"},
            {profile.p1.down, "P1_DPAD_DOWN"},
            {profile.p1.right, "P1_DPAD_RIGHT"},
            {profile.p2.up, "P2_DPAD_UP"},
            {profile.p2.left, "P2_DPAD_LEFT"},
            {profile.p2.down, "P2_DPAD_DOWN"},
            {profile.p2.right, "P2_DPAD_RIGHT"},
        };

        for (const auto& pin : pins) {
            console.PrintLn("  %s: %d", pin.name, hardware::ReadDigitalLevel(pin.pin));
        }
    } else {
        static constexpr PinInfo PINS[] = {
            {0, "A3/GPIO0"},
            {1, "A2/GPIO1"},
            {4, "A0/GPIO4"},
            {7, "MOSI/GPIO7"},
            {8, "MISO/GPIO8"},
            {9, "BOOT/GPIO9"},
            {10, "SCK/GPIO10"},
            {20, "RX/GPIO20"},
            {21, "TX/GPIO21"},
        };

        for (const auto& pin : PINS) {
            console.PrintLn("  %s: %d", pin.name, hardware::ReadDigitalLevel(pin.pin));
        }
    }

    console.PrintLn("Tip: press/hold a button, re-run 'input scan', and look for the pin that flips.");
    return true;
}

bool LedPowerCommand(Console& console, const Console::Args& args) {
    const std::string_view sub = args.Has(1) ? args.Get(1) : std::string_view("status");
    if (!(sub == "status" || sub == "clear" || sub == "latch")) {
        console.PrintLn("Usage: ledpower [status|clear|latch]");
        return false;
    }

    auto& sys = GetSystem();
    if (sys.comptest.IsActive() && sub != "status") {
        console.PrintLn("ledpower: stop comptest before changing protection"); return false;
    }
    if (sub == "clear") {
        if (!ClearLedPowerProtectionLatch(sys, fxe::Millis())) {
            console.PrintLn(
                "ledpower: protection remains latched; fresh S3 CC sample unavailable");
            return false;
        }
        console.PrintLn("ledpower: protection cleared; normal power budget retry enabled");
    } else if (sub == "latch") {
        ResetUsbCcSourceAuthorization(sys);
        (void)SetLedBrownoutLimitLatched(sys.settings, true);
        runtime::RuntimeHealthFact fact;
        if (firmware::MakeBrownoutRuntimeHealthFact(
                IsLedBrownoutLimitLatched(sys.settings),
                fxe::Millis(),
                fact)) {
            (void)ApplyRuntimeHealthFactAndForward(sys, fact);
        }
    }

    if (sub != "clear") {
        RefreshLedPowerSourceStatus(sys, fxe::Millis(), true);
    }
    sys.ledPowerLimitMa = GetActiveLedCurrentLimitMa(sys);
    const uint16_t fixedSystemMa = GetFixedSystemCurrentMa();
    const uint16_t totalLimitMa = GetActiveTotalCurrentLimitMa(sys);
    const uint32_t predictedTotalMa =
        static_cast<uint32_t>(fixedSystemMa) +
        sys.ledPowerStats.appliedLedCurrentMa;

    console.PrintLn("ledpower: brownout_latched=%u led_limit_ma=%u total_limit_ma=%u baseline_ma=%u",
                    static_cast<unsigned>(IsLedBrownoutLimitLatched(sys.settings) ? 1U : 0U),
                    static_cast<unsigned>(sys.ledPowerLimitMa),
                    static_cast<unsigned>(totalLimitMa),
                    static_cast<unsigned>(fixedSystemMa));
    console.PrintLn("ledpower: last_modeled_requested_ma=%u "
                    "last_modeled_applied_ma=%u last_modeled_total_ma=%lu "
                    "last_scale_pct=%lu",
                    static_cast<unsigned>(sys.ledPowerStats.estimatedLedCurrentMa),
                    static_cast<unsigned>(sys.ledPowerStats.appliedLedCurrentMa),
                    static_cast<unsigned long>(predictedTotalMa),
                    static_cast<unsigned long>(
                        static_cast<uint32_t>(sys.ledPowerStats.appliedBrightnessScale01 * 100.0f + 0.5f)));

    if (hardware::HW_IS_S3) {
        console.PrintLn("ledpower: protection_source_budget_ma=%u source_margin_ma=%u",
                        static_cast<unsigned>(modules::S3_PROTECTION_SOURCE_BUDGET_MA),
                        static_cast<unsigned>(modules::S3_PROTECTION_SOURCE_MARGIN_MA));
        const hardware::UsbCcThresholds thresholds = LoadUsbCcThresholds(sys.settings);
        const bool entryCandidate =
            hardware::UsbCcReadingsMeetStrongConfirmationThreshold(
                sys.lastUsbCc1, sys.lastUsbCc2, thresholds);
        console.PrintLn("ledpower: cc_raw1=%u cc_raw2=%u advertised=%s "
                        "strong_samples=%u/%u confirmed=%u",
                        static_cast<unsigned>(sys.lastUsbCc1),
                        static_cast<unsigned>(sys.lastUsbCc2),
                        hardware::UsbCcLevelToString(sys.lastUsbCcAdvertisedLevel),
                        static_cast<unsigned>(
                            sys.usbCcSourcePolicy.confirmation.samples),
                        static_cast<unsigned>(modules::S3_STRONG_SOURCE_CONFIRMATION_SAMPLES),
                        static_cast<unsigned>(modules::IsStrongSourceConfirmed(
                                                  sys.usbCcSourcePolicy.confirmation)
                                                  ? 1U
                                                  : 0U));
        console.PrintLn("ledpower: cc_active_pin=%s cc_monitor_raw=%u "
                        "downgrade_pending=%u pair_recheck_pending=%u "
                        "monitor_interval_ms=%u debounce_ms=%u",
                        hardware::UsbCcActivePinToString(
                            sys.usbCcSourcePolicy.activePin),
                        static_cast<unsigned>(sys.lastUsbCcMonitorRaw),
                        static_cast<unsigned>(
                            sys.usbCcSourcePolicy.monitor
                                    .downgradeCandidateActive
                                ? 1U
                                : 0U),
                        static_cast<unsigned>(
                            sys.usbCcSourcePolicy.fullPairRecheckPending
                                ? 1U
                                : 0U),
                        static_cast<unsigned>(modules::S3_CC_MONITOR_INTERVAL_MS),
                        static_cast<unsigned>(modules::S3_CC_CHANGE_DEBOUNCE_MS));
        console.PrintLn("ledpower: cc_thresh_500=%u cc_thresh_1500=%u "
                        "cc_thresh_3000=%u strong_enter_raw=%u entry_candidate=%u",
                        static_cast<unsigned>(thresholds.level500),
                        static_cast<unsigned>(thresholds.level1500),
                        static_cast<unsigned>(thresholds.level3000),
                        static_cast<unsigned>(
                            hardware::GetUsbCcStrongConfirmationRawThreshold(
                                thresholds)),
                        static_cast<unsigned>(entryCandidate ? 1U : 0U));
    }

    return true;
}

size_t BoardComplete(const Console::Args& args,
                     const size_t argIndex,
                     const char* prefix,
                     const char** outMatches,
                     const size_t outMatchesCap) {
    size_t matchCount = 0;

    if (argIndex == 1) {
        matchCount = AddMatchIfPrefix("status", prefix, outMatches, matchCount, outMatchesCap);
        matchCount = AddMatchIfPrefix("resync", prefix, outMatches, matchCount, outMatchesCap);
        matchCount = AddMatchIfPrefix("stress", prefix, outMatches, matchCount, outMatchesCap);
        matchCount = AddMatchIfPrefix("hallmap", prefix, outMatches, matchCount, outMatchesCap);
        return matchCount;
    }

    if (argIndex == 2 && prefix != nullptr) {
        matchCount = AddMatchIfPrefix("start", prefix, outMatches, matchCount, outMatchesCap);
        matchCount = AddMatchIfPrefix("stop", prefix, outMatches, matchCount, outMatchesCap);
        matchCount = AddMatchIfPrefix("status", prefix, outMatches, matchCount, outMatchesCap);
        matchCount = AddMatchIfPrefix("reset", prefix, outMatches, matchCount, outMatchesCap);
        return args.Equals(1, "hallmap") ? matchCount : 0;
    }

    if (argIndex == 4 && args.Equals(1, "stress")) {
        matchCount = AddMatchIfPrefix("0", prefix, outMatches, matchCount, outMatchesCap);
        matchCount = AddMatchIfPrefix("1", prefix, outMatches, matchCount, outMatchesCap);
        return matchCount;
    }

    return 0;
}

bool BoardCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();
    const std::string_view sub = args.Has(1) ? args.Get(1) : std::string_view();

    if (sub.empty() || sub == "status") {
        const bool rotateEnabled = GetFlipSidesSetting(sys.settings);
        const bool firstPlayerIsP2 = GetFirstPlayerIsP2Setting(sys.settings);
        const unsigned firstPlayerValue = firstPlayerIsP2 ? 2U : 1U;
        const char* p1PhysicalEnd = firstPlayerIsP2 ? "P2" : "P1";
        const bool squareRotate = modules::ComputeSquareRotate(
            rotateEnabled, firstPlayerIsP2, sys.gamePersistence.Litchcraft().IsActive());
        const bool placementMode = sys.stateMachine.IsPlacementModeActive();

        uint64_t rawOcc64 = 0;
        const bool rawOk = sys.boardInput.TryReadSnapshot(rawOcc64);

        uint64_t mappedOcc64 = 0;
        for (uint8_t rawSquare = 0; rawSquare < 64; ++rawSquare) {
            if ((rawOcc64 & (1ULL << rawSquare)) == 0) {
                continue;
            }
            const uint8_t logicalSquare = runtime::MapSquareIndex(rawSquare, squareRotate);
            mappedOcc64 |= (1ULL << logicalSquare);
        }

        console.PrintLn("rotate=%u firstplayer=%u (white_owner=P1 square_rotate=%u) placement_mode=%u i2c_errors=%lu p1_physical_end=%s",
                        static_cast<unsigned>(rotateEnabled ? 1U : 0U),
                        firstPlayerValue,
                        static_cast<unsigned>(squareRotate ? 1U : 0U),
                        static_cast<unsigned>(placementMode ? 1U : 0U),
                        static_cast<unsigned long>(sys.boardInput.GetI2cErrorCount()),
                        p1PhysicalEnd);
        console.PrintLn("raw_snapshot_ok=%u raw_occ64=0x%016llx",
                        static_cast<unsigned>(rawOk ? 1U : 0U),
                        static_cast<unsigned long long>(rawOcc64));
        console.PrintLn("mapped_occ64=0x%016llx", static_cast<unsigned long long>(mappedOcc64));

        const auto sideChar = [](const modules::LitchMaxEngine::PieceSide side) -> char {
            switch (side) {
                case modules::LitchMaxEngine::PieceSide::White:
                    return 'W';
                case modules::LitchMaxEngine::PieceSide::Black:
                    return 'B';
                default:
                    return '-';
            }
        };
        if (sys.gamePersistence.IsEngineLaneActive()) {
            console.PrintLn("engine_sides: unavailable (worker active)");
            return rawOk;
        }
        console.PrintLn("engine_sides: a8=%c e8=%c e1=%c a1=%c",
                        sideChar(sys.engine.GetPieceSideAtSquare(0)),
                        sideChar(sys.engine.GetPieceSideAtSquare(4)),
                        sideChar(sys.engine.GetPieceSideAtSquare(60)),
                        sideChar(sys.engine.GetPieceSideAtSquare(56)));
        return rawOk;
    }

    if (sub == "resync") {
        sys.board.Resync();
        console.PrintLn("OK");
        return true;
    }

    if (sub == "stress") {
        uint16_t durationSeconds = 10;
        if (args.Has(2) && !TryParseU16(args.Get(2), durationSeconds)) {
            console.PrintLn("usage: board stress [seconds] [delay_ms] [ambient_0_or_1]");
            return false;
        }

        uint16_t delayMs = 2;
        if (args.Has(3) && !TryParseU16(args.Get(3), delayMs)) {
            console.PrintLn("usage: board stress [seconds] [delay_ms] [ambient_0_or_1]");
            return false;
        }

        uint16_t ambientFlag = hardware::HW_HAS_LIGHT_SENSORS ? 1U : 0U;
        if (args.Has(4) && !TryParseU16(args.Get(4), ambientFlag)) {
            console.PrintLn("usage: board stress [seconds] [delay_ms] [ambient_0_or_1]");
            return false;
        }

        if (durationSeconds == 0) {
            durationSeconds = 1;
        }

        const bool includeAmbient = hardware::HW_HAS_LIGHT_SENSORS && (ambientFlag != 0U);
        const uint32_t durationMs = static_cast<uint32_t>(durationSeconds) * 1000U;
        const uint32_t startI2cErrors = sys.boardInput.GetI2cErrorCount();
        const uint32_t startMs = fxe::Millis();
        uint32_t nextProgressMs = startMs + 1000U;

        uint32_t iterations = 0;
        uint32_t snapshotOkCount = 0;
        uint32_t snapshotFailCount = 0;
        uint32_t occChangeCount = 0;
        uint32_t ambientSampleCount = 0;

        bool baselineValid = false;
        uint64_t baselineOcc64 = 0;
        uint64_t previousOcc64 = 0;

        console.PrintLn("board stress: seconds=%u delay_ms=%u ambient=%u",
                        static_cast<unsigned>(durationSeconds),
                        static_cast<unsigned>(delayMs),
                        static_cast<unsigned>(includeAmbient ? 1U : 0U));

        // This diagnostic owns the main loop for seconds. Shed any
        // above-default LED load before normal CC monitoring pauses.
        ResetUsbCcSourceAuthorization(sys);

        while ((fxe::Millis() - startMs) < durationMs) {
            runtime::RunDiagnosticIteration([&]() {
                const uint32_t nowMs = fxe::Millis();
                uint64_t occ64 = 0;
                if (sys.boardInput.TryReadSnapshot(occ64)) {
                    ++snapshotOkCount;

                    if (!baselineValid) {
                        baselineValid = true;
                        baselineOcc64 = occ64;
                        previousOcc64 = occ64;
                    } else if (occ64 != previousOcc64) {
                        ++occChangeCount;
                        previousOcc64 = occ64;
                    }
                } else {
                    ++snapshotFailCount;
                }

                if (includeAmbient && sys.ambientLights.Update()) {
                    ++ambientSampleCount;
                }

                ++iterations;

                if ((nowMs - nextProgressMs) < 0x80000000UL) {
                    const uint32_t elapsedSeconds = (nowMs - startMs) / 1000U;
                    const uint32_t i2cNow = sys.boardInput.GetI2cErrorCount();
                    const uint32_t i2cProgressDelta =
                        (i2cNow >= startI2cErrors) ? (i2cNow - startI2cErrors) : 0U;

                    console.PrintLn("board stress: t=%lus/%us iter=%lu ok=%lu fail=%lu i2c_delta=%lu",
                                    static_cast<unsigned long>(elapsedSeconds),
                                    static_cast<unsigned long>(durationSeconds),
                                    static_cast<unsigned long>(iterations),
                                    static_cast<unsigned long>(snapshotOkCount),
                                    static_cast<unsigned long>(snapshotFailCount),
                                    static_cast<unsigned long>(i2cProgressDelta));

                    while ((nowMs - nextProgressMs) < 0x80000000UL) {
                        nextProgressMs += 1000U;
                    }
                }
            }, delayMs);
        }

        const uint32_t endI2cErrors = sys.boardInput.GetI2cErrorCount();
        const uint32_t i2cDelta = (endI2cErrors >= startI2cErrors) ? (endI2cErrors - startI2cErrors) : 0U;

        uint64_t finalOcc64 = 0;
        const bool finalOk = sys.boardInput.TryReadSnapshot(finalOcc64);
        const uint8_t baselineChanged = (baselineValid && finalOk && (baselineOcc64 != finalOcc64)) ? 1U : 0U;

        console.PrintLn("board stress: iter=%lu ok=%lu fail=%lu occ_changes=%lu ambient_samples=%lu",
                        static_cast<unsigned long>(iterations),
                        static_cast<unsigned long>(snapshotOkCount),
                        static_cast<unsigned long>(snapshotFailCount),
                        static_cast<unsigned long>(occChangeCount),
                        static_cast<unsigned long>(ambientSampleCount));
        console.PrintLn("board stress: i2c_errors +%lu (total=%lu)",
                        static_cast<unsigned long>(i2cDelta),
                        static_cast<unsigned long>(endI2cErrors));
        console.PrintLn("board stress: baseline_valid=%u baseline_changed=%u",
                        static_cast<unsigned>(baselineValid ? 1U : 0U),
                        static_cast<unsigned>(baselineChanged));

        if (i2cDelta == 0U && snapshotFailCount == 0U && baselineChanged == 0U) {
            console.PrintLn("board stress: PASS (no I2C errors, no read failures, stable occupancy)");
            return true;
        }

        console.PrintLn("board stress: WARN (bus instability or occupancy jitter detected)");
        return false;
    }

    if (sub == "hallmap") {
        if (!hardware::HW_HAS_EXPANDER_INTERRUPTS) {
            console.PrintLn("hallmap: unsupported on this hardware");
            return false;
        }

        const std::string_view mode = args.Has(2) ? args.Get(2) : std::string_view();
        if (mode.empty() || mode == "status") {
            console.PrintLn("hallmap: active=%u seq=%lu",
                            static_cast<unsigned>(sys.hallMapActive ? 1U : 0U),
                            static_cast<unsigned long>(sys.hallMapSeq));
            return true;
        }

        if (mode == "start") {
            sys.hallMapActive = true;
            sys.hallMapLastUpdate = sys.inputScan.GetUpdateCounter();
            console.PrintLn("hallmap: started (move magnet across squares)");
            return true;
        }

        if (mode == "stop") {
            sys.hallMapActive = false;
            console.PrintLn("hallmap: stopped");
            return true;
        }

        if (mode == "reset") {
            sys.hallMapSeq = 0;
            sys.hallMapLastUpdate = sys.inputScan.GetUpdateCounter();
            console.PrintLn("hallmap: reset");
            return true;
        }

        console.PrintLn("usage: board hallmap <start|stop|status|reset>");
        return false;
    }

    console.PrintLn("usage: board [status|resync|stress|hallmap]");
    return false;
}

} // namespace firmware_internal::console_commands

#endif
