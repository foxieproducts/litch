#include <cstdio>
#include <string>

#include "firmware/power_runtime_health.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/settings_store.hpp"
#include "modules/upward_output_slew.hpp"
#include "modules/usb_cc_source_policy.hpp"
#include "modules/volatile_persistence_session.hpp"
#include "modules/control_mutation_service.hpp"

namespace {

int failures = 0;

void Check(const bool value, const char* message) {
    if (value) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++failures;
}

void TestSourceFactMapping() {
    runtime::RuntimeHealthFact fact;
    Check(!firmware::MakePowerSourceRuntimeHealthFact(
              modules::StrongSourceConfirmationTransition::None, 10U, fact),
          "pre-confirmation source evidence publishes no health fact");

    Check(firmware::MakePowerSourceRuntimeHealthFact(
              modules::StrongSourceConfirmationTransition::Gained, 11U, fact) &&
              fact.kind == runtime::RuntimeHealthFactKind::PowerSource &&
              fact.source == runtime::RuntimeHealthSource::Stable &&
              fact.tMs == 11U,
          "confirmed source gain publishes positive stable evidence");

    Check(!firmware::MakePowerSourceRuntimeHealthFact(
              modules::StrongSourceConfirmationTransition::Lost, 12U, fact),
          "loss of CC advertisement alone publishes no power fault");

    runtime::RuntimeHealthReducer reducer;
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "CC loss alone leaves board health healthy");
    firmware::MakePowerSourceRuntimeHealthFact(
        modules::StrongSourceConfirmationTransition::Gained, 13U, fact);
    reducer.Apply(fact);
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "positive confirmed gain clears source degradation");
}

void TestEnvelopeEvidence() {
    const modules::LedPowerEnvelope normal =
        modules::SelectS3LedPowerEnvelope(false);
    const modules::LedPowerEnvelope conservative =
        modules::SelectS3LedPowerEnvelope(true);
    runtime::RuntimeHealthFact fact;

    Check(firmware::EffectiveLedHeadroomMa(normal) == 1000U &&
              firmware::EffectiveS3LedHeadroomPercent(normal) == 100U,
          "normal S3 envelope has full effective LED headroom");
    Check(firmware::EffectiveLedHeadroomMa(conservative) == 216U &&
              firmware::EffectiveS3LedHeadroomPercent(conservative) == 21U,
          "conservative S3 envelope uses total-current headroom math");
    Check(!firmware::MakePowerEnvelopeRuntimeHealthFact(
              true, false, false, conservative, 20U, fact),
          "unavailable safety policy cannot start protection evidence");
    Check(!firmware::MakePowerEnvelopeRuntimeHealthFact(
              false, true, false, normal, 21U, fact),
          "C3 fixed policy publishes no envelope health evidence");
    Check(!firmware::MakePowerEnvelopeRuntimeHealthFact(
              true, true, true, conservative, 21U, fact),
          "brownout-constrained envelope cannot create a second severe cause");

    const modules::LedPowerEnvelope severe{500U, 734U, 234U};
    Check(firmware::MakePowerEnvelopeRuntimeHealthFact(
              true, true, false, severe, 22U, fact) &&
              fact.kind == runtime::RuntimeHealthFactKind::PowerEnvelope &&
              fact.powerEnvelopePercent == 50U,
          "authorized policy feeds effective 50-percent envelope threshold");

    runtime::RuntimeHealthReducer reducer;
    reducer.Apply(fact);
    fact.tMs = 1022U;
    reducer.Apply(fact);
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::DegradedUsable &&
              reducer.Snapshot().advice.boardInputAllowed &&
              reducer.Snapshot().advice.moveAcceptanceAllowed &&
              reducer.Snapshot().advice.clocksAllowed,
          "sustained severe envelope keeps play usable under the latched limit");
}

