#include <cstdio>
#include <cstdlib>
#include "firmware/factory_boot_intent.hpp"
#include "firmware/factory_completion_reboot.hpp"
#include "firmware/hardware_test_mode.hpp"
#include "hardware/hardware_test_boot_entry.hpp"
#include "modules/factory_provisioning_policy.hpp"
#include "virtual_litch_session.hpp"

// Reuse the existing controlled SDK boundary with a unique translation-unit
// owner. Actual coordinator/transport and nonce protocol bodies are unchanged.
#define FactoryRadioFixtureBle FactoryMinimalFixtureBle
#define FactoryRadioFixtureCoordinator FactoryMinimalFixtureCoordinator
#include "factory_radio_runtime_fixture.hpp"
#undef FactoryRadioFixtureCoordinator
#undef FactoryRadioFixtureBle

namespace {
using Mode = firmware::HardwareTestMode;
using Stage = modules::ManufacturingTestStage;
using Button = runtime::VirtualLitchPhysicalButton;
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::printf("[FAIL] %s\n", message); }
}
virtual_litch_config Config(bool newer = false) {
    virtual_litch_config result{};
    virtual_litch_default_config(&result);
    result.hardware = newer ? VIRTUAL_LITCH_HARDWARE_V3_3 : VIRTUAL_LITCH_HARDWARE_V3;
    return result;
}
struct Board {
    runtime::VirtualLitchSession session;
    explicit Board(bool newer = false)
        : session(Config(newer), runtime::VirtualLitchBootProfile::Factory) {
        Check(!session.HasNormalRuntimeOwners(), "factory boot never constructs engine/game/dispatcher/control owners");
        session.RunManufacturingIngress([](void*, Mode& mode, uint32_t now) {
            mode.ReportLightSensorLinks(true, true, now);
            mode.ReportMotionPresence(true, now);
        }, nullptr);
    }
    uint32_t Now() const { return session.PhysicalFrame().now_ms; }
    const Mode& Test() const { return session.ManufacturingTest(); }
    void Advance(uint32_t elapsed) {
        Check(session.Advance(elapsed) == VIRTUAL_LITCH_OK, "factory clock advances through existing owner");
    }
    void Tap(uint8_t bank, Button button) {
        Check(session.SetButtonDown(bank, button, true) == VIRTUAL_LITCH_OK, "raw physical press accepted");
        Advance(25U); Advance(40U);
        Check(session.SetButtonDown(bank, button, false) == VIRTUAL_LITCH_OK, "raw physical release accepted");
        Advance(25U);
    }
    void Local() {
        for (unsigned n = 0; n < 4U; ++n) Tap(1U, Button::O);
        Check(Test().GetStep() == Mode::Step::HallSensors, "R/G/B/W fields lead to Hall review");
        session.ObserveHallMask(UINT64_MAX); session.ObserveHallMask(0ULL);
        Tap(1U, Button::O);
        for (uint8_t bank : {1U, 2U})
            for (auto button : {Button::Up, Button::Down, Button::Left, Button::Right, Button::X, Button::O})
                Tap(bank, button);
        for (uint32_t lux : {400U, 100U}) {
            modules::ManufacturingLightSample sample{3U, {lux, lux}};
            for (uint8_t count = 0U; count < 3U; ++count) {
                session.RunManufacturingIngress([](void* context, Mode& mode, uint32_t now) {
                    mode.ObserveLightSample(*static_cast<modules::ManufacturingLightSample*>(context), now);
                }, &sample);
            }
        }
        session.ObserveHallMask(0ULL);
        Check(Test().GetStage() == Stage::Radio, "local control release and both light responses admit Radio with current empty Hall");
    }
    void Proof() {
        session.RunManufacturingIngress([](void*, Mode& mode, uint32_t now) {
            mode.ReportRadioVerified(modules::FactoryRadioChannel::Ble, now);
        }, nullptr);
    }
    void Save(bool durable) {
        session.RunManufacturingIngress([](void* context, Mode& mode, uint32_t) {
            mode.ReportPersistenceResult(*static_cast<bool*>(context));
        }, &durable);
    }
    void Capture(const char* action, const char* next) const {
        const auto progress = Test().GetProgress();
        const auto& frame = session.PhysicalFrame();
        std::printf("[factory_minimal_boot_two_board_completion_journey] t=%u revision=%u action=%s normal_owners=%u stage=%u step=%u buttons=%04x held=%04x hall_clear=%016llx hall_active=%016llx fresh_empty=%u radio=%u leds=%u next=%s\n",
            Now(), unsigned(Test().GetRevision()), action, session.HasNormalRuntimeOwners(),
            unsigned(Test().GetStage()), unsigned(Test().GetStep()), progress.observedButtonMask,
            progress.pressedButtonMask, (unsigned long long)progress.hallClearMask,
            (unsigned long long)progress.hallActiveMask, progress.hallSnapshotValid && progress.hallOccupiedMask == 0ULL,
            progress.radioChannelMask, session.PhysicalFrame().led_count, next);
        if (const char* path = std::getenv("LITCH_FACTORY_MINIMAL_PLAYBACK_PATH")) {
            if (FILE* out = std::fopen(path, "a")) {
                std::fprintf(out, "{\"journey\":\"factory_minimal_boot_two_board_completion_journey\",\"action\":\"%s\",\"next\":\"%s\",\"time_ms\":%u,\"revision\":%u,\"normal_owners\":%u,\"stage\":%u,\"step\":%u,\"buttons\":%u,\"held\":%u,\"radios\":%u,\"limited\":%u,\"pixels\":[",
                    action, next, Now(), unsigned(Test().GetRevision()), session.HasNormalRuntimeOwners(),
                    unsigned(Test().GetStage()), unsigned(Test().GetStep()), progress.observedButtonMask,
                    progress.pressedButtonMask, progress.radioChannelMask, frame.power_limited);
                for (unsigned n = 0U; n < frame.led_count; ++n) {
                    const auto pixel = frame.pixels[n];
                    std::fprintf(out, "%s[%u,%u,%u,%u]", n ? "," : "", pixel.r, pixel.g, pixel.b, pixel.w);
                }
                std::fprintf(out, "]}\n");
                std::fclose(out);
            }
        }

    }
};
void BootSelection() {
    using firmware::FactoryBootIntent;
    firmware::FactoryBootRequest request;
    Check(sizeof(request) == 8U, "RTC request is fixed eight bytes");
    for (auto intent : {FactoryBootIntent::Test, FactoryBootIntent::Verifier}) {
        firmware::ArmFactoryBootRequest(request, intent);
        Check(firmware::ConsumeFactoryBootRequest(request, true, false, true) == intent,
              "exact software reset consumes one requested profile");
        Check(firmware::ConsumeFactoryBootRequest(request, true, false, true) == FactoryBootIntent::None,
              "request consumed once; later reset cannot inherit it");
        for (unsigned edge = 0U; edge < 3U; ++edge) {
            firmware::ArmFactoryBootRequest(request, intent);
            Check(firmware::ConsumeFactoryBootRequest(request, edge != 0U, edge == 1U, edge != 2U) == FactoryBootIntent::None,
                  "power loss/recovery/unsupported identity discard request");
        }
        firmware::ArmFactoryBootRequest(request, intent); request.complement ^= 1U;
        Check(firmware::ConsumeFactoryBootRequest(request, true, false, true) == FactoryBootIntent::None,
              "corrupt RTC request fails closed");
    }
    Check(firmware::FactoryBootProfileFor(false, false) == firmware::FactoryBootProfile::Normal,
          "normal boot cannot infer a request from historical verifier bytes");
    Check(firmware::FactoryBootProfileFor(true, true) == firmware::FactoryBootProfile::Factory,
          "Required test takes precedence over requested verifier service");
    runtime::VirtualLitchSession normal(Config());
    Check(normal.HasNormalRuntimeOwners(), "ordinary boot retains normal engine/game/control initialization");
    Mode saved;
    modules::HardwareTestRecord record;
    record.revision = hardware::BoardRevision::V3S3;
    record.radioResult = modules::ManufacturingTestRadioResult::Verified;
    record.radioChannelMask = modules::FACTORY_RADIO_ALL_CHANNELS;
    Check(!saved.RestoreSavedVerifier(record, 0U), "disabled saved service cannot masquerade as requested verifier");
    record.factoryVerifierEnabled = true;
    Check(saved.RestoreSavedVerifier(record, 0U) && !saved.IsActive() && saved.GetStep() == Mode::Step::Passed,
          "eligible saved projection preserves PASS without entering/fabricating new local qualification");
    Mode legacyBypassService;
    modules::HardwareTestRecord legacyBypass;
    Check(modules::DecodeHardwareTestRecord("1|3|2|1|0|1780|7200|840|D4D636E6", legacyBypass) &&
              !legacyBypassService.RestoreSavedVerifier(legacyBypass, 0U) &&
              legacyBypassService.GetStep() != Mode::Step::Passed,
          "actual legacy bypass record cannot restore a false green saved-verifier projection");
    modules::HardwareTestRecord pending;
    Check(!saved.BuildPendingRecord(pending), "saved projection cannot create a new qualification record");
}
void PassedUpReentry() {
    struct Store {
        modules::FactoryProvisioningState state = modules::FactoryProvisioningState::Passed;
        unsigned writes = 0U;
    };
    for (const bool newer : {false, true}) for (const uint8_t bank : {1U, 2U}) {
        Store store;
        modules::FactoryProvisioningPolicy provisioning;
        const auto revision = newer ? hardware::BoardRevision::V3_3S3 : hardware::BoardRevision::V3S3;
        provisioning.Begin(revision, true, true,
            [](void* context) { return static_cast<Store*>(context)->state; },
            [](void* context, modules::FactoryProvisioningState state) {
                auto& value = *static_cast<Store*>(context);
                ++value.writes; value.state = state; return true;
            }, &store);
        Check(!provisioning.ShouldAutoEnterHardwareTest(), "saved PASS still selects normal boot without held Up");
        const bool requested = hardware::ShouldEnterHardwareTestFromBoot(revision, true, bank == 1U, bank == 2U) &&
            provisioning.AllowsPhysicalUpEntry();
        Check(requested, "either physical Up during boot admits a fresh test after saved PASS");
        if (!requested) continue;
        Check(firmware::FactoryBootProfileFor(requested, true) == firmware::FactoryBootProfile::Factory,
              "fresh held-Up test takes precedence over any saved-verifier request");
        Board board(newer);
        const auto initial = board.Test().GetProgress();
        Check(board.Test().GetStage() == Stage::Local && board.Test().GetStep() == Mode::Step::LedRainbow &&
                  initial.observedButtonMask == 0U && initial.radioChannelMask == 0U,
              "held-Up boot starts at red without old controls, radio proof or green acceptance");
        board.Capture(bank == 1U ? "saved_PASS_P1_Up_fresh_red" : "saved_PASS_P2_Up_fresh_red",
                      "inspect R/G/B/W; historical PASS is preserved, not current qualification");
        board.Local();
        modules::HardwareTestRecord pending;
        Check(board.Test().GetStage() == Stage::Radio && !board.Test().BuildPendingRecord(pending),
              "fresh local checks after held Up cannot save without new actual peer proof");
        board.Capture("saved_PASS_retest_waits_for_new_BLE",
                      "wait for fresh peer proof or reboot with Up released to keep historical PASS");
        Check(store.state == modules::FactoryProvisioningState::Passed && store.writes == 0U,
              "admission and unsaved retest leave historical provisioning unchanged");
    }
}
void DelayedAcknowledgement() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    firmware::FactoryCompletionReboot completion;
    completion.NoteFrameAcknowledged(true);
    Check(!completion.IsReady(false, true), "failed save/NVS/origin cannot acknowledge completion that was never begun");
    completion.Begin();
    fxe::ManualMillis::AdvanceMs(1500U); // Slow owners/render before the actual ACK.
    completion.NoteFrameAcknowledged(false);
    Check(!completion.IsAcknowledged() && !completion.IsReady(false, true), "denied frame cannot start result dwell");
    completion.NoteFrameAcknowledged(true);
    Check(completion.AcknowledgedAtMs() == 1600U && !completion.IsReady(false, true),
          "delayed Completed ACK starts1s at post-transfer clock; no same-iteration stale-clock wrap");
    fxe::ManualMillis::AdvanceMs(999U);
    Check(!completion.IsReady(false, true), "999ms after actual ACK retains green");
    completion.NoteFrameAcknowledged(true);
    Check(completion.AcknowledgedAtMs() == 1600U, "later accepted frames cannot move the originating ACK deadline");
    fxe::ManualMillis::AdvanceMs(1U);
    Check(completion.IsReady(false, true), "1000ms after actual ACK permits reboot eligibility");
    Check(!completion.IsReady(true, true) && !completion.IsReady(false, false),
          "live storage/terminal/OTA operation or unconfirmed OTA health retains completion");
    completion.Cancel();
    Check(!completion.IsReady(false, true), "accepted reboot/cancel consumes completion once");
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 500U);
    completion.Begin(); completion.NoteFrameAcknowledged(true);
    fxe::ManualMillis::AdvanceMs(1000U);
    Check(completion.IsReady(false, true), "post-ACK interval remains correct across timer wrap");
    std::puts("[FACTORY_COMPLETION_ACK] pre_ACK_work=1500ms ACK=1600ms no_reboot_at_ACK/+999 eligible_at_+1000; denied/save_authority/operation/health/cancel/wrap guards retained");
}
void TwoBoardCompletion() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    Board first, late(true);
    first.Capture("Required_boot", "inspect R/G/B/W fields");
    first.Local();
    Check(!modules::IsFactoryRadioStageReady(late.Test().GetStage()), "late local review cannot advertise proof eligibility");
    first.Capture("local_ready_peer_still_reviewing", "keep peer powered; finish its local checks");
    late.Local(); late.Capture("late_peer_now_locally_ready", "BLE nonce proof");
    modules::FactoryMinimalFixtureBle ble;
    firmware::FactoryMinimalFixtureCoordinator server;
    const uint32_t protocolStart = first.Now();
    const auto synchronizeBoards = [&](uint32_t now) {
        first.Advance(now - first.Now());
        late.Advance(now - late.Now());
        fxe::ManualMillis::SetNowMs(now);
    };
    auto update = [&](uint32_t now) {
        synchronizeBoards(now);
        server.Update(true, modules::IsFactoryRadioStageReady(first.Test().GetStage()),
            first.Test().GetStage(), ble, now, first.Test().GetRunId());
    };
    update(protocolStart); update(protocolStart + 1500U);
    Check(server.IsAutomaticVerifier(), "actual coordinator elects a locally-ready responder");
    first.session.RunManufacturingIngress([](void*, Mode& mode, uint32_t now) {
        mode.SetAutomaticVerifier(true, now);
    }, nullptr);
    // BLE SDK connection/attribute delivery is controlled; both nonce state
    // machines are the actual production protocol, not an injected PASS.
    modules::FactoryRadioDutHandshake bleDut;
    modules::FactoryRadioVerifierHandshake bleServer;
    bleServer.SetEligible(true);
    modules::FactoryRadioFrame hello, challenge, response, terminal, unused;
    Check(bleDut.Start(modules::FactoryRadioChannel::Ble, 600U, 700U, hello) &&
          bleServer.Handle(hello, 800U, challenge) && bleDut.Handle(challenge, response) &&
          bleServer.Handle(response, 800U, terminal) && !bleDut.IsVerified(),
          "actual BLE responder proves bidirectional exchange while dropped terminal keeps DUT unverified");
    Check(first.session.SetButtonDown(1U, Button::X, true) == VIRTUAL_LITCH_OK,
          "raw Radio X hold arrives before completed peer proof");
    first.Advance(25U); first.Advance(40U);
    first.Capture("Radio_X_held_before_protocol_completion", "keep hold; arriving proof cannot save or navigate");
    ble.proof = {600U, 700U, 800U}; ble.serverProof = true;
    update(protocolStart + 1600U);
    const bool responderProved = server.ConsumeVerifierBleVerified();
    Check(responderProved && server.HasPendingTerminalReply(),
          "responder earns its own BLE proof but retains terminal retry ownership");
    if (responderProved) first.Proof();
    Check(first.Test().GetProgress().radioChannelMask == modules::FACTORY_RADIO_ALL_CHANNELS &&
          first.Test().GetProgress().pressedButtonMask != 0U && first.Test().GetStage() != Stage::Persisting,
          "actual BLE completion retains its proof bit while held X suppresses save");
    first.Capture("server_BLE_proof_terminal_dropped_X_held", "release X to return; completed BLE proof remains pending");
    Check(first.session.SetButtonDown(1U, Button::X, false) == VIRTUAL_LITCH_OK,
          "fresh physical release performs Radio back action");
    first.Advance(25U);
    first.session.RunManufacturingIngress([](void*, Mode& mode, uint32_t now) {
        mode.SetAutomaticVerifier(false, now); // Actual coordinator restarts discovery on X-back.
    }, nullptr);
    Check(first.Test().GetStep() == Mode::Step::Buttons &&
          first.Test().GetProgress().radioChannelMask == modules::FACTORY_RADIO_ALL_CHANNELS &&
          first.Test().GetStage() != Stage::Persisting,
          "Radio back retains genuine completed proof and revokes review before save");
    first.Capture("Radio_X_release_back_proof_retained", "fresh O review and current empty Hall required");
    first.Tap(1U, Button::O);
    Check(first.Test().GetStage() != Stage::Persisting,
          "fresh O cannot save before a current post-review empty Hall observation");
    first.Capture("fresh_O_review_waits_current_empty_Hall", "obtain current empty Hall; no new radio proof needed");
    first.session.ObserveHallMask(0ULL);
    Check(first.Test().GetStage() == Stage::Persisting &&
          first.Test().GetProgress().radioChannelMask == modules::FACTORY_RADIO_ALL_CHANNELS,
          "fresh valid empty review admits save with the retained actual protocol proof");
    modules::FactoryRadioFrame retriedTerminal;
    Check(bleServer.Handle(response, 800U, retriedTerminal) &&
          retriedTerminal.type == modules::FactoryRadioFrameType::Verified &&
          bleDut.Handle(retriedTerminal, unused) && bleDut.IsVerified(),
          "duplicate BLE response obtains the retained real terminal during retry grace");
    late.Proof();
    late.session.ObserveHallMask(0ULL);
    late.Capture("DUT_terminal_retry_qualified", "await each board's tracked durable result");
    Check(first.Test().GetStage() == Stage::Persisting && late.Test().GetStage() == Stage::Persisting,
          "both independent local+BLE proofs still await persistence");
    modules::HardwareTestRecord retainedServer;
    Check(first.Test().BuildPendingRecord(retainedServer) &&
              retainedServer.radioResult == modules::ManufacturingTestRadioResult::PeerVerified &&
              first.Test().IsAutomaticVerifier(),
          "genuine responder identity survives X-back/review after discovery reset");
    Check(server.HasPendingTerminalReply(), "neither early save nor reboot may tear down remaining terminal grace");
    synchronizeBoards(protocolStart + 2600U);
    server.PollTerminalReply(ble, protocolStart + 2600U);
    Check(!server.HasPendingTerminalReply(), "existing one-second grace completes before radio release/settings worker");
    Check(first.session.PhysicalFrame().pixels[27U * 4U].g == 0U &&
          late.session.PhysicalFrame().pixels[27U * 4U].g == 0U,
          "pending tracked results keep the inner radio square dark; no green before save");
    first.Save(true); late.Save(true);
    Check(first.session.PhysicalFrame().pixels[27U * 4U].g != 0U &&
          late.session.PhysicalFrame().pixels[27U * 4U].g != 0U,
          "real post-budget accepted frames fill the central square green after each durable acknowledgement");
    Check(first.Test().GetStage() == Stage::Passed && late.Test().GetStage() == Stage::Passed,
          "controlled durable acknowledgements independently qualify both boards");
    first.Capture("durable_saved_green", "retain this proven responder; DUT reboots after readable interval");
    late.Capture("durable_saved_green", "acknowledge frame; normal reboot after readable interval");
    update(protocolStart + 2610U);
    server.Update(false, false, Stage::Inactive, ble, protocolStart + 2611U);
    first.session.RunManufacturingIngress([](void* context, Mode& mode, uint32_t now) {
        Check(mode.RestoreSavedVerifier(*static_cast<modules::HardwareTestRecord*>(context), now),
              "actual saved mode admits durable genuine responder");
        mode.ReportVerifierProofComplete(now);
    }, &retainedServer);
    ble.SetFactoryVerifierConfiguredEnabled(true); // Runtime re-arms only after adopted receipt/NVS Passed (lifecycle suite).
    server.Update(false, true, Stage::Passed, ble, protocolStart + 2611U);
    firmware::FactoryCompletionReboot firstCompletion, lateCompletion;
    lateCompletion.Begin(); // Only the DUT's originating durable result begins normal reboot.
    synchronizeBoards(first.Now() + 1500U);
    firstCompletion.NoteFrameAcknowledged(true); lateCompletion.NoteFrameAcknowledged(true);
    Check(!firstCompletion.IsReady(false, true) && !lateCompletion.IsReady(false, true),
          "slow actual frame iteration cannot spend the post-ACK result interval");
    first.Capture("completed_green_frame_ACK", "retain result for1000ms after this completed frame");
    late.Capture("completed_green_frame_ACK", "retain result for1000ms after this completed frame");
    synchronizeBoards(first.Now() + 999U);
    Check(!firstCompletion.IsReady(false, true) && !lateCompletion.IsReady(false, true),
          "both boards remain factory at ACK+999ms");
    first.Capture("ACK_plus999_no_reboot", "keep green;1ms of acknowledged interval remains");
    synchronizeBoards(first.Now() + 1U);
    Check(!firstCompletion.IsReady(false, true) && lateCompletion.IsReady(false, true) && ble.allowed,
          "DUT requests normal reboot at ACK+1000ms while proven responder keeps serving");
    first.Capture("ACK_plus1000_verifier_still_serving", "leave verifier powered; qualify next DUT");
    firstCompletion.Cancel(); lateCompletion.Cancel();
    Check(firmware::FactoryBootProfileFor(false, false) == firmware::FactoryBootProfile::Normal,
          "completion chooses ordinary next boot without explicit verifier request");
    runtime::VirtualLitchSession normalAfterReset(Config());
    Check(normalAfterReset.HasNormalRuntimeOwners(), "controlled next normal boot restores normal runtime owners");
    std::printf("[factory_minimal_boot_two_board_completion_journey] next_boot_time=0 profile=normal normal_owners=%u reset_delivery=controlled no_factory_auto_request=1\n",
        normalAfterReset.HasNormalRuntimeOwners());
    synchronizeBoards(first.Now() + 5000U);
    server.Update(false, true, Stage::Passed, ble, first.Now());
    Check(ble.allowed && !first.session.HasNormalRuntimeOwners() && !first.Test().IsActive() &&
              !firstCompletion.IsReady(false, true),
          "five seconds after DUT normal reset the saved responder remains minimal and available");
    first.Capture("DUT_normal_reset_verifier_retained", "later DUT begins local qualification");
    Board laterDut;
    laterDut.Local();
    modules::FactoryRadioDutHandshake laterHandshake;
    modules::FactoryRadioFrame laterHello, laterChallenge, laterResponse, laterTerminal;
    Check(laterHandshake.Start(modules::FactoryRadioChannel::Ble, 900U, 1000U, laterHello) &&
              bleServer.Handle(laterHello, 1100U, laterChallenge) &&
              laterHandshake.Handle(laterChallenge, laterResponse) &&
              bleServer.Handle(laterResponse, 1100U, laterTerminal) &&
              laterHandshake.Handle(laterTerminal, unused) && laterHandshake.IsVerified(),
          "later DUT completes a fresh actual BLE nonce protocol against retained responder");
    ble.proof = {900U, 1000U, 1100U}; ble.serverProof = true;
    server.Update(false, true, Stage::Passed, ble, first.Now());
    Check(server.ConsumeVerifierBleVerified() && server.HasPendingTerminalReply(),
          "inactive retained responder accepts the later real proof and owns its terminal grace");
    laterDut.Proof(); laterDut.session.ObserveHallMask(0ULL); laterDut.Save(true);
    Check(laterDut.Test().GetStage() == Stage::Passed,
          "later independently reviewed DUT qualifies only after its own fresh Hall/proof/durable result");
    first.Advance(1000U);
    server.PollTerminalReply(ble, first.Now());
    server.Update(false, true, Stage::Passed, ble, first.Now());
    Check(ble.allowed && !server.HasPendingTerminalReply(),
          "terminal grace expires and retained verifier resumes serving another board");
    first.Capture("later_DUT_verified_service_rearmed", "continue batch or use hwtest verifier off for normal boot");
    Board failed;
    failed.Local(); failed.Proof(); failed.session.ObserveHallMask(0ULL); failed.Save(false);
    Check(failed.Test().GetStage() != Stage::Passed && !failed.session.HasNormalRuntimeOwners(),
          "failed durable result retains factory recovery and cannot initialize normal owners");
    failed.Capture("save_failed", "inspect storage; restart factory boot");
    failed.session.RunManufacturingIngress([](void*, Mode& mode, uint32_t) { mode.Stop(); }, nullptr);
    failed.Advance(2000U);
    Check(!failed.session.HasNormalRuntimeOwners(), "engineering Stop and later input/ticks never fall through to game initialization");
    std::puts("[LIMITS] Arduino startup branches have supplemental exact-source guards; GPIO/I2C observations, BLE SDK, storage receipt/NVS and reset delivery are controlled boundaries. Actual allocation, task startup, OTA delivery and RF require frozen S3/HIL gates.");
}
} // namespace
int main() {
    BootSelection(); PassedUpReentry(); DelayedAcknowledgement(); TwoBoardCompletion();
    std::printf("[FACTORY_MINIMAL_BOOT] failures=%d\n", failures);
    return failures ? 1 : 0;
}
