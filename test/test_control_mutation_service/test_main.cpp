#include <cstdint>
#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

#include "modules/control_mutation_service.hpp"
#include "firmware/sleep_controller.hpp"
#include "modules/volatile_persistence_session.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

struct Harness {
    modules::SettingsStore settings;
    modules::ControlMutationService service;
    modules::ControlCoordinatorResult startGate =
        modules::ControlCoordinatorResult::Applied;
    modules::ControlCoordinatorResult undoStart =
        modules::ControlCoordinatorResult::Pending;
    modules::ControlCoordinatorResult newGameStart =
        modules::ControlCoordinatorResult::Applied;
    modules::ControlCoordinatorResult polled =
        modules::ControlCoordinatorResult::Pending;
    modules::ControlActionState actionState =
        modules::ControlActionState::Idle;
    modules::ControlActionState actionStateAfterUndoRequest =
        modules::ControlActionState::Idle;
    uint32_t beforeCount = 0U;
    uint32_t projectionCount = 0U;
    uint32_t startGateCheckCount = 0U;
    uint32_t cpuCancelCount = 0U;
    uint32_t hintCancelCount = 0U;
    uint32_t undoStartCount = 0U;
    uint32_t newGameStartCount = 0U;
    uint32_t pollCount = 0U;
    uint32_t valueSeenBeforeMutation = 0U;
    bool coachingP1 = false;
    bool coachingP2 = false;

    Harness() {
        modules::ControlMutationCallbacks callbacks;
        callbacks.context = this;
        callbacks.beforeSettingMutation = [](void* context, const char* key) {
            auto& self = *static_cast<Harness*>(context);
            ++self.beforeCount;
            self.valueSeenBeforeMutation =
                self.settings.ReadValue(key, static_cast<uint32_t>(0U));
        };
        callbacks.settingProjected = [](void* context, const char*) {
            ++static_cast<Harness*>(context)->projectionCount;
        };
        callbacks.checkStartOnlyGate = [](void* context, const char*) {
            auto& self = *static_cast<Harness*>(context);
            ++self.startGateCheckCount;
            return self.startGate;
        };
        callbacks.cancelCpuWork = [](void* context) {
            ++static_cast<Harness*>(context)->cpuCancelCount;
        };
        callbacks.cancelHintWorkAndRestart = [](void* context) {
            ++static_cast<Harness*>(context)->hintCancelCount;
        };
        callbacks.requestHint = [](void*, uint16_t) {
            return modules::ControlCoordinatorResult::Applied;
        };
        callbacks.requestUndo = [](void* context, uint16_t, uint8_t) {
            auto& self = *static_cast<Harness*>(context);
            ++self.undoStartCount;
            self.actionState = self.actionStateAfterUndoRequest;
            return self.undoStart;
        };
        callbacks.requestNewGame = [](void* context, uint16_t) {
            auto& self = *static_cast<Harness*>(context);
            ++self.newGameStartCount;
            return self.newGameStart;
        };
        callbacks.pollAction = [](void* context, uint16_t) {
            auto& self = *static_cast<Harness*>(context);
            ++self.pollCount;
            return self.polled;
        };
        callbacks.getActionState = [](void* context) {
            return static_cast<Harness*>(context)->actionState;
        };
        callbacks.coachingChanged = [](void* context,
                                          const modules::UiOwner owner,
                                          const bool enabled) {
            auto& self = *static_cast<Harness*>(context);
            if (owner == modules::UiOwner::P1) {
                self.coachingP1 = enabled;
            } else if (owner == modules::UiOwner::P2) {
                self.coachingP2 = enabled;
            }
        };
        service.Configure(settings, callbacks);
    }

    modules::ControlMutationOutcome Set(const char* key,
                                        const JsonVariantConst value,
                                        const uint16_t requestId = 0U) {
        return service.MutateSetting(key, value, requestId);
    }
};

void TestTypedSleepAction() {
    modules::SettingsStore settings;
    firmware::SleepController sleep;
    modules::ControlMutationCallbacks callbacks;
    callbacks.context = &sleep;
    callbacks.requestSleep = [](void* context) {
        return static_cast<firmware::SleepController*>(context)->RequestSleep(
            firmware::SleepRequestOrigin::App, 100U, false)
            ? modules::ControlCoordinatorResult::Applied
            : modules::ControlCoordinatorResult::Busy;
    };
    modules::ControlMutationService service;
    service.Configure(settings, callbacks);
    const auto malformed = service.MutateAction(modules::ControlAction::Sleep, 1U,
        modules::UiOwner::P1);
    Check(malformed.status == modules::ControlMutationStatus::Rejected &&
              sleep.GetState() == firmware::SleepState::Awake,
          "Sleep rejects noncanonical API fields before domain mutation");
    const auto accepted = service.MutateAction(modules::ControlAction::Sleep, 2U);
    Check(accepted.status == modules::ControlMutationStatus::Applied &&
              accepted.reason == modules::ControlMutationReason::None &&
              sleep.GetRequestOrigin() == firmware::SleepRequestOrigin::App &&
              sleep.GetState() == firmware::SleepState::Fading &&
              sleep.GetRemainingCountdownMs() == 0U &&
              sleep.GetCountdownDurationMs() == 0U && sleep.GetFadeScale256() == 256U,
          "typed App Sleep callback acknowledges immediate fade entry without a countdown");
}