void TestBootBrownoutResetAndSourceRecoveryJourney() {
    bool storedLatch = false;
    runtime::RuntimeHealthReducer health;
    modules::UsbCcSourcePolicyState source;
    modules::UpwardOutputSlew output;
    const auto thresholds = hardware::GetDefaultUsbCcThresholds();
    const auto publish = [&](const runtime::RuntimeHealthFact& fact) {
        health.Apply(fact);
    };
    const auto budget = [&]() {
        return modules::led_render_policy::MakePowerBudgetPolicyConfig(
            {true, storedLatch});
    };
    const auto checkpoint = [&](const char* stage, const char* nextAction) {
        const auto snapshot = health.Snapshot();
        std::printf("[POWER_BOOT_JOURNEY] stage=%s modeled_total_ma=%u modeled_led_ma=%u latch=%u health=%u causes=%04x strong_samples=%u input=%u clocks=%u next=%s\n",
                    stage, budget().maxTotalCurrentMa,
                    firmware::EffectiveLedHeadroomMa(modules::SelectS3LedPowerEnvelope(storedLatch)),
                    storedLatch, static_cast<unsigned>(snapshot.state), snapshot.causes,
                    source.confirmation.samples, snapshot.advice.boardInputAllowed,
                    snapshot.advice.clocksAllowed, nextAction);
    };
    const auto boot = [&](const bool brownoutReset, const uint32_t nowMs) {
        health = runtime::RuntimeHealthReducer{};
        source = modules::UsbCcSourcePolicyState{};
        output.Reset(modules::S3_CONSERVATIVE_TOTAL_CURRENT_MA, nowMs);
        firmware::RestorePowerProtectionAfterBoot(
            brownoutReset, storedLatch, nowMs,
            [&]() { storedLatch = true; }, publish);
    };
    const auto sample = [&](const uint16_t cc1, const uint16_t cc2,
                            const uint32_t nowMs) {
        const auto update = modules::UpdateUsbCcSourceFromFullSample(
            source, cc1, cc2, thresholds, nowMs);
        source = update.state;
        firmware::ClearPowerProtectionForConfirmedSource(
            update.transition, storedLatch, true, nowMs,
            [&]() { storedLatch = false; }, publish);
        runtime::RuntimeHealthFact fact;
        if (firmware::MakePowerSourceRuntimeHealthFact(
                update.transition, nowMs, fact)) publish(fact);
    };
    const auto reset = [&](const bool freshCompletePair) {
        return firmware::RunValidatedPowerProtectionClear(
            true,
            [&]() {
                source = modules::ResetUsbCcSourcePolicy(source);
                return freshCompletePair;
            },
            [&]() { (void)budget(); },
            [&]() { storedLatch = false; },
            [&]() {
                runtime::RuntimeHealthFact fact;
                fact.kind = runtime::RuntimeHealthFactKind::ResetLatchedProtection;
                publish(fact);
            });
    };
    boot(false, 0U);
    sample(0U, 0U, 0U);
    Check(!storedLatch && budget().maxTotalCurrentMa == 1234U &&
              health.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "unadvertised USB-A boot attempts full normal budget without health fault");
    for (uint32_t nowMs = 40U; nowMs <= 800U; nowMs += 40U)
        output.Update(budget().maxTotalCurrentMa, nowMs);
    Check(output.Update(budget().maxTotalCurrentMa, 840U) == 1234U,
          "unadvertised boot reaches normal output through existing bounded ramp");
    checkpoint("unadvertised-boot", "normal play");
    boot(true, 1000U);
    Check(storedLatch && budget().maxTotalCurrentMa == 450U &&
              runtime::HasRuntimeHealthCause(
                  health.Snapshot().causes, runtime::RuntimeHealthCause::BrownoutLatched),
          "one confirmed brownout selects nominal 500mA protection and health on next boot");
    checkpoint("brownout-next-boot", "reset protection or use a confirmed strong source");
    sample(0U, 0U, 1100U);
    sample(320U, 80U, 2100U);
    Check(storedLatch && budget().maxTotalCurrentMa == 450U,
          "unknown or default CC cannot silently clear a brownout");
    boot(false, 3000U);
    Check(storedLatch && runtime::HasRuntimeHealthCause(
              health.Snapshot().causes, runtime::RuntimeHealthCause::BrownoutLatched),
          "ordinary reboot immediately republishes retained protection");
    Check(!reset(false) && storedLatch, "failed explicit validation retains protection");
    Check(reset(true) && !storedLatch && budget().maxTotalCurrentMa == 1234U &&
              health.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "existing Setup reset clears protection and retries above 500mA on unadvertised power");
    checkpoint("setup-reset", "normal power retry");
    sample(0U, 0U, 4000U);
    Check(!storedLatch && budget().maxTotalCurrentMa == 1234U,
          "unadvertised evidence after reset never relatches protection");
    boot(true, 5000U);
    Check(storedLatch && budget().maxTotalCurrentMa == 450U,
          "another confirmed brownout relatches on its next boot");
    checkpoint("repeat-brownout", "reset protection or use a confirmed strong source");

    for (const uint16_t strongRaw : {620U, 850U}) {
        boot(true, 6000U);
        sample(strongRaw, 80U, 6000U);
        sample(strongRaw, 80U, 7000U);
        Check(storedLatch, "partial strong evidence cannot clear a brownout latch");
        sample(strongRaw, 80U, 8000U);
        Check(!storedLatch && budget().maxTotalCurrentMa == 1234U &&
                  health.Snapshot().state == runtime::RuntimeHealthState::Healthy,
              "confirmed 1.5A or 3A source automatically clears retained brownout protection");
        checkpoint(strongRaw == 620U ? "confirmed-1.5A" : "confirmed-3A", "normal play");
        const auto before = output.Update(budget().maxTotalCurrentMa, 8040U);
        sample(0U, 0U, 8050U);
        sample(0U, 0U, 8070U);
        Check(!storedLatch && budget().maxTotalCurrentMa == 1234U &&
                  health.Snapshot().state == runtime::RuntimeHealthState::Healthy &&
                  output.Update(budget().maxTotalCurrentMa, 8080U) >= before,
              "CC loss neither relatches nor caps, degrades health, or resets output ramp");
        checkpoint("CC-loss", "normal play; source qualification continues");
        sample(strongRaw, 80U, 9000U);
        sample(strongRaw, 80U, 10000U);
        sample(strongRaw, 80U, 11000U);
        Check(modules::IsStrongSourceConfirmed(source.confirmation),
              "source recovery requalifies automatically after CC loss");
    }
    std::printf("[POWER_BOOT_JOURNEY] result=%s\n", failures == 0 ? "accepted" : "rejected");
}

