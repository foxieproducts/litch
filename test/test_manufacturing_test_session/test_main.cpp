#include <cstdio>
#include <initializer_list>

#include "hardware/board_revision.hpp"
#include "modules/factory_radio_protocol.hpp"
#include "modules/hardware_test_record.hpp"
#include "modules/manufacturing_test_session.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

modules::ButtonEvent Button(const modules::UserButton button,
                            const modules::UiOwner owner) {
    return modules::ButtonEvent{button, fxe::Button::Event::Press, owner};
}

void Tap(modules::ManufacturingTestSession& session, modules::ButtonEvent event, uint32_t nowMs) {
    session.ObserveButton(event, nowMs);
    event.event = fxe::Button::Event::Release;
    session.ObserveButton(event, nowMs);
}

void PressEveryP1Control(modules::ManufacturingTestSession& session) {
    Tap(session, Button(modules::UserButton::Up, modules::UiOwner::P1), 20U);
    Tap(session, Button(modules::UserButton::Down, modules::UiOwner::P1), 21U);
    Tap(session, Button(modules::UserButton::Left, modules::UiOwner::P1), 22U);
    Tap(session, Button(modules::UserButton::Right, modules::UiOwner::P1), 23U);
    Tap(session, Button(modules::UserButton::P1X, modules::UiOwner::P1), 25U);
    Tap(session, Button(modules::UserButton::P1O, modules::UiOwner::P1), 26U);
}

void PressEveryV3Control(modules::ManufacturingTestSession& session) {
    PressEveryP1Control(session);
    Tap(session, Button(modules::UserButton::Up, modules::UiOwner::P2), 30U);
    Tap(session, Button(modules::UserButton::Down, modules::UiOwner::P2), 31U);
    Tap(session, Button(modules::UserButton::Left, modules::UiOwner::P2), 32U);
    Tap(session, Button(modules::UserButton::Right, modules::UiOwner::P2), 33U);
    Tap(session, Button(modules::UserButton::P2X, modules::UiOwner::P2), 35U);
    Tap(session, Button(modules::UserButton::P2O, modules::UiOwner::P2), 36U);
}

void PrepareV3LocalTest(modules::ManufacturingTestSession& session) {
    PressEveryV3Control(session);
    session.ObserveHallSnapshot(true, 0ULL, 40U);
    session.ObserveHallSnapshot(true, UINT64_MAX, 41U);
    session.ObserveHallSnapshot(true, 0ULL, 42U);
    session.ReportAutomatedCheck(
        modules::ManufacturingTestLocalCheck::LedDrive, true, 50U);
    session.ReportAutomatedCheck(
        modules::ManufacturingTestLocalCheck::I2cExpanders, true, 51U);
    session.ReportAutomatedCheck(
        modules::ManufacturingTestLocalCheck::LightSensors, true, 52U);
    session.ReportAutomatedCheck(
        modules::ManufacturingTestLocalCheck::MotionSensor, true, 53U);
    session.ObserveMotionEvent(53U);
    session.ConfirmLedEmission(true, 54U);
    session.ConfirmOperatorReview(54U);
}

void CompleteV3LocalTest(modules::ManufacturingTestSession& session) {
    PrepareV3LocalTest(session);
    for (const uint32_t lux : {100U, 20U}) {
        for (uint8_t sample = 0U; sample < 3U; ++sample) {
            session.ObserveLightSample({3U, {lux, lux}}, 54U);
        }
    }
    session.ObserveHallSnapshot(true, 0ULL, 55U);
}

} // namespace