void TestRegistryFailuresAndNoop() {
    Harness harness;
    StaticJsonDocument<96> value;

    value.set(4U);
    auto outcome = harness.Set("UNKNOWN", value.as<JsonVariantConst>());
    Check(outcome.reason == modules::ControlMutationReason::UnknownKey,
          "unknown key is stable rejection");
    value.set("bright");
    outcome = harness.Set("BRIGHTNESS", value.as<JsonVariantConst>());
    Check(outcome.reason == modules::ControlMutationReason::TypeMismatch,
          "type mismatch is stable rejection");
    value.set(9U);
    outcome = harness.Set("BRIGHTNESS", value.as<JsonVariantConst>());
    Check(outcome.reason == modules::ControlMutationReason::OutOfRange,
          "range failure is stable rejection");
    value.set("invalid");
    outcome = harness.Set("MOVE_CONFIRM", value.as<JsonVariantConst>());
    Check(outcome.reason == modules::ControlMutationReason::InvalidEnumValue,
          "enum failure is stable rejection");
    Check(harness.beforeCount == 0U && harness.settings.GetRevision() == 0U,
          "rejections do not invoke callback or dirty settings");

    value.set(5U);
    outcome = harness.Set("BRIGHTNESS", value.as<JsonVariantConst>(), 41U);
    Check(outcome.status == modules::ControlMutationStatus::Noop &&
              outcome.requestId == 41U &&
              outcome.settingsRevision == 0U,
          "effective default write is correlated Noop");
    Check(harness.beforeCount == 0U && harness.projectionCount == 0U,
          "Noop has no pre-callback or projection side effect");
}

void TestUnconfiguredServiceFailsClosed() {
    modules::ControlMutationService service;
    StaticJsonDocument<32> value;
    value.set(6U);

    const auto write = service.MutateSetting(
        "BRIGHTNESS", value.as<JsonVariantConst>(), 31U);
    Check(write.status == modules::ControlMutationStatus::Rejected &&
              write.reason ==
                  modules::ControlMutationReason::StorageUnavailable &&
              write.requestId == 31U,
          "unconfigured service rejects setting writes as storage unavailable");

    const auto reset = service.ResetSetting("BRIGHTNESS", 32U);
    Check(reset.status == modules::ControlMutationStatus::Rejected &&
              reset.reason ==
                  modules::ControlMutationReason::StorageUnavailable &&
              reset.requestId == 32U,
          "unconfigured service rejects setting resets without mutation");
}

void TestSettingResetAndAliasCanonicalization() {
    Harness harness;
    StaticJsonDocument<32> value;

    harness.settings.Doc()["BRIGHTNESS"] = 7U;
    auto outcome = harness.service.ResetSetting("brightness", 42U);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.requestId == 42U &&
              outcome.persistenceTier ==
                  modules::ControlMutationPersistenceTier::Ordinary &&
              outcome.settingsRevision == 1U &&
              harness.settings.ReadValue("BRIGHTNESS", 5U) == 5U &&
              !harness.settings.Doc().containsKey("BRIGHTNESS"),
          "registered reset removes the stored value and restores its default");
    Check(harness.beforeCount == 1U && harness.projectionCount == 1U,
          "effective reset runs the ordinary safety and projection policy");

    outcome = harness.service.ResetSetting("BRIGHTNESS", 43U);
    Check(outcome.status == modules::ControlMutationStatus::Noop &&
              outcome.settingsRevision == 1U &&
              harness.beforeCount == 1U && harness.projectionCount == 1U,
          "absent default reset is a side-effect-free Noop");

    harness.settings.Doc()["BRIGHTNESS"] = 5U;
    outcome = harness.service.ResetSetting("BRIGHTNESS", 44U);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.settingsRevision == 2U &&
              !harness.settings.Doc().containsKey("BRIGHTNESS") &&
              harness.beforeCount == 2U && harness.projectionCount == 1U,
          "explicit stored default reset is document-changing Applied without projection");

    harness.settings.Doc()["brightness"] = 5U;
    value.set(5U);
    outcome = harness.Set("BRIGHTNESS", value.as<JsonVariantConst>(), 45U);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.settingsRevision == 3U &&
              harness.settings.Doc().containsKey("BRIGHTNESS") &&
              !harness.settings.Doc().containsKey("brightness"),
          "case-alias cleanup is an Applied service mutation with one revision");

    harness.settings.Doc()["board_rotate"] = true;
    harness.startGate = modules::ControlCoordinatorResult::NotAtStart;
    const uint32_t gatedRevision = harness.settings.GetRevision();
    outcome = harness.service.ResetSetting("BOARD_ROTATE", 46U);
    Check(outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason == modules::ControlMutationReason::NotAtStart &&
              harness.settings.ReadValue("BOARD_ROTATE", false) &&
              harness.settings.GetRevision() == gatedRevision,
          "start-only reset rejects without removing aliases or changing revision");

    harness.startGate = modules::ControlCoordinatorResult::Applied;
    harness.settings.Doc()["VS_CPU_LEVEL"] = 4U;
    harness.settings.Doc()["HINT_LEVEL"] = 9U;
    (void)harness.service.ResetSetting("VS_CPU_LEVEL");
    (void)harness.service.ResetSetting("HINT_LEVEL");
    Check(harness.cpuCancelCount == 1U && harness.hintCancelCount == 0U,
          "CPU reset cancels CPU work; retired hint level never cancels analysis");

    harness.settings.Doc()["MOVE_CONFIRM"] = "immediate";
    const uint32_t enumResetRevision = harness.settings.GetRevision();
    outcome = harness.service.ResetSetting("MOVE_CONFIRM", 47U);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              !harness.settings.Doc().containsKey("MOVE_CONFIRM") &&
              outcome.settingsRevision == enumResetRevision + 1U,
          "enum reset safely removes the override with one revision");

    Harness disabled;
    disabled.settings.DisableForBoot();
    disabled.settings.Doc()["BRIGHTNESS"] = 7U;
    const uint32_t disabledRevision = disabled.settings.GetRevision();
    outcome = disabled.service.ResetSetting("BRIGHTNESS");
    Check(outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason ==
                  modules::ControlMutationReason::StorageUnavailable &&
              disabled.settings.Doc().containsKey("BRIGHTNESS") &&
              disabled.settings.GetRevision() == disabledRevision &&
              disabled.beforeCount == 0U,
          "reset rejects disabled durable storage before any document effect");

    Harness palette;
    palette.settings.Doc()["GAME_FIRST_PLAYER"] = 2U;
    palette.settings.Doc()["P1_END_PIECE_WHEEL"] = 43U;
    palette.settings.Doc()["P2_END_PIECE_WHEEL"] = 213U;
    palette.settings.Doc()["P1_PIECE_WHEEL"] = 213U;
    palette.settings.Doc()["P2_PIECE_WHEEL"] = 43U;
    outcome = palette.service.ResetSetting("GAME_FIRST_PLAYER", 48U);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              !palette.settings.Doc().containsKey("GAME_FIRST_PLAYER") &&
              palette.settings.Doc()["P1_PIECE_WHEEL"].as<uint32_t>() ==
                  43U &&
              palette.settings.Doc()["P2_PIECE_WHEEL"].as<uint32_t>() ==
                  213U,
          "player-end reset reprojects logical aliases through default placement");

    palette.settings.Doc()["P2_END_PIECE_WHEEL"] = 160U;
    palette.settings.Doc()["P2_PIECE_WHEEL"] = 160U;
    outcome = palette.service.ResetSetting("P1_END_PIECE_WHEEL", 49U);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              palette.settings.Doc()["P1_END_PIECE_WHEEL"].as<uint32_t>() ==
                  192U &&
              palette.settings.Doc()["P1_PIECE_WHEEL"].as<uint32_t>() ==
                  192U,
          "physical palette reset preserves the two-end separation invariant");
}