void TestUnavailableSettingsRetainsBrownoutAfterSourceConfirmation() {
    constexpr const char* LATCH_KEY = "LED_BROWNOUT_LIMIT_LATCH";
    const auto thresholds = hardware::GetDefaultUsbCcThresholds();
    for (const uint16_t strongRaw : {620U, 850U}) {
        modules::SettingsStore settings;
        settings.Doc()[LATCH_KEY] = true;
        (void)modules::QuarantineUnavailableSettings(settings, false);
        Check(settings.IsDisabled() && settings.ReadValue(LATCH_KEY, false),
              "production quarantine retains the known brownout in disabled settings");
        runtime::RuntimeHealthReducer health;
        const auto publish = [&](const runtime::RuntimeHealthFact& fact) { health.Apply(fact); };
        firmware::RestorePowerProtectionAfterBoot(false,
            settings.ReadValue(LATCH_KEY, false), 0U, []() {}, publish);
        modules::UsbCcSourcePolicyState source;
        unsigned clears = 0U;
        unsigned stableFacts = 0U;
        for (uint32_t nowMs : {0U, 1000U, 2000U}) {
            const auto update = modules::UpdateUsbCcSourceFromFullSample(
                source, strongRaw, 80U, thresholds, nowMs);
            source = update.state;
            firmware::ClearPowerProtectionForConfirmedSource(update.transition,
                settings.ReadValue(LATCH_KEY, false), !settings.IsDisabled(), nowMs,
                [&]() {
                    ++clears;
                    settings.Doc().remove(LATCH_KEY);
                    settings.ArmPersistenceGuard();
                    settings.MarkMustSaveDirty();
                }, publish);
            runtime::RuntimeHealthFact fact;
            if (firmware::MakePowerSourceRuntimeHealthFact(update.transition, nowMs, fact)) {
                ++stableFacts;
                publish(fact);
            }
        }
        const auto budget = modules::led_render_policy::MakePowerBudgetPolicyConfig(
            {true, settings.ReadValue(LATCH_KEY, false)});
        Check(modules::IsStrongSourceConfirmed(source.confirmation) && stableFacts == 1U,
              "unavailable settings retain confirmed-source diagnostics after three valid pairs");
        Check(clears == 0U && settings.ReadValue(LATCH_KEY, false) &&
                  runtime::HasRuntimeHealthCause(health.Snapshot().causes,
                      runtime::RuntimeHealthCause::BrownoutLatched) &&
                  budget.maxTotalCurrentMa == 450U,
              "unavailable settings retain latch, brownout health, and 450mA after strong-source confirmation");
        std::printf("[POWER_UNAVAILABLE_JOURNEY] source_raw=%u confirmed=%u disabled=%u latch=%u health=%u causes=%04x modeled_total_ma=%u\n",
                    strongRaw, modules::IsStrongSourceConfirmed(source.confirmation),
                    settings.IsDisabled(), settings.ReadValue(LATCH_KEY, false),
                    static_cast<unsigned>(health.Snapshot().state), health.Snapshot().causes,
                    budget.maxTotalCurrentMa);
    }
}

