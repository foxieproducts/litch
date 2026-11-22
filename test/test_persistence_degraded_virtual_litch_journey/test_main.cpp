#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include "virtual_litch_session.hpp"
#include "modules/board_health_notification.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/rgb_optical_calibration.hpp"
#include "modules/rgbw_auto_map.hpp"
#include "modules/volatile_persistence_session.hpp"
#include "firmware/sleep_controller.hpp"

namespace {
constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] persistence journey: %s\n", message); }
}
std::string Read(const std::string& path) {
    std::ifstream in(path); return {std::istreambuf_iterator<char>(in), {}};
}
void Write(const std::string& path, const std::string& text) { std::ofstream(path) << text; }
virtual_litch_config Config() {
    virtual_litch_config c{}; virtual_litch_default_config(&c);
    c.hardware = VIRTUAL_LITCH_HARDWARE_S3; return c;
}
void Opening(runtime::VirtualLitchSession& s) {
    Check(s.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK, "complete opening scan accepted");
    Check(s.Advance(200U) == VIRTUAL_LITCH_OK && s.Advance(6000U) == VIRTUAL_LITCH_OK, "setup acknowledgement expires");
    Check(s.State().expected_occupied_mask == OPENING && s.State().correction_mask == 0,
          "opening playable without correction");
}
uint8_t ScaleChannel(const uint8_t value, const uint16_t scale65535) {
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(value) * scale65535 + 32767U) / 65535U);
}
virtual_litch_color ExpectedStatusPixel(
    const virtual_litch_color semantic, const uint16_t scale65535) {
    const auto calibrated = modules::rgb_optical_calibration::PresentForS3({
        ScaleChannel(semantic.r, scale65535),
        ScaleChannel(semantic.g, scale65535),
        ScaleChannel(semantic.b, scale65535),
    });
    const auto mapped = modules::AutoMapRgbToRgbw(calibrated, 32U);
    return {mapped.r, mapped.g, mapped.b, mapped.w};
}
void CheckTurn(runtime::VirtualLitchSession& s,
               runtime::VirtualLitchSession& healthy,
               const bool savingUnavailable) {
    const uint32_t phase = s.State().now_ms % 5000U;
    const uint32_t untilPulse = phase == 0U ? 0U : 5000U - phase;
    Check(s.Advance(untilPulse) == VIRTUAL_LITCH_OK &&
              healthy.Advance(untilPulse) == VIRTUAL_LITCH_OK,
          "storage warning and healthy reference reach the same five-second pulse boundary");
    unsigned differences = 0;
    unsigned blue = 0;
    const uint16_t statusStart =
        static_cast<uint16_t>(s.PhysicalFrame().led_count - 32U);
    for (unsigned i=0; i<32; ++i) {
        const auto a = s.SemanticFrame().status[i];
        const auto b = healthy.SemanticFrame().status[i];
        Check(a.r == b.r && a.g == b.g && a.b == b.b,
              "persistence degradation does not alter the semantic Turn ring");
        const auto degradedPhysical = s.PhysicalFrame().pixels[statusStart + i];
        const auto expectedPhysical = ExpectedStatusPixel(
            a, s.PhysicalFrame().applied_brightness_scale_65535);
        if (degradedPhysical.r != expectedPhysical.r ||
            degradedPhysical.g != expectedPhysical.g ||
            degradedPhysical.b != expectedPhysical.b ||
            degradedPhysical.w != expectedPhysical.w) {
            ++differences;
            std::printf("[PLAYBACK] edge-diff index=%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                        i, degradedPhysical.r, degradedPhysical.g,
                        degradedPhysical.b, degradedPhysical.w,
                        expectedPhysical.r, expectedPhysical.g,
                        expectedPhysical.b, expectedPhysical.w);
        }
        if (a.b > a.r) ++blue;
    }
    std::printf("[PLAYBACK] edge-check differences=%u blue_turn_pixels=%u cue=%u\n",differences,blue,unsigned(s.EdgeCueIntent().kind));
    if (savingUnavailable) {
        Check(differences == 1 &&
                  s.PhysicalFrame().pixels[statusStart + 27U].r != 0U &&
                  s.PhysicalFrame().pixels[statusStart + 27U].g != 0U &&
                  s.PhysicalFrame().pixels[statusStart + 27U].b == 0U,
              "unavailable saving overlays one fixed USB-side amber marker, retaining every other Turn pixel");
    } else {
        Check(differences == 0,
              "automatic game-record repair does not present a false flash-health warning on the board");
    }
    Check(blue > 0, "active P1 Turn remains visible, never a red fault ring");
    Check(s.PhysicalFrame().led_count != 0, "production physical pipeline rendered");
}
void Move(runtime::VirtualLitchSession& s) {
    uint64_t occ = OPENING & ~(UINT64_C(1)<<52);
    Check(s.ObserveHallMask(occ) == VIRTUAL_LITCH_OK, "e2 lift accepted");
    occ |= UINT64_C(1)<<36;
    Check(s.ObserveHallMask(occ) == VIRTUAL_LITCH_OK && s.Advance(2100) == VIRTUAL_LITCH_OK,
          "e4 place and settle accepted");
    Check(s.State().expected_occupied_mask == occ && s.State().correction_mask == 0,
          "degraded session completes e2e4 and gives next turn");
}
void BootFixture(const std::string& path, const std::string& data, char token) {
    Write(path,data);
    test_support::BoundedHintSession sessionFixture(Config(),path.c_str());
    auto& session = sessionFixture.session;
    auto& settings = session.SettingsForPersistenceTest();
    const std::string healthyPath = path + ".healthy";
    Write(healthyPath,
          std::string("{\"BRIGHTNESS\":") +
              std::to_string(settings.ReadValue(
                  "BRIGHTNESS", static_cast<uint32_t>(8U))) +
              "}");
    test_support::BoundedHintSession healthyFixture(
        Config(), healthyPath.c_str());
    auto& healthy = healthyFixture.session;
    Opening(session); Opening(healthy);
    const auto line = session.HomeStateLine();
    const std::string expected = std::string("\"ps\":\"")+token+"\"";
    Check(token == 'n' ? std::strstr(line.CStr(), "\"ps\"") == nullptr :
              std::strstr(line.CStr(), expected.c_str()) != nullptr,
          "logical load refusal does not claim confirmed storage failure");
    Check(Read(path) == data, "quarantine preserves original flash evidence");
    const bool sessionOnly = token == 'n';
    Check(settings.IsDisabled() == sessionOnly,
          sessionOnly
              ? "unreadable settings use an explicitly session-only document"
              : "rejected game keeps the readable settings document writable");
    CheckTurn(session,healthy,false);
    Check(settings.ReadValue("LED_BROWNOUT_LIMIT_LATCH",false) == (token=='q'),
          "known latch preserved; corrupt JSON never invents brownout");
    modules::ControlMutationCallbacks cb; cb.allowVolatileUserSettings = true;
    cb.checkStartOnlyGate = [](void*,const char*) {return modules::ControlCoordinatorResult::Applied;};
    modules::ControlMutationService mutation; mutation.Configure(settings,cb);
    StaticJsonDocument<16> value; value.set(3U);
    const auto brightness =
        mutation.MutateSetting("BRIGHTNESS",value.as<JsonVariantConst>());
    Check(brightness.status == modules::ControlMutationStatus::Applied &&
              brightness.persistenceTier ==
                  (sessionOnly
                       ? modules::ControlMutationPersistenceTier::None
                       : modules::ControlMutationPersistenceTier::Ordinary),
          sessionOnly
              ? "validated RAM brightness is usable without claiming durability"
              : "game quarantine retains ordinary durable settings writes");
    value.set(100U);
    Check(mutation.MutateSetting("BRIGHTNESS",value.as<JsonVariantConst>()).reason == modules::ControlMutationReason::OutOfRange,
          "volatile permission retains range validation");
    if (sessionOnly) {
        Check(mutation.MutateUsbCcThresholds(100U,200U,300U).status == modules::ControlMutationStatus::Rejected,
              "volatile session refuses hardware calibration");
    }
    value.set(85U);
    for (const char* key : {"P1_END_PIECE_WHEEL", "P2_END_PIECE_WHEEL"}) {
        const auto outcome = mutation.MutateSetting(key, value.as<JsonVariantConst>());
        Check(outcome.status == modules::ControlMutationStatus::Applied &&
                  outcome.persistenceTier ==
                      (sessionOnly
                           ? modules::ControlMutationPersistenceTier::None
                           : modules::ControlMutationPersistenceTier::Ordinary),
              sessionOnly
                  ? "Setup color applies without claiming durability"
                  : "Setup color remains eligible for ordinary persistence");
        Check(settings.ReadValue(key, 999U) == value.as<uint32_t>(),
              "Setup color readback reflects the accepted RAM value");
        value.set(170U);
    }
    Check(mutation.MutateSetting("LED_BROWNOUT_LIMIT_LATCH", value.as<JsonVariantConst>()).status ==
              modules::ControlMutationStatus::Rejected,
          "volatile access does not expose internal-only safety state");
    if (!sessionOnly) {
        std::string queued;
        settings.SetAsyncSaveRequestCallback([&](const JsonDocument& doc) {
            queued.clear();
            serializeJson(doc, queued);
            return true;
        });
        Check(settings.SaveNow() && !queued.empty(),
              "repaired game and retained settings enter the verified save route");
        DynamicJsonDocument repaired(8192U);
        Check(!deserializeJson(repaired, queued),
              "repair snapshot is valid JSON");
        Check(repaired["BRIGHTNESS"].as<uint32_t>() == 3U &&
                  std::strcmp(repaired["GAME_VARIANT_V1"] | "", "invalid") != 0,
              "repair snapshot retains preferences and replaces only the rejected game");
        auto degradation = session.PersistenceDegradation();
        Check(modules::NoteVerifiedPersistenceSave(degradation) &&
                  degradation == modules::PersistenceDegradation::None,
              "only verified save completion clears game-repair health");
    }
    Move(session);
    std::printf(
        "[PLAYBACK] boot-file-%c input=real-file-load/full-opening mode=FreshSetup outcome=playable edge=%s ack=expired app=%s next=e2e4/next-turn storage=evidence-preserved\n",
        token, sessionOnly ? "Turn/session-only-no-flash-warning" : "Turn-only/repair-pending",
        line.CStr());
}
void BootRepairFixture(const std::string& path) {
    Write(path, "{\"BRIGHTNESS\":2}");
    Write(path + ".tmp", "{\"BRIGHTNESS\":6,\"P1_END_PIECE_WHEEL\":85}");
    std::filesystem::create_directory(path + ".bak");
    Write(path + ".bak/block", "occupied");
    test_support::BoundedHintSession fixture(Config(), path.c_str());
    auto& session = fixture.session;
    auto& source = session.SettingsForPersistenceTest();
    Check(!source.IsDisabled() && source.IsLoadRepairPending(),
          "failed boot repair retains worker eligibility and exact repair state");
    Check(source.ReadValue("BRIGHTNESS", 0U) == 6U &&
              source.ReadValue("P1_END_PIECE_WHEEL", 0U) == 85U,
          "failed promotion retains the valid recovered document");
    Check(std::strstr(session.HomeStateLine().CStr(), "\"ps\"") == nullptr && source.IsLoadRepairPending(),
          "unconfirmed path repair stays pending without claiming flash failure");
    Check(Read(path) == "{\"BRIGHTNESS\":2}" &&
              Read(path + ".tmp").find("\"BRIGHTNESS\":6") != std::string::npos,
          "failed repair preserves both valid source artifacts");
    std::string queued;
    source.SetAsyncSaveRequestCallback([&](const JsonDocument& doc) {
        queued.clear(); serializeJson(doc, queued); return true;
    });
    Check(source.SaveNow(), "recovered document can enter the existing worker route");
    Check(std::strstr(session.HomeStateLine().CStr(), "\"ps\"") == nullptr && source.IsLoadRepairPending(),
          "queue acceptance cannot claim verified repair completion");
    std::filesystem::remove_all(path + ".bak");
    modules::SettingsStoreConfig config; config.path = path.c_str();
    modules::SettingsStore writer(config);
    Check(!deserializeJson(writer.Doc(), queued), "worker snapshot contains valid JSON");
    Check(writer.SaveNowBlocking(), "bounded recovery saves once obstruction disappears");
    source.ReportAsyncSaveSuccess(writer.GetLastSaveSizeBytes(), writer.GetPersistedSaveTotal());
    Check(!source.IsLoadRepairPending() &&
              source.GetLastError() == modules::SettingsStoreError::None &&
              std::strstr(session.HomeStateLine().CStr(), "\"ps\"") == nullptr,
          "verified worker completion clears the repair warning");
    modules::SettingsStore reloaded(config);
    Check(reloaded.Load() && reloaded.ReadValue("BRIGHTNESS", 0U) == 6U,
          "reboot restores the recovered preference");
    Opening(session);
    Move(session);
    std::printf("[PLAYBACK] boot-repair input=valid-tmp/blocked-backup mode=playable settings=retained queue=warning-retained verified=warning-clear reboot=restored next=e2e4/next-turn\n");
}
void RuntimeWriteFixture(const std::string& path) {
    Write(path,"{}");
    test_support::BoundedHintSession sFixture(Config(),path.c_str());
    auto& s = sFixture.session; Opening(s);
    auto& source=s.SettingsForPersistenceTest();
    modules::SettingsStoreConfig cfg; cfg.path=path.c_str();
    modules::SettingsStore writer(cfg);
    bool accept=false; std::string queued;
    source.SetAsyncSaveRequestCallback([&](const JsonDocument& doc) {
        if (!accept) return false;
        queued.clear(); serializeJson(doc,queued); return true;
    });
    const uint32_t errorsBeforeRejectedSave=source.GetErrorCount();
    Check(!source.SaveNow(),"queue rejection reproduced");
    accept=true; Check(source.SaveNow(),"retry enqueued");
    Check(std::strstr(s.HomeStateLine().CStr(),"\"ps\"")==nullptr &&
        source.GetLastError()==modules::SettingsStoreError::AdmissionRejected &&
        source.GetErrorCount()==errorsBeforeRejectedSave+1U && !source.IsDisabled() &&
        !source.HasTerminalSaveFailure() && !source.HasConfirmedStorageFailure(),
        "accepted retry retains admission refusal evidence without claiming confirmed flash failure");
    deserializeJson(writer.Doc(),queued);
    std::filesystem::create_directory(path+".tmp"); Write(path+".tmp/block","occupied");
    Check(!writer.SaveNowBlocking(),"real write failure injected");
    source.ReportAsyncSaveFailure(writer.GetLastError(), writer.HasConfirmedStorageFailure()
        ? fxe::SettingsStore::StorageFailureEvidence::Confirmed
        : fxe::SettingsStore::StorageFailureEvidence::Unconfirmed);
    Check(std::strstr(s.HomeStateLine().CStr(),"\"ps\"")==nullptr,"obstructed open path is not proof of flash failure");
    std::filesystem::remove_all(path+".tmp");
    writer.SetStorageAccessCallbacks(nullptr,[&]() {
        if(std::filesystem::is_regular_file(path+".tmp")) Write(path+".tmp","invalid");
    });
    Check(!writer.SaveNowBlocking(),"real readback verification failure injected");
    source.ReportAsyncSaveFailure(writer.GetLastError(), writer.HasConfirmedStorageFailure()
        ? fxe::SettingsStore::StorageFailureEvidence::Confirmed
        : fxe::SettingsStore::StorageFailureEvidence::Unconfirmed);
    Check(writer.GetLastError()==modules::SettingsStoreError::VerifyFailed &&
        writer.HasConfirmedStorageFailure() && source.HasConfirmedStorageFailure() && source.HasTerminalSaveFailure(),
        "real failed readback confirms storage failure for the persistence owner");
    writer.SetStorageAccessCallbacks(nullptr,nullptr);
    Check(writer.SaveNowBlocking(),"retry verifies actual saved JSON");
    source.ReportAsyncSaveSuccess(writer.GetLastSaveSizeBytes(),writer.GetPersistedSaveTotal());
    Check(std::strstr(s.HomeStateLine().CStr(),"\"ps\"")==nullptr &&
        source.GetLastError()==modules::SettingsStoreError::None &&
        !source.HasConfirmedStorageFailure() && !source.HasTerminalSaveFailure(),
        "verified success clears confirmed failure and production ps");
    Move(s);
    std::printf("[PLAYBACK] runtime-write input=queue-refusal/write-failure/readback-failure mode=playable queue/path=unsaved-without-fault readback=ps-i verified=ps-clear next=e2e4/next-turn\n");
}
}
int main() {
    char directory[]="/tmp/litch-health-XXXXXX";
    Check(mkdtemp(directory)!=nullptr,"temporary fixture directory created");
    const std::string root=directory;
    BootFixture(root+"/q.json","{\"GAME_VARIANT_V1\":\"invalid\",\"LED_BROWNOUT_LIMIT_LATCH\":true,\"BRIGHTNESS\":4}",'q');
    BootFixture(root+"/i.json","{invalid",'n');
    RuntimeWriteFixture(root+"/runtime.json");
    BootRepairFixture(root+"/repair.json");
    const auto normal=modules::led_render_policy::MakePowerBudgetPolicyConfig({true,false});
    Check(normal.maxTotalCurrentMa==modules::S3_NORMAL_TOTAL_CURRENT_MA,
          "unreadable settings alone do not reduce power without a brownout latch");
    {
        fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(0U);
        modules::SettingsStore settings;
        settings.DisablePersistenceForSession();
        firmware::SleepController sleep;
        sleep.Update(settings, 0U, firmware::SleepSuppression::Allow);
        fxe::ManualMillis::AdvanceMs(3600000U);
        sleep.Update(settings, fxe::Millis(), firmware::SleepSuppression::Allow);
        Check(sleep.GetState() == firmware::SleepState::Awake,
              "session-only state prevents automatic idle sleep without blocking explicit control");
        Check(sleep.RequestSleep(firmware::SleepRequestOrigin::Idle, fxe::Millis(), true),
              "prior idle countdown is a real accepted request");
        sleep.Update(settings, fxe::Millis(), firmware::SleepSuppression::Allow);
        Check(sleep.GetState() == firmware::SleepState::Awake,
              "save failure cancels a pending automatic countdown");
        for (const auto origin : {firmware::SleepRequestOrigin::Menu, firmware::SleepRequestOrigin::Console}) {
            Check(sleep.RequestSleep(origin, fxe::Millis(), false) &&
                      sleep.GetState() == firmware::SleepState::Countdown,
                  "explicit RAM-only power-off retains the visible cue");
            fxe::ManualMillis::AdvanceMs(3500U);
            sleep.Update(settings, fxe::Millis(), firmware::SleepSuppression::Allow);
            Check(sleep.GetState() == firmware::SleepState::Preparing,
                  "storage failure allows explicit preparation after the common cue and fade");
            sleep.NoteHigherPriorityState();
        }
    }
    std::filesystem::remove_all(root);
    return failures?1:0;
}