void TestBleEnabledUsesOrdinarySettingSemantics() {
    Harness harness;
    StaticJsonDocument<16> value;

    value.set(true);
    auto outcome = harness.Set("BLE_ENABLED",
                               value.as<JsonVariantConst>(), 50U);
    Check(outcome.status == modules::ControlMutationStatus::Noop &&
              outcome.requestId == 50U &&
              outcome.settingsRevision == 0U &&
              harness.beforeCount == 0U,
          "default BLE enabled write is a side-effect-free service Noop");

    value.set(false);
    outcome = harness.Set("BLE_ENABLED", value.as<JsonVariantConst>(), 51U);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.persistenceTier ==
                  modules::ControlMutationPersistenceTier::Ordinary &&
              outcome.settingsRevision == 1U &&
              !harness.settings.ReadValue("BLE_ENABLED", true) &&
              harness.beforeCount == 1U,
          "BLE disable applies once through ordinary mutation policy");

    outcome = harness.Set("BLE_ENABLED", value.as<JsonVariantConst>(), 52U);
    Check(outcome.status == modules::ControlMutationStatus::Noop &&
              outcome.settingsRevision == 1U &&
              harness.beforeCount == 1U,
          "repeated BLE disable does not add a revision or callback effect");
}

void TestBoardSetupAssistanceUsesOrdinaryUngatedSemantics() {
    Harness harness;
    harness.startGate = modules::ControlCoordinatorResult::NotAtStart;
    StaticJsonDocument<16> value;

    Check(harness.settings.ReadValue("BOARD_SETUP_ASSISTANCE", true),
          "board setup assistance defaults on when absent");

    value.set(false);
    auto outcome = harness.Set(
        "BOARD_SETUP_ASSISTANCE", value.as<JsonVariantConst>(), 53U);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.reason == modules::ControlMutationReason::None &&
              outcome.requestId == 53U &&
              outcome.persistenceTier ==
                  modules::ControlMutationPersistenceTier::Ordinary &&
              outcome.settingsRevision == 1U &&
              !harness.settings.ReadValue("BOARD_SETUP_ASSISTANCE", true),
          "board setup assistance false applies once through ordinary persistence");
    Check(harness.startGateCheckCount == 0U &&
              harness.beforeCount == 1U &&
              harness.projectionCount == 0U &&
              harness.cpuCancelCount == 0U &&
              harness.hintCancelCount == 0U,
          "board setup assistance adds no start gate or dedicated side effect");

    outcome = harness.Set(
        "BOARD_SETUP_ASSISTANCE", value.as<JsonVariantConst>(), 54U);
    Check(outcome.status == modules::ControlMutationStatus::Noop &&
              outcome.reason == modules::ControlMutationReason::None &&
              outcome.requestId == 54U &&
              outcome.settingsRevision == 1U &&
              harness.startGateCheckCount == 0U &&
              harness.beforeCount == 1U,
          "repeated board setup assistance false is a side-effect-free Noop");

    outcome = harness.service.ResetSetting("BOARD_SETUP_ASSISTANCE", 55U);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.reason == modules::ControlMutationReason::None &&
              outcome.requestId == 55U &&
              outcome.persistenceTier ==
                  modules::ControlMutationPersistenceTier::Ordinary &&
              outcome.settingsRevision == 2U &&
              harness.settings.ReadValue("BOARD_SETUP_ASSISTANCE", true) &&
              !harness.settings.Doc().containsKey(
                  "BOARD_SETUP_ASSISTANCE"),
          "board setup assistance reset removes storage and restores default on");
    Check(harness.startGateCheckCount == 0U &&
              harness.beforeCount == 2U &&
              harness.projectionCount == 0U &&
              harness.cpuCancelCount == 0U &&
              harness.hintCancelCount == 0U,
          "board setup assistance reset remains ungated without special effects");
}