void TestBrownoutAndValidatedClear() {
    runtime::RuntimeHealthReducer reducer;
    runtime::RuntimeHealthFact ambient;
    ambient.tMs = 1U;
    ambient.kind = runtime::RuntimeHealthFactKind::AmbientSensorCount;
    ambient.ambientSensorCount = 0U;
    reducer.Apply(ambient);

    runtime::RuntimeHealthFact brownout;
    Check(!firmware::MakeBrownoutRuntimeHealthFact(false, 2U, brownout),
          "an inactive persisted brownout latch publishes no fact");
    Check(firmware::MakeBrownoutRuntimeHealthFact(true, 2U, brownout) &&
              brownout.kind == runtime::RuntimeHealthFactKind::Brownout &&
              brownout.brownoutDetected && brownout.tMs == 2U,
          "an active persisted brownout latch republishes protection after boot");
    reducer.Apply(brownout);
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::DegradedUsable &&
              reducer.Snapshot().advice.boardInputAllowed &&
              reducer.Snapshot().advice.moveAcceptanceAllowed &&
              reducer.Snapshot().advice.clocksAllowed,
          "boot brownout fact retains the safe limit without pausing play");
    Check(!firmware::CanClearPowerProtection(true, false),
          "missing S3 full sample rejects clear before reducer reset");
    Check(firmware::CanClearPowerProtection(true, true),
          "fresh complete S3 sample validates clear boundary");
    Check(firmware::CanClearPowerProtection(false, false),
          "C3 fixed policy validates explicit clear without CC sample");

    runtime::RuntimeHealthFact reset;
    reset.tMs = 3U;
    reset.kind = runtime::RuntimeHealthFactKind::ResetLatchedProtection;
    reducer.Apply(reset);
    const runtime::RuntimeHealthSnapshot snapshot = reducer.Snapshot();
    Check(snapshot.state == runtime::RuntimeHealthState::DegradedUsable &&
              runtime::HasRuntimeHealthCause(
                  snapshot.causes, runtime::RuntimeHealthCause::NoAmbientSensors) &&
              !runtime::HasRuntimeHealthCause(
                  snapshot.causes, runtime::RuntimeHealthCause::BrownoutLatched),
          "validated reset clears only power protection, preserving unrelated causes");
}

void TestProductionClearOwnerOrderingAndFailureRetention() {
    bool persistedLatch = true;
    uint16_t activeEnvelopeMa = 216U;
    uint32_t resetFacts = 0U;
    std::string operations;
    runtime::RuntimeHealthReducer reducer;
    runtime::RuntimeHealthFact ambient;
    ambient.tMs = 1U;
    ambient.kind = runtime::RuntimeHealthFactKind::AmbientSensorCount;
    ambient.ambientSensorCount = 0U;
    reducer.Apply(ambient);
    runtime::RuntimeHealthFact brownout;
    (void)firmware::MakeBrownoutRuntimeHealthFact(true, 2U, brownout);
    reducer.Apply(brownout);

    const auto run = [&](const bool sampleValid) {
        return firmware::RunValidatedPowerProtectionClear(
            true,
            [&]() {
                operations += 'S';
                return sampleValid;
            },
            [&]() {
                operations += 'R';
                activeEnvelopeMa = persistedLatch ? 216U : 1000U;
            },
            [&]() {
                operations += 'C';
                persistedLatch = false;
            },
            [&]() {
                operations += 'P';
                ++resetFacts;
                runtime::RuntimeHealthFact reset;
                reset.tMs = 3U;
                reset.kind =
                    runtime::RuntimeHealthFactKind::ResetLatchedProtection;
                reducer.Apply(reset);
            });
    };

    Check(!run(false) && operations == "S" && persistedLatch &&
              activeEnvelopeMa == 216U && resetFacts == 0U &&
              runtime::HasRuntimeHealthCause(
                  reducer.Snapshot().causes,
                  runtime::RuntimeHealthCause::BrownoutLatched),
          "failed fresh S3 sample preserves stored latch, safe envelope, and health cause");

    operations.clear();
    Check(run(true) && operations == "SRCRP" && !persistedLatch &&
              activeEnvelopeMa == 1000U && resetFacts == 1U &&
              !runtime::HasRuntimeHealthCause(
                  reducer.Snapshot().causes,
                  runtime::RuntimeHealthCause::BrownoutLatched) &&
              runtime::HasRuntimeHealthCause(
                  reducer.Snapshot().causes,
                  runtime::RuntimeHealthCause::NoAmbientSensors),
          "production clear owner validates, recomputes, clears, recomputes, and publishes once");
}