int main() {
    std::puts("[MANUFACTURING_TEST_SESSION] start");

    modules::ManufacturingTestSession lightRequired;
    lightRequired.Begin(modules::ManufacturingTestConfig::ForRevision(hardware::BoardRevision::V3_3S3), 0U);
    PrepareV3LocalTest(lightRequired);
    lightRequired.ObserveHallSnapshot(true, 0ULL, 55U);
    lightRequired.Tick(11000U);
    Check(lightRequired.GetStage() == modules::ManufacturingTestStage::Local,
          "good links with no ALS response wait locally beyond the electronic deadline");
    for (uint8_t sample = 0U; sample < 3U; ++sample) {
        lightRequired.ObserveLightSample({3U, {80U, 12U}}, 11000U + 50U * sample);
    }
    for (uint8_t sample = 0U; sample < 3U; ++sample) {
        lightRequired.ObserveLightSample({3U, {20U, 12U}}, 11150U + 50U * sample);
    }
    lightRequired.ObserveHallSnapshot(true, 0ULL, 11251U);
    Check(lightRequired.GetProgress().lightSensorResponseMask == 1U &&
              lightRequired.GetStage() == modules::ManufacturingTestStage::Local,
          "one substantial drop without uncovering passes only that sensor and cannot enter Radio");
    for (uint8_t sample = 0U; sample < 3U; ++sample) {
        lightRequired.ObserveLightSample({3U, {20U, 40U}}, 11300U + 50U * sample);
    }
    Check(lightRequired.GetLightSensorResponseMask() == 3U && lightRequired.IsOperatorReviewReady() &&
              lightRequired.GetStage() == modules::ManufacturingTestStage::Local,
          "an independent increase from dim passes the second sensor but cannot reuse the old Hall read");
    lightRequired.ReportCurrentSensorCheck(modules::ManufacturingTestLocalCheck::LightSensors, false, 11401U);
    lightRequired.ObserveHallSnapshot(true, 0ULL, 11402U);
    Check(!lightRequired.IsOperatorReviewReady() && lightRequired.GetLightSensorResponseMask() == 3U &&
              lightRequired.GetStage() == modules::ManufacturingTestStage::Local,
          "historical responses cannot qualify while a current light link is missing");
    lightRequired.ReportCurrentSensorCheck(modules::ManufacturingTestLocalCheck::LightSensors, true, 11403U);
    Check(lightRequired.GetStage() == modules::ManufacturingTestStage::Local,
          "a recovered link still needs a new empty Hall observation");
    lightRequired.ObserveHallSnapshot(true, 0ULL, 11404U);
    Check(lightRequired.GetStage() == modules::ManufacturingTestStage::Radio,
          "the next valid empty Hall admits Radio after both substantial transitions");

    // An isolated spike cannot establish a baseline or count as a response.
    modules::ManufacturingTestSession lightSpike;
    lightSpike.Begin(modules::ManufacturingTestConfig::ForRevision(
                         hardware::BoardRevision::V3S3), 0U);
    for (const uint32_t lux : {100U, 100U, 1000U, 100U, 100U}) {
        lightSpike.ObserveLightSample({3U, {lux, lux}}, 1U);
    }
    Check(lightSpike.GetProgress().lightSensors[0].stage ==
              modules::LightSensorResponseStage::Baseline,
          "single bright sample restarts baseline confirmation");
    lightSpike.ObserveLightSample({3U, {100U, 100U}}, 2U);
    lightSpike.ObserveLightSample({3U, {1000U, 1000U}}, 3U);
    for (uint8_t index = 0U; index < 3U; ++index) {
        lightSpike.ObserveLightSample({3U, {100U, 100U}}, 4U);
    }
    Check(lightSpike.GetProgress().lightSensors[0].stage ==
              modules::LightSensorResponseStage::Change &&
              lightSpike.GetProgress().lightSensors[0].roomLux == 100U,
          "a stable new baseline owns the threshold after a spike");
    for (const uint32_t lux : {20U, 100U, 20U, 20U, 300U, 20U, 70U, 70U, 70U}) {
        lightSpike.ObserveLightSample({3U, {lux, lux}}, 5U);
    }
    Check(lightSpike.GetLightSensorResponseMask() == 0U,
          "single spikes, changing directions, and small relative changes cannot qualify");
    for (uint8_t sample = 0U; sample < 2U; ++sample) {
        lightSpike.ObserveLightSample({3U, {20U, 20U}}, 6U);
    }
    lightSpike.ObserveLightSample({1U, {20U, 999U}}, 7U);
    auto dropout = lightSpike.GetProgress();
    Check(dropout.lightSensorResponseMask == 1U &&
              dropout.lightSensors[1].stage == modules::LightSensorResponseStage::Baseline &&
              dropout.lightSensors[1].lastLux == 20U,
          "an invalid read drops only that sensor's baseline and retains its last good reading");
    for (uint8_t sample = 0U; sample < 3U; ++sample) {
        lightSpike.ObserveLightSample({3U, {20U, 20U}}, 8U);
    }
    Check(lightSpike.GetLightSensorResponseMask() == 1U &&
              lightSpike.GetProgress().lightSensors[1].stage == modules::LightSensorResponseStage::Change,
          "recovered samples establish a new baseline instead of comparing against pre-failure light");
    for (uint8_t sample = 0U; sample < 3U; ++sample) {
        lightSpike.ObserveLightSample({2U, {999U, 40U}}, 9U);
    }
    dropout = lightSpike.GetProgress();
    Check(lightSpike.GetLightSensorResponseMask() == 3U,
          "a new sustained change qualifies the recovered sensor");
    Check(dropout.lightSensors[0].stage == modules::LightSensorResponseStage::Passed &&
              dropout.lightSensors[0].lastLux == 20U,
          "a later invalid sample cannot destroy completed evidence or replace its last good reading");

    modules::ManufacturingTestSession lightReset;
    lightReset.Begin(modules::ManufacturingTestConfig::ForRevision(hardware::BoardRevision::V2S3), 0U);
    for (const uint32_t dim : {8U, 9U, 8U}) {
        lightReset.ObserveLightSample({3U, {100U, dim}}, 1U);
    }
    for (uint8_t sample = 0U; sample < 3U; ++sample) {
        lightReset.ObserveLightSample({3U, {50U, 17U}}, 2U);
    }
    Check(lightReset.GetLightSensorResponseMask() == 1U,
          "an exact 50 percent change passes; a less than ten lux change cannot");
    lightReset.ResetPendingLightResponses();
    auto progress = lightReset.GetProgress();
    Check(progress.lightSensorResponseMask == 1U && progress.lightSensors[0].coveredLux == 50U &&
              progress.lightSensors[1].stage == modules::LightSensorResponseStage::Baseline &&
              progress.lightSensors[1].lastLux == 17U,
          "lighting changes reset unfinished baselines while keeping completed proof and live feedback");
    for (uint8_t sample = 0U; sample < 3U; ++sample) {
        lightReset.ObserveLightSample({3U, {50U, 80U}}, 3U);
    }
    Check(lightReset.GetLightSensorResponseMask() == 1U,
          "a new background reading cannot compare against the previous display's baseline");
    for (uint8_t sample = 0U; sample < 3U; ++sample) {
        lightReset.ObserveLightSample({3U, {50U, UINT32_MAX}}, 4U);
    }
    progress = lightReset.GetProgress();
    Check(progress.lightSensorResponseMask == 3U && progress.lightSensors[1].coveredLux == 80U &&
              progress.lightSensors[1].recoveredLux == UINT32_MAX,
          "a sustained increase stores ordered evidence and does not overflow");

    modules::ManufacturingTestSession pressed;
    pressed.Begin(modules::ManufacturingTestConfig::ForRevision(
                      hardware::BoardRevision::V3S3),
                  0U);
    pressed.ObserveButton(
        Button(modules::UserButton::P1O, modules::UiOwner::P1), 1U);
    Check(pressed.GetProgress().pressedButtonMask == (1U << 6U),
          "physical P1 pressed state uses its own control bank");
    pressed.ObserveButton(
        modules::ButtonEvent{modules::UserButton::P1O,
                             fxe::Button::Event::Release,
                             modules::UiOwner::P1},
        2U);
    Check(pressed.GetProgress().pressedButtonMask == 0U,
          "physical button acknowledgement clears on release");

    modules::ManufacturingTestSession v3;
    v3.Begin(modules::ManufacturingTestConfig::ForRevision(
                 hardware::BoardRevision::V3S3),
             10U);
    PressEveryP1Control(v3);
    Check(v3.GetProgress().buttonsObserved == 6U,
          "V3 keeps P1 progress separate from P2");
    Check(!v3.GetProgress().buttonsComplete,
          "V3 does not pass with only one player's controls");
    PressEveryV3Control(v3);
    Check(v3.GetProgress().buttonsObserved == 12U,
          "V3 requires all twelve physical controls");
    Check(v3.GetProgress().buttonsComplete,
          "V3 button coverage completes after both sides");

    v3.ObserveHallSnapshot(true, 0ULL, 40U);
    v3.ObserveHallSnapshot(true, UINT64_MAX, 41U);
    Check(v3.GetProgress().hallClearObserved == 64U,
          "hall sweep proves every sensor can read clear");
    Check(v3.GetProgress().hallActiveObserved == 64U,
          "hall sweep proves every sensor can read active");
    Check(!v3.GetProgress().hallComplete, "jig must be removed before Hall completion");
    v3.ObserveHallSnapshot(true, 0ULL, 42U);
    Check(v3.GetProgress().hallComplete,
          "hall coverage requires both states on every square");
    Check(!v3.GetProgress().motionEventObserved,
          "V3 retains optional real motion-event evidence separately from required presence/configuration");

    modules::ManufacturingTestSession stuck;
    stuck.Begin(modules::ManufacturingTestConfig::ForRevision(
                    hardware::BoardRevision::V3S3),
                0U);
    stuck.ObserveHallSnapshot(true, 1ULL, 1U);
    stuck.ObserveHallSnapshot(true, UINT64_MAX, 2U);
    Check(!stuck.GetProgress().hallComplete,
          "a sensor never observed clear cannot pass as merely occupied");

    modules::ManufacturingTestSession missingElectronics;
    missingElectronics.Begin(modules::ManufacturingTestConfig::ForRevision(
                                 hardware::BoardRevision::V3S3),
                             0U);
    missingElectronics.Tick(10001U);
    Check(missingElectronics.GetStage() ==
              modules::ManufacturingTestStage::Failed,
          "required automated electronics fail closed after ten seconds");

    modules::ManufacturingTestSession bypass;
    bypass.Begin(modules::ManufacturingTestConfig::ForRevision(
                     hardware::BoardRevision::V3S3),
                 0U);
    CompleteV3LocalTest(bypass);
    Check(bypass.GetStage() == modules::ManufacturingTestStage::Radio,
          "local completion awaits radio proof when no verifier role has been elected");

    bypass.Tick(5100U);
    Check(bypass.GetStage() == modules::ManufacturingTestStage::Radio,
          "elapsed time cannot silently claim a radio bypass");
    bypass.SetAutomaticVerifier(true, 5100U);
    bypass.ObserveHallSnapshot(true, 0ULL, 5101U);
    Check(bypass.GetStage() == modules::ManufacturingTestStage::Radio,
          "automatic verifier with complete local evidence remains pending without radio proof");
    modules::HardwareTestRecord bypassRecord;
    Check(!bypass.BuildPendingRecord(bypassRecord), "no peer never creates a completion record");
    bypass.ReportRadioVerified(modules::FactoryRadioChannel::Ble, 5200U);
    bypass.ObserveHallSnapshot(true, 0ULL, 5220U);
    Check(bypass.BuildPendingRecord(bypassRecord) && bypassRecord.radioResult == modules::ManufacturingTestRadioResult::PeerVerified,
          "verifier needs its own bidirectional BLE proof before saving");
    bypass.ReportPersistenceResult(true);
    Check(bypass.GetStage() == modules::ManufacturingTestStage::Passed, "verified verifier shows PASS only after persistence");

    modules::ManufacturingTestSession verifiedSession;
    verifiedSession.Begin(modules::ManufacturingTestConfig::ForRevision(
                              hardware::BoardRevision::V3S3),
                          0U);
    CompleteV3LocalTest(verifiedSession);
    verifiedSession.ReportRadioVerified(
        modules::FactoryRadioChannel::Ble, 60U);
    Check(verifiedSession.GetStage() ==
              modules::ManufacturingTestStage::Radio,
          "BLE proof still awaits a fresh final empty Hall observation");
    verifiedSession.ObserveHallSnapshot(true, 0ULL, 70U);
    Check(verifiedSession.GetStage() ==
              modules::ManufacturingTestStage::Persisting &&
              verifiedSession.GetRequestedRadioResult() ==
                  modules::ManufacturingTestRadioResult::Verified,
          "BLE challenge response and final empty Hall request verified persistence");
    modules::HardwareTestRecord verifiedRecord;
    Check(verifiedSession.BuildPendingRecord(verifiedRecord) &&
              verifiedRecord.version == 2U &&
              verifiedRecord.radioChannelMask ==
                  modules::FACTORY_RADIO_ALL_CHANNELS &&
              verifiedRecord.factoryVerifierEnabled,
          "verified unit may serve the batch after its saved proof");
    verifiedSession.ReportPersistenceResult(false);
    Check(verifiedSession.GetStage() ==
              modules::ManufacturingTestStage::Failed,
          "save failure cannot display or retain a pass state");

    modules::ManufacturingTestSession unsupported;
    unsupported.Begin(modules::ManufacturingTestConfig::ForRevision(hardware::BoardRevision::V3S3), 0U);
    CompleteV3LocalTest(unsupported);
    unsupported.ReportRadioVerified(static_cast<modules::FactoryRadioChannel>(2U), 60U);
    unsupported.ObserveHallSnapshot(true, 0ULL, 61U);
    Check(unsupported.GetStage() == modules::ManufacturingTestStage::Radio &&
              unsupported.GetProgress().radioChannelMask == 0U,
          "unsupported channel cannot create qualification proof");
    unsupported.ReportRadioVerified(modules::FactoryRadioChannel::Ble, 62U);
    unsupported.ObserveHallSnapshot(false, 0ULL, 63U);
    Check(unsupported.GetStage() == modules::ManufacturingTestStage::Local,
          "invalid final Hall cannot finalize BLE qualification");
    unsupported.ObserveHallSnapshot(true, 1ULL, 64U);
    Check(unsupported.GetStage() == modules::ManufacturingTestStage::Local,
          "occupied final Hall cannot finalize BLE qualification");
    unsupported.ObserveHallSnapshot(true, 0ULL, 65U);
    Check(unsupported.GetStage() == modules::ManufacturingTestStage::Persisting,
          "next valid empty Hall finalizes retained BLE proof");

    modules::ManufacturingTestSession early;
    early.Begin(modules::ManufacturingTestConfig::ForRevision(hardware::BoardRevision::V3S3), 0U);
    early.ReportRadioVerified(modules::FactoryRadioChannel::Ble, 5U);
    Check(early.GetStage() == modules::ManufacturingTestStage::Local,
          "early radio proof cannot bypass local evidence");
    CompleteV3LocalTest(early);
    modules::HardwareTestRecord earlyRecord;
    Check(early.BuildPendingRecord(earlyRecord) && earlyRecord.radioDurationMs == 5U,
          "concurrent proof survives local work and stores its actual independent duration");

    modules::ManufacturingTestSession held;
    held.Begin(modules::ManufacturingTestConfig::ForRevision(hardware::BoardRevision::V3S3), 0U);
    PressEveryV3Control(held);
    held.ObserveButton(Button(modules::UserButton::Left, modules::UiOwner::P2), 50U);
    Check(!held.GetProgress().buttonsComplete,
          "an observed but stuck-down physical control blocks completion");

    modules::ManufacturingTestSession forced;
    forced.Begin(modules::ManufacturingTestConfig::ForRevision(
                     hardware::BoardRevision::V3S3),
                 100U);
    forced.RequestOverride(5100U);
    modules::HardwareTestRecord forcedRecord;
    Check(forced.GetStage() == modules::ManufacturingTestStage::Persisting &&
              forced.GetRequestedRadioResult() ==
                  modules::ManufacturingTestRadioResult::Override &&
              forced.BuildPendingRecord(forcedRecord) &&
              forcedRecord.radioChannelMask == 0U &&
              !forcedRecord.factoryVerifierEnabled,
          "explicit override persists a distinct non-verifier result");

    if (g_failures == 0) {
        std::puts("[MANUFACTURING_TEST_SESSION] ok");
    }
    return g_failures == 0 ? 0 : 1;
}