void TestUsbCcThresholdTransaction() {
    Harness harness;

    auto outcome = harness.service.MutateUsbCcThresholds(
        400U, 600U, 900U, 51U);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.requestId == 51U &&
              outcome.persistenceTier ==
                  modules::ControlMutationPersistenceTier::MustSave &&
              outcome.settingsRevision == 1U,
          "valid CC threshold group applies with one must-save revision");
    Check(harness.settings.ReadValue("USB_CC_THRESH_500", 0U) == 400U &&
              harness.settings.ReadValue("USB_CC_THRESH_1500", 0U) == 600U &&
              harness.settings.ReadValue("USB_CC_THRESH_3000", 0U) == 900U &&
              harness.beforeCount == 1U,
          "CC threshold group writes all values and deduplicates safety callback");

    outcome = harness.service.MutateUsbCcThresholds(
        400U, 600U, 900U, 52U);
    Check(outcome.status == modules::ControlMutationStatus::Noop &&
              outcome.settingsRevision == 1U && harness.beforeCount == 1U,
          "unchanged CC threshold group has zero callback or revision effects");

    outcome = harness.service.MutateUsbCcThresholds(
        450U, 800U, 700U, 53U);
    Check(outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason == modules::ControlMutationReason::OutOfRange &&
              harness.settings.ReadValue("USB_CC_THRESH_500", 0U) == 400U &&
              harness.settings.ReadValue("USB_CC_THRESH_1500", 0U) == 600U &&
              harness.settings.ReadValue("USB_CC_THRESH_3000", 0U) == 900U &&
              harness.settings.GetRevision() == 1U &&
              harness.beforeCount == 1U,
          "invalid CC relation rejects all-or-none without callback or revision");

    outcome = harness.service.MutateUsbCcThresholds(
        0U, 600U, 900U, 54U);
    Check(outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason == modules::ControlMutationReason::OutOfRange &&
              harness.settings.GetRevision() == 1U,
          "descriptor-invalid CC group leaves the whole document unchanged");

    outcome = harness.service.ResetUsbCcThresholds(55U);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.settingsRevision == 2U &&
              !harness.settings.Doc().containsKey("USB_CC_THRESH_500") &&
              !harness.settings.Doc().containsKey("USB_CC_THRESH_1500") &&
              !harness.settings.Doc().containsKey("USB_CC_THRESH_3000") &&
              harness.beforeCount == 2U,
          "CC reset removes the transactional group with one callback and revision");
    outcome = harness.service.ResetUsbCcThresholds(56U);
    Check(outcome.status == modules::ControlMutationStatus::Noop &&
              outcome.settingsRevision == 2U && harness.beforeCount == 2U,
          "already-default absent CC group resets as a true Noop");

    StaticJsonDocument<32> singleValue;
    singleValue.set(900U);
    outcome = harness.Set("USB_CC_THRESH_500",
                          singleValue.as<JsonVariantConst>(), 57U);
    Check(outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason == modules::ControlMutationReason::OutOfRange &&
              !harness.settings.Doc().containsKey("USB_CC_THRESH_500") &&
              harness.settings.GetRevision() == 2U,
          "single-setting ingress cannot create an invalid CC threshold relation");

    Harness disabled;
    disabled.settings.DisableForBoot();
    disabled.settings.Doc()["USB_CC_THRESH_500"] = 400U;
    disabled.settings.Doc()["USB_CC_THRESH_1500"] = 600U;
    disabled.settings.Doc()["USB_CC_THRESH_3000"] = 900U;
    const uint32_t disabledRevision = disabled.settings.GetRevision();
    outcome = disabled.service.ResetUsbCcThresholds(58U);
    Check(outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason ==
                  modules::ControlMutationReason::StorageUnavailable &&
              outcome.persistenceTier ==
                  modules::ControlMutationPersistenceTier::MustSave &&
              disabled.settings.ReadValue("USB_CC_THRESH_500", 0U) == 400U &&
              disabled.settings.ReadValue("USB_CC_THRESH_1500", 0U) == 600U &&
              disabled.settings.ReadValue("USB_CC_THRESH_3000", 0U) == 900U &&
              disabled.settings.GetRevision() == disabledRevision &&
              disabled.beforeCount == 0U,
          "disabled storage rejects CC reset with zero partial effects");
}

void TestApplyOrderTierRevisionAndWheelSpacing() {
    Harness harness;
    harness.settings.Doc()["BRIGHTNESS"] = 5U;
    StaticJsonDocument<64> value;
    value.set(6U);
    auto outcome = harness.Set("BRIGHTNESS", value.as<JsonVariantConst>());
    Check(outcome.status == modules::ControlMutationStatus::Applied,
          "valid setting is applied");
    Check(outcome.persistenceTier ==
              modules::ControlMutationPersistenceTier::Ordinary &&
              outcome.settingsRevision == 1U,
          "ordinary setting reports one revision and tier");
    Check(harness.beforeCount == 1U &&
              harness.valueSeenBeforeMutation == 5U &&
              harness.projectionCount == 1U,
          "callback observes validated old value before immediate projection");

    value.set(100U);
    (void)harness.Set("P1_PIECE_WHEEL", value.as<JsonVariantConst>());
    value.set(120U);
    outcome = harness.Set("P2_PIECE_WHEEL", value.as<JsonVariantConst>());
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              harness.settings.ReadValue("P2_PIECE_WHEEL", 0U) == 132U,
          "registry-owned wheel spacing is applied by service");

    value.set(321U);
    outcome = harness.Set("USB_CC_THRESH_500", value.as<JsonVariantConst>());
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              outcome.persistenceTier ==
              modules::ControlMutationPersistenceTier::MustSave,
          "valid single CC mutation selects the force-save tier");
}