void TestDeferredSaveVersusTerminalSessionPowerPolicy() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    using Admission = fxe::SettingsStore::AsyncSaveAdmission;
    modules::SettingsStore settings;
    settings.Doc()["GAME_FEN"] = "retained live game";
    settings.Doc()["BRIGHTNESS"] = 8U;
    Admission admission = Admission::Deferred;
    settings.SetAsyncSaveAdmissionCallback([&](const JsonDocument&) { return admission; });
    settings.MarkMustSaveDirty();
    modules::ControlMutationService controls;
    auto degradation = modules::PersistenceDegradation::None;
    Check(!settings.SaveNow() && !settings.HasTerminalSaveFailure(),
          "busy artwork/settings admission defers without inventing terminal storage failure");
    fxe::ManualMillis::AdvanceMs(20000U);
    modules::UpdateRuntimePersistenceSession(settings, controls, degradation, true);
    Check(!settings.IsDisabled() && !settings.HasTerminalSaveFailure(),
          "a twenty-second deferred save alone cannot enter session-only policy");
    admission = Admission::Rejected;
    Check(!settings.SaveNow() && !settings.HasTerminalSaveFailure(),
          "consumer rejection cannot supply terminal storage failure evidence");
    modules::UpdateRuntimePersistenceSession(settings, controls, degradation, true);
    Check(!settings.IsDisabled(), "rejected request leaves shared saving available");
    modules::UpdateRuntimePersistenceSession(settings, controls, degradation, false);
    const auto budget = modules::led_render_policy::MakePowerBudgetPolicyConfig(
        {true,false});
    runtime::RuntimeHealthReducer health;
    runtime::RuntimeHealthFact fact;
    Check(settings.IsDisabled() && budget.maxTotalCurrentMa == modules::S3_NORMAL_TOTAL_CURRENT_MA &&
              health.Snapshot().state == runtime::RuntimeHealthState::Healthy &&
              !firmware::MakePowerEnvelopeRuntimeHealthFact(true,false,false,
                  modules::SelectS3LedPowerEnvelope(true),fxe::Millis(),fact) &&
              settings.Doc()["GAME_FEN"].as<std::string>() == "retained live game" &&
              settings.ReadValue("BRIGHTNESS",0U) == 8U &&
              degradation == modules::PersistenceDegradation::None,
          "unavailable worker preserves live game, brightness and normal power with a truthful session-only notice");
    admission = Admission::Accepted;
    settings.ReportAsyncSaveSuccess(100U,1U);
    modules::UpdateRuntimePersistenceSession(settings, controls, degradation, true);
    Check(settings.IsDisabled() && !settings.SaveNow(),
          "late completion cannot silently re-enable this boot's disabled saving");
    std::puts("[POWER_SAVE_BOUNDARY] deferred/rejected=retryable; worker-unavailable=session-only/normal-power; late-success=still-session-only");
}

} // namespace

int main() {
    std::puts("[POWER_RUNTIME_HEALTH] start");
    TestDeferredSaveVersusTerminalSessionPowerPolicy();
    TestSourceFactMapping();
    TestEnvelopeEvidence();
    TestBootBrownoutResetAndSourceRecoveryJourney();
    TestUnavailableSettingsRetainsBrownoutAfterSourceConfirmation();
    TestBrownoutAndValidatedClear();
    TestProductionClearOwnerOrderingAndFailureRetention();
    std::printf("[POWER_RUNTIME_HEALTH] %s\n",
                failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
