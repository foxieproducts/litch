#ifdef ARDUINO
#include "firmware/compliance_test.hpp"
#include "firmware/firmware_internal.hpp"
#include "console.hpp"
#include "foxie_embedded/millis.hpp"
#include <Arduino.h>

namespace firmware::compliance {
namespace {
const char* ColorName(const Color color) {
    switch (color) {
        case Color::White: return "white";
        case Color::Red: return "red";
        case Color::Green: return "green";
        case Color::Blue: return "blue";
        case Color::Rgb: return "rgb";
    }
    return "unknown";
}
constexpr const char* USAGE =
    "comptest: start|core|rail|led|ble|combined|status|stop; rail on|off; level 0..5; pattern static|animated; color white|red|green|blue|rgb; budget detected|500|1500|3000; ble off|advertise|traffic";
}

bool Session::Begin(fxe::Console& console, const Load& load) {
    auto& sys = firmware_internal::GetSystem();
    if (sys.hardwareTest.IsActive() || sys.amptest.IsActive() || sys.amptestVerify.IsActive() ||
        sys.ble.IsOtaInProgress() || sys.rebootPersistencePending ||
        sys.sleep.GetState() != firmware::SleepState::Awake ||
        (!sys.factoryBootOnly && (sys.gamePersistence.Litchcraft().IsActive() ||
            sys.gamePersistence.IsEngineLaneActive() ||
            sys.gamePersistence.GetGameClock().IsRunning()))) {
        console.PrintLn("comptest: finish the active game, test, update or save before starting"); return false;
    }
    sys.ble.SetFactoryVerifierRuntimeAllowed(false);
    if (!sys.ble.QuiesceFactoryBleDut()) {
        console.PrintLn("comptest: factory radio cleanup pending; retry start"); return false;
    }
    const bool savedRail = sys.powerRail.Is5vEnabled();
    const bool savedAmbient = sys.ambientLights.IsPoweredOn();
    if (!Apply(load)) {
        console.PrintLn("comptest: LED clear incomplete; load unchanged; retry");
        return false;
    }
    m_savedRail = savedRail;
    m_savedAmbient = savedAmbient;
    m_startedAtMs = fxe::Millis(); m_lastTrafficMs = m_startedAtMs;
    m_trafficPublished = 0U; m_trafficSkipped = 0U;
    m_rxBaseline = sys.ble.SessionIngressCount(); m_txBaseline = sys.ble.SessionNotifyCount();
    if (!sys.factoryBootOnly) sys.gamePersistence.RequestCancelAllThinking();
    sys.quickSettingsOverlayActive = false;
    sys.quickSettingsOverlayOwner = modules::UiOwner::None;
    sys.quickSettingsOverlayController.Reset();
    sys.resetHoldController.Reset(); sys.hintButtonController.Reset();
    if (!sys.factoryBootOnly) sys.gamePersistence.SetBoardSettingsActive(false);
    sys.menuOverlay.Stop();
    sys.gameResetHoldActive = false; sys.gameResetHoldTriggered = false;
    m_active = true;
    console.PrintLn("comptest: temporary governed load; USB Console remains available; stop restores runtime; 3.3V-domain parts remain supplied (V3.3 Hall included)");
    return true;
}

bool Session::Command(fxe::Console& console, const std::string_view action,
                      const std::string_view value) {
    if ((action == "status" || action == "stop") && value.empty()) {
        if (action == "stop" && !Stop()) {
            console.PrintLn("comptest: LED clear incomplete; test remains active; retry stop");
            return false;
        }
        PrintStatus(console); return true;
    }
    Load next = m_load;
    if (!ParseLoad(action, value, next)) { console.PrintLn("%s", USAGE); return false; }
    if (!m_active && !value.empty()) {
        console.PrintLn("comptest: start a preset first"); return false;
    }
    if (!m_active) {
        if (!Begin(console, next)) return false;
    } else if (!Apply(next)) {
        console.PrintLn("comptest: LED clear incomplete; load unchanged; retry");
        return false;
    }
    m_load = next; Render(); PrintStatus(console); return true;
}

bool Session::Apply(const Load& load) {
    auto& sys = firmware_internal::GetSystem();
    if (sys.powerRail.Is5vEnabled() != load.rail) {
        if (load.rail) { if (!firmware_internal::EnableLedRail(sys)) return false; }
        else if (!firmware_internal::QuiesceLedRail(sys)) return false;
    }
    sys.ambientLights.ShutDown();
    sys.ble.SetDiagnosticRadioOverride(true, load.ble != BleLoad::Off);
    sys.ble.Update(sys.settings);
    // Existing factory owner quiesces election/peer work before taking the
    // temporary BLE load.
    sys.factoryRadio.Update(false, false, modules::ManufacturingTestStage::Inactive,
                            sys.ble, fxe::Millis());
    return true;
}

void Session::Update() {
    if (!m_active) return;
    auto& sys = firmware_internal::GetSystem();
    if (sys.ble.IsOtaInProgress()) { Stop(); return; }
    const uint32_t now = fxe::Millis();
    if (m_load.ble == BleLoad::Traffic && now - m_lastTrafficMs >= 50U) {
        m_lastTrafficMs = now;
        if (sys.ble.RefreshDiagnosticPhysicalInputState()) ++m_trafficPublished;
        else ++m_trafficSkipped;
    }
}

void Session::Render() {
    if (!m_active) return;
    auto& sys = firmware_internal::GetSystem();
    // A disabled rail is silent: never clock an unpowered LED chain.
    if (!m_load.rail) return;
    const auto frame = ComposeLoad(m_load, sys.runtimeHardware.resolved.revision,
                                  fxe::Millis() - m_startedAtMs);
    firmware_internal::RenderPhysicalFrameWithPowerBudget(sys, frame, 256U);
}

bool Session::Stop() {
    if (!m_active) return true;
    auto& sys = firmware_internal::GetSystem();
    if (!m_savedRail && !firmware_internal::QuiesceLedRail(sys)) return false;
    m_rxCount = sys.ble.SessionIngressCount() - m_rxBaseline;
    m_txCount = sys.ble.SessionNotifyCount() - m_txBaseline;
    if (sys.powerRail.Is5vEnabled()) { firmware_internal::ClearLedOutput(sys); }
    if (sys.powerRail.Is5vEnabled() != m_savedRail) {
        if (m_savedRail) { if (!firmware_internal::EnableLedRail(sys)) return false; }
    }
    if (m_savedAmbient) sys.ambientLights.PowerOn();
    sys.ble.SetDiagnosticRadioOverride(false, false);
    sys.ble.Update(sys.settings);
    sys.gpioUserInput.SuppressHeldControlsUntilRelease();
    sys.ble.ResetConnectionButtonState();
    sys.buttonInputQueue.Clear();
    m_active = false;
    return true;
}

void Session::PrintStatus(fxe::Console& console) const {
    const auto& sys = firmware_internal::GetSystem();
    const auto detected = sys.usbCcSampleValid ? sys.lastUsbCcAdvertisedLevel : hardware::UsbCcLevel::Unknown;
    console.PrintLn("comptest: active=%u 5v_en=%u level=%u/5 pattern=%s color=%s detected_usb=%s source_ceiling_ma=%u requested_ceiling_ma=%u runtime_total_limit_ma=%u led_limit_ma=%u requested_led_ma=%u applied_led_ma=%u",
        m_active, sys.powerRail.Is5vEnabled(), m_load.level, m_load.animated ? "animated" : "static",
        ColorName(m_load.color), hardware::UsbCcLevelToString(detected), SourceCeilingMa(detected),
        m_load.ceilingMa, firmware_internal::GetActiveTotalCurrentLimitMa(sys), sys.ledPowerLimitMa,
        sys.ledPowerStats.estimatedLedCurrentMa, sys.powerRail.Is5vEnabled() ? sys.ledPowerStats.appliedLedCurrentMa : 0U);
    console.PrintLn("comptest: ble_enabled=%u advertising=%u connected=%u shared_session=%u traffic_requested=%u traffic_published=%lu traffic_skipped=%lu session_notify_accepted=%lu session_ingress=%lu",
        sys.ble.IsEnabled(), sys.ble.IsAdvertising(), sys.ble.IsConnected(), sys.ble.IsConnectionSessionActive(),
        m_load.ble == BleLoad::Traffic, static_cast<unsigned long>(m_trafficPublished), static_cast<unsigned long>(m_trafficSkipped),
        static_cast<unsigned long>(m_active ? sys.ble.SessionNotifyCount()-m_txBaseline : m_txCount), static_cast<unsigned long>(m_active ? sys.ble.SessionIngressCount()-m_rxBaseline : m_rxCount));
}
} // namespace firmware::compliance
#endif