void TestDomainGatesCleanupAndCancellation() {
    Harness harness;
    StaticJsonDocument<64> value;
    harness.startGate = modules::ControlCoordinatorResult::NotAtStart;
    value.set(true);
    auto outcome = harness.Set("BOARD_ROTATE", value.as<JsonVariantConst>());
    Check(outcome.reason == modules::ControlMutationReason::NotAtStart &&
              harness.beforeCount == 0U,
          "start gate rejects before mutation callback");

    value.set(2U);
    outcome = harness.Set("TIMER", value.as<JsonVariantConst>());
    Check(outcome.status == modules::ControlMutationStatus::Rejected &&
              outcome.reason == modules::ControlMutationReason::NotAtStart &&
              harness.settings.ReadValue("TIMER", 0U) == 0U &&
              harness.settings.GetRevision() == 0U &&
              harness.beforeCount == 0U,
          "midgame timer change is rejected without resetting clock state");

    harness.startGate = modules::ControlCoordinatorResult::Applied;
    harness.settings.Doc()["GAME_P1_PLAYS_BLACK"] = true;
    harness.settings.Doc()["p1_plays_black"] = true;
    value.set(2U);
    outcome = harness.Set("GAME_FIRST_PLAYER", value.as<JsonVariantConst>());
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              !harness.settings.Doc().containsKey("GAME_P1_PLAYS_BLACK") &&
              !harness.settings.Doc().containsKey("p1_plays_black"),
          "first-player mutation preserves legacy cleanup");

    value.set(4U);
    (void)harness.Set("VS_CPU_LEVEL", value.as<JsonVariantConst>());
    value.set(1U);
    (void)harness.Set("VS_CPU_PLAYER", value.as<JsonVariantConst>());
    value.set(9U);
    (void)harness.Set("HINT_LEVEL", value.as<JsonVariantConst>());
    Check(harness.cpuCancelCount == 2U && harness.hintCancelCount == 0U,
          "CPU settings cancel CPU work; legacy hint writes preserve live analysis");
}

void TestActionsAndOwnerLocalCoaching() {
    Harness harness;
    const uint32_t revision = harness.settings.GetRevision();
    auto outcome = harness.service.MutateAction(
        modules::ControlAction::Coaching, 10U, modules::UiOwner::P1,
        modules::CoachingMutation::Enable);
    Check(outcome.status == modules::ControlMutationStatus::Applied &&
              harness.settings.ReadValue("P1_AUTO_HINTS", false) &&
              !harness.settings.ReadValue("P2_AUTO_HINTS", false) &&
              outcome.persistenceTier == modules::ControlMutationPersistenceTier::Ordinary &&
              outcome.settingsRevision == revision + 1U,
          "Coaching persists only the requested owner's Automatic Hints preference");
    outcome = harness.service.MutateAction(
        modules::ControlAction::Coaching, 11U, modules::UiOwner::P1,
        modules::CoachingMutation::Enable);
    Check(outcome.status == modules::ControlMutationStatus::Noop &&
              harness.settings.GetRevision() == revision + 1U,
          "unchanged Coaching does not spend another settings revision");

    harness.settings.Doc()["VS_CPU_LEVEL"] = 1U;
    harness.settings.Doc()["VS_CPU_PLAYER"] = 1U;
    outcome = harness.service.MutateAction(
        modules::ControlAction::Coaching, 12U, modules::UiOwner::P1,
        modules::CoachingMutation::Toggle);
    Check(outcome.status == modules::ControlMutationStatus::Rejected &&
              harness.settings.ReadValue("P1_AUTO_HINTS", false),
          "CPU-owned Coaching action cannot destroy the saved human preference");
    StaticJsonDocument<16> automatic;
    automatic.set(false);
    outcome = harness.Set("P1_AUTO_HINTS", automatic.as<JsonVariantConst>());
    Check(outcome.status == modules::ControlMutationStatus::Rejected &&
              harness.settings.ReadValue("P1_AUTO_HINTS", false),
          "raw setting mutation cannot bypass CPU-owned Coaching inhibition");

    outcome = harness.service.MutateAction(
        modules::ControlAction::Undo, 22U, modules::UiOwner::None,
        modules::CoachingMutation::Toggle, 1U);
    Check(outcome.status == modules::ControlMutationStatus::Accepted &&
              outcome.requestId == 22U,
          "Undo returns immediate correlated Accepted");
    const auto busy = harness.service.MutateAction(
        modules::ControlAction::NewGame, 23U);
    Check(busy.reason == modules::ControlMutationReason::Busy,
          "one bounded asynchronous action is in flight");
    harness.service.Update();
    Check(harness.undoStartCount == 1U,
          "accepted Undo delegates on later service update");
    modules::ControlMutationOutcome completion;
    Check(!harness.service.TryTakeCompletion(completion),
          "pending coordinator has no early completion");
    harness.polled = modules::ControlCoordinatorResult::Applied;
    harness.service.Update();
    Check(harness.service.TryTakeCompletion(completion) &&
              completion.requestId == 22U &&
              completion.status == modules::ControlMutationStatus::Applied,
          "Undo completion retains app request ID");

    outcome = harness.service.MutateAction(
        modules::ControlAction::NewGame, 24U);
    Check(outcome.status == modules::ControlMutationStatus::Accepted,
          "New Game returns immediate Accepted");
    harness.service.Update();
    Check(harness.newGameStartCount == 1U &&
              harness.service.TryTakeCompletion(completion) &&
              completion.requestId == 24U &&
              completion.status == modules::ControlMutationStatus::Applied,
          "New Game completion is later and correlated");
}

void TestUndoOutcomesExposeCoordinatorSemanticState() {
    Harness harness;
    harness.undoStart = modules::ControlCoordinatorResult::Applied;
    harness.actionStateAfterUndoRequest =
        modules::ControlActionState::UndoPending;

    auto outcome = harness.service.MutateAction(
        modules::ControlAction::Undo, 25U);
    Check(outcome.status == modules::ControlMutationStatus::Accepted &&
              outcome.actionState == modules::ControlActionState::Idle,
          "first Undo acknowledgement reports the pre-request semantic state");
    harness.service.Update();
    modules::ControlMutationOutcome completion;
    Check(harness.service.TryTakeCompletion(completion) &&
              completion.status == modules::ControlMutationStatus::Applied &&
              completion.actionState ==
                  modules::ControlActionState::UndoPending &&
              harness.service.GetActionState() ==
                  modules::ControlActionState::UndoPending,
          "first Undo completion reports that physical correction is pending");

    harness.actionStateAfterUndoRequest =
        modules::ControlActionState::Idle;
    outcome = harness.service.MutateAction(
        modules::ControlAction::Undo, 26U);
    Check(outcome.status == modules::ControlMutationStatus::Accepted &&
              outcome.actionState ==
                  modules::ControlActionState::UndoPending,
          "second canonical Undo acknowledgement preserves cancel availability");
    harness.service.Update();
    Check(harness.service.TryTakeCompletion(completion) &&
              completion.requestId == 26U &&
              completion.status == modules::ControlMutationStatus::Applied &&
              completion.actionState == modules::ControlActionState::Idle,
          "cancel-Undo completion reports the restored ordinary state");
}

void TestStableReasonTokens() {
    Check(static_cast<uint8_t>(modules::ControlMutationReason::ArchiveRequired) == 15U &&
              modules::ControlMutationReasonFromCoordinatorResult(
                  modules::ControlCoordinatorResult::ArchiveRequired) ==
                  modules::ControlMutationReason::ArchiveRequired &&
              std::strcmp(modules::ControlMutationReasonToString(
                  modules::ControlMutationReason::ArchiveRequired),
                  "archive_required") == 0 &&
              modules::ControlMutationReasonToCompactCode(
                  modules::ControlMutationReason::ArchiveRequired) == 'r',
          "archive-required rejection has one stable typed and diagnostic reason");
    Harness harness;
    const auto unknown = harness.service.MutateAction(
        modules::ControlAction::Unknown);
    Check(unknown.requestId != 0U &&
              unknown.reason == modules::ControlMutationReason::UnknownAction,
          "unknown action receives an allocated bounded correlation ID");
    Check(std::strcmp(modules::ControlMutationReasonToString(
                          modules::ControlMutationReason::StorageUnavailable),
                      "storage_unavailable") == 0,
          "storage reason string is stable");
    Check(modules::ControlMutationStatusToCompactCode(
              modules::ControlMutationStatus::Accepted) == 'a' &&
              modules::ControlMutationReasonToCompactCode(
                  modules::ControlMutationReason::NotAtStart) == 's',
          "minimum-ATT compact codes are stable");
    modules::ControlMutationOutcome maximum;
    maximum.requestId = UINT16_MAX;
    maximum.status = modules::ControlMutationStatus::Rejected;
    maximum.reason = modules::ControlMutationReason::ArchiveRequired;
    char compact[24] = {};
    const size_t compactLength =
        modules::FormatControlMutationCompactResponse(
            maximum, compact, sizeof(compact));
    Check(compactLength == 18U &&
              std::strcmp(compact, "{\"x\":[65535,2,15]}") == 0,
          "maximum correlated compact response fits ATT MTU 23 payload");
}

void TestTargetedCompletionRetrieval() {
    Harness harness;
    harness.undoStart = modules::ControlCoordinatorResult::Applied;
    auto first = harness.service.MutateAction(
        modules::ControlAction::Undo, 101U, modules::UiOwner::None,
        modules::CoachingMutation::Toggle, 1U);
    Check(first.status == modules::ControlMutationStatus::Accepted,
          "first undo accepted for completion ownership test");
    harness.service.Update();

    auto second = harness.service.MutateAction(
        modules::ControlAction::Undo, 202U, modules::UiOwner::None,
        modules::CoachingMutation::Toggle, 1U);
    Check(second.status == modules::ControlMutationStatus::Accepted,
          "second undo accepted after first completion queued");
    harness.service.Update();

    modules::ControlMutationOutcome completion;
    Check(harness.service.TryTakeCompletion(202U, completion) &&
              completion.requestId == 202U,
          "targeted completion retrieval can take a later request first");
    Check(harness.service.TryTakeCompletion(completion) &&
              completion.requestId == 101U,
          "targeted retrieval preserves earlier unmatched completions");
}

void TestDuplicateCallerSuppliedRequestIds() {
    Harness harness;
    auto accepted = harness.service.MutateAction(
        modules::ControlAction::Undo, 500U, modules::UiOwner::None,
        modules::CoachingMutation::Toggle, 1U);
    Check(accepted.status == modules::ControlMutationStatus::Accepted &&
              accepted.requestId == 500U,
          "first caller-supplied undo ID is accepted");

    auto duplicatePending = harness.service.MutateAction(
        modules::ControlAction::NewGame, 500U);
    Check(duplicatePending.status == modules::ControlMutationStatus::Rejected &&
              duplicatePending.reason == modules::ControlMutationReason::Busy &&
              duplicatePending.requestId == 500U,
          "duplicate caller-supplied pending ID reports busy");

    harness.undoStart = modules::ControlCoordinatorResult::Applied;
    harness.service.Update();
    auto duplicateCompletion = harness.service.MutateAction(
        modules::ControlAction::NewGame, 500U);
    Check(duplicateCompletion.status ==
                  modules::ControlMutationStatus::Rejected &&
              duplicateCompletion.reason ==
                  modules::ControlMutationReason::BadRequest &&
              duplicateCompletion.requestId == 500U,
          "duplicate unread completion ID remains a bad request");

    modules::ControlMutationOutcome completion;
    Check(harness.service.TryTakeCompletion(500U, completion) &&
              completion.requestId == 500U &&
              completion.status == modules::ControlMutationStatus::Applied,
          "accepted action still completes with the same ID");
}

void TestCompletionAbandonmentPreservesExecutionAndOtherOwners() {
    Harness harness;
    harness.newGameStart = modules::ControlCoordinatorResult::Applied;

    auto outcome = harness.service.MutateAction(
        modules::ControlAction::NewGame, 601U);
    Check(outcome.status == modules::ControlMutationStatus::Accepted &&
              harness.service.AbandonCompletion(601U),
          "pending completion ownership can be abandoned");
    harness.service.Update();
    modules::ControlMutationOutcome completion;
    Check(harness.newGameStartCount == 1U &&
              !harness.service.TryTakeCompletion(601U, completion),
          "abandoning a pending response still executes the accepted action once");

    outcome = harness.service.MutateAction(
        modules::ControlAction::NewGame, 602U);
    Check(outcome.status == modules::ControlMutationStatus::Accepted,
          "service accepts later work after a discarded completion");
    harness.service.Update();
    Check(harness.service.TryTakeCompletion(602U, completion) &&
              completion.status == modules::ControlMutationStatus::Applied,
          "later owned completion remains available");

    (void)harness.service.MutateAction(
        modules::ControlAction::NewGame, 603U);
    harness.service.Update();
    (void)harness.service.MutateAction(
        modules::ControlAction::NewGame, 604U);
    harness.service.Update();
    Check(harness.service.AbandonCompletion(604U),
          "already-queued completion can be abandoned by exact ID");
    Check(harness.service.TryTakeCompletion(completion) &&
              completion.requestId == 603U &&
              !harness.service.TryTakeCompletion(604U, completion),
          "targeted abandonment preserves unrelated completion ownership");
}

void TestAbandonCyclesDoNotWedgeCompletionCapacity() {
    Harness harness;
    harness.newGameStart = modules::ControlCoordinatorResult::Applied;
    modules::ControlMutationOutcome completion;

    for (size_t index = 0U;
         index < modules::ControlMutationService::COMPLETION_CAPACITY + 3U;
         ++index) {
        const uint16_t requestId = static_cast<uint16_t>(700U + index);
        const auto accepted = harness.service.MutateAction(
            modules::ControlAction::NewGame, requestId);
        Check(accepted.status == modules::ControlMutationStatus::Accepted &&
                  harness.service.AbandonCompletion(requestId),
              "each abandoned cycle retains an executable pending action");
        harness.service.Update();
        Check(!harness.service.TryTakeCompletion(requestId, completion),
              "abandoned cycle does not consume completion capacity");
    }
    Check(harness.newGameStartCount ==
              modules::ControlMutationService::COMPLETION_CAPACITY + 3U,
          "more-than-capacity abandon cycles execute exactly once each");

    for (size_t index = 0U;
         index < modules::ControlMutationService::COMPLETION_CAPACITY;
         ++index) {
        const uint16_t requestId = static_cast<uint16_t>(800U + index);
        Check(harness.service.MutateAction(
                  modules::ControlAction::NewGame, requestId).status ==
                  modules::ControlMutationStatus::Accepted,
              "unrelated completion queue fill action accepted");
        harness.service.Update();
    }

    const auto discardWhileFull = harness.service.MutateAction(
        modules::ControlAction::NewGame, 900U);
    Check(discardWhileFull.status == modules::ControlMutationStatus::Accepted &&
              harness.service.AbandonCompletion(900U),
          "response can be relinquished while unrelated completion ring is full");
    harness.service.Update();
    const auto next = harness.service.MutateAction(
        modules::ControlAction::NewGame, 901U);
    Check(next.status == modules::ControlMutationStatus::Accepted,
          "discard-marked action progresses despite a full unrelated ring");
    Check(harness.service.AbandonCompletion(901U),
          "new pending action can also relinquish completion ownership");
    harness.service.Update();

    size_t retained = 0U;
    while (harness.service.TryTakeCompletion(completion)) {
        ++retained;
        Check(completion.requestId >= 800U && completion.requestId < 804U,
              "full-ring abandonment never consumes an unrelated completion");
    }
    Check(retained == modules::ControlMutationService::COMPLETION_CAPACITY,
          "all unrelated queued completions remain available");
}

} // namespace

void RuntimeVolatilePermissionPreservesAction() {
    Harness h;
    const auto undo = h.service.MutateAction(modules::ControlAction::Undo, 881U);
    h.service.Update();
    Check(undo.status == modules::ControlMutationStatus::Accepted && h.undoStartCount == 1U,
          "runtime fallback starts with an actual in-flight control action");
    h.settings.ReportAsyncSaveFailure(modules::SettingsStoreError::WriteFailed,
        modules::SettingsStore::StorageFailureEvidence::Confirmed);
    auto degradation = modules::PersistenceDegradation::None;
    Check(modules::UpdateRuntimePersistenceSession(h.settings, h.service, degradation) &&
              h.settings.IsDisabled(), "terminal save changes runtime durability policy");
    StaticJsonDocument<32U> value; value.set(4U);
    const auto applied = h.service.MutateSetting("BRIGHTNESS", value.as<JsonVariantConst>());
    Check(applied.status == modules::ControlMutationStatus::Applied &&
              applied.persistenceTier == modules::ControlMutationPersistenceTier::None,
          "healthy-boot callbacks gain validated RAM permission after runtime failure");
    h.polled = modules::ControlCoordinatorResult::Applied; h.service.Update();
    modules::ControlMutationOutcome complete;
    Check(h.service.TryTakeCompletion(881U, complete) &&
              complete.action == modules::ControlAction::Undo &&
              complete.status == modules::ControlMutationStatus::Applied && h.undoStartCount == 1U,
          "RAM permission transition never drops or repeats pending completion");
}

void TestStylePreviewProjection() {
    Harness harness;
    StaticJsonDocument<64> value;
    value.set("landing_flash");
    Check(harness.Set("SETTLE_ANIM_STYLE", value.as<JsonVariantConst>()).status ==
              modules::ControlMutationStatus::Applied && harness.projectionCount == 1U,
          "changed placement style projects after accepted mutation");
    const auto revision = harness.settings.GetRevision();
    Check(harness.Set("SETTLE_ANIM_STYLE", value.as<JsonVariantConst>()).status ==
              modules::ControlMutationStatus::Noop && harness.projectionCount == 2U &&
              harness.settings.GetRevision() == revision && harness.beforeCount == 1U,
          "fresh same-style placement action projects once without dirtying storage");
    value.set("fireworks");
    Check(harness.Set("CHECKMATE_ANIM_STYLE", value.as<JsonVariantConst>()).status ==
              modules::ControlMutationStatus::Applied && harness.projectionCount == 3U,
          "changed mate style projects accepted current value");
    Check(harness.Set("CHECKMATE_ANIM_STYLE", value.as<JsonVariantConst>()).status ==
              modules::ControlMutationStatus::Noop && harness.projectionCount == 3U,
          "same mate style is quiet");
    value.set("invalid");
    Check(harness.Set("SETTLE_ANIM_STYLE", value.as<JsonVariantConst>()).status ==
              modules::ControlMutationStatus::Rejected && harness.projectionCount == 3U,
          "rejected placement value never projects");
}

void SupportControlUserActionJourney() {
    Harness harness;
    trace::SupportActionHistory actions;
    harness.service.SetSupportActions(&actions);
    harness.newGameStart = modules::ControlCoordinatorResult::Pending;
    auto accepted = harness.service.MutateAction(modules::ControlAction::NewGame, 501U);
    Check(accepted.status == modules::ControlMutationStatus::Accepted && actions.Size() == 1U &&
          actions.At(0U).outcome == trace::SupportActionOutcome::Accepted,
          "support records one accepted New Game, not a completed save");
    (void)harness.service.MutateAction(modules::ControlAction::NewGame, 501U);
    harness.service.Update(); harness.service.Update();
    Check(actions.Size() == 1U && actions.At(0U).outcome == trace::SupportActionOutcome::Accepted,
          "duplicate request and pending polls are not extra user actions");
    harness.polled = modules::ControlCoordinatorResult::Applied;
    harness.service.Update();
    Check(actions.Size() == 1U && actions.At(0U).outcome == trace::SupportActionOutcome::Applied,
          "real coordinator result settles original action row");
    StaticJsonDocument<32> value;
    value.set(9U);
    (void)harness.service.MutateSetting("BRIGHTNESS", value.as<JsonVariantConst>());
    Check(actions.Size() == 2U && actions.At(1U).kind == trace::SupportActionKind::SettingChange,
          "user setting mutation is recorded without retaining key/value");
    (void)harness.service.MutateAction(modules::ControlAction::Coaching, 502U, modules::UiOwner::P1);
    Check(actions.Size() == 3U && actions.At(2U).kind == trace::SupportActionKind::Coaching,
          "nested Coaching setting projection remains one user action");
}

int main() {
    std::puts("[CONTROL_MUTATION_SERVICE] start");
    SupportControlUserActionJourney();
    TestStylePreviewProjection();
    TestTypedSleepAction();
    TestRegistryFailuresAndNoop();
    RuntimeVolatilePermissionPreservesAction();
    TestUnconfiguredServiceFailsClosed();
    TestSettingResetAndAliasCanonicalization();
    TestBleEnabledUsesOrdinarySettingSemantics();
    TestBoardSetupAssistanceUsesOrdinaryUngatedSemantics();
    TestUsbCcThresholdTransaction();
    TestApplyOrderTierRevisionAndWheelSpacing();
    TestDomainGatesCleanupAndCancellation();
    TestActionsAndOwnerLocalCoaching();
    TestUndoOutcomesExposeCoordinatorSemanticState();
    TestStableReasonTokens();
    TestTargetedCompletionRetrieval();
    TestDuplicateCallerSuppliedRequestIds();
    TestCompletionAbandonmentPreservesExecutionAndOtherOwners();
    TestAbandonCyclesDoNotWedgeCompletionCapacity();

    if (g_failures == 0) {
        std::puts("[CONTROL_MUTATION_SERVICE] ok");
        return 0;
    }
    std::printf("[CONTROL_MUTATION_SERVICE] failures=%d\n", g_failures);
    return 1;
}
