#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include "firmware/hardware_test_mode.hpp"
#include "firmware/manufacturing_test_led_frame.hpp"
#include "firmware/motion_wake_controller.hpp"
#include "modules/factory_radio_role_gate.hpp"
#include "modules/hardware_test_record.hpp"
#include "virtual_litch_session.hpp"
#include "../../src/virtual_litch_session_internal.hpp"
#include "modules/led_render_policy.hpp"

namespace {
using Mode = firmware::HardwareTestMode;
using Stage = modules::ManufacturingTestStage;
using Button = runtime::VirtualLitchPhysicalButton;
int failures = 0;
void Check(bool ok, const char* what) { if (!ok) { ++failures; std::printf("[FAIL] %s\n", what); } }
virtual_litch_config Config(hardware::BoardRevision revision, bool rotate) {
    virtual_litch_config c{}; virtual_litch_default_config(&c);
    c.hardware = revision == hardware::BoardRevision::V3_3S3 ? VIRTUAL_LITCH_HARDWARE_V3_3 : VIRTUAL_LITCH_HARDWARE_S3;
    c.board_rotate = rotate; c.square_rotate_for_output = rotate; return c;
}
struct Board {
    hardware::BoardRevision revision;
    bool rotated = false;
    std::string input = "power_on";
    std::string requestInput;
    const char* requestOutcome = nullptr;
    uint16_t rawHeld = 0U;
    runtime::VirtualLitchSession session;
    modules::FactoryRadioRoleGate role;
    explicit Board(hardware::BoardRevision r = hardware::BoardRevision::V3S3, bool rotate = false, uint32_t jitter = 1500U)
        : revision(r), rotated(rotate), session(Config(r, rotate)) {
        session.RunManufacturingIngress([](void* context, Mode& m, uint32_t now) {
            m.Enter(*static_cast<hardware::BoardRevision*>(context), now);
            m.ReportLightSensorLinks(true, true, now);
            if (hardware::HasFourLedSquares(m.GetRevision()))
                m.ReportAutomatedCheck(modules::ManufacturingTestLocalCheck::MotionSensor, true, now);
        }, &revision);
        role.Begin(Now(), jitter);
    }
    uint32_t Now() const { return session.PhysicalFrame().now_ms; }
    const Mode& Test() const { return session.ManufacturingTest(); }
    void Advance(uint32_t ms) {
        Check(session.Advance(ms) == VIRTUAL_LITCH_OK, "production virtual clock advances");
        role.Tick(Now()); bool verifier = role.IsVerifier();
        session.RunManufacturingIngress([](void* p, Mode& m, uint32_t now) { m.SetAutomaticVerifier(*static_cast<bool*>(p), now); }, &verifier);
    }
    void Down(uint8_t bank, Button button, bool down) {
        constexpr const char* BUTTON_NAMES[] = {"Up","Down","Left","Right","Press","X","O"};
        input = "bank" + std::to_string(bank) + "_" + BUTTON_NAMES[unsigned(button)] + (down ? "_down" : "_release");
        const uint16_t bit = 1U << ((bank - 1U) * 7U + unsigned(button));
        if (down) rawHeld |= bit; else rawHeld &= static_cast<uint16_t>(~bit);
        Check(session.SetButtonDown(bank, button, down) == VIRTUAL_LITCH_OK, "production raw button ingress accepts electrical level");
        Advance(25U); if (down) Advance(40U);
    }
    void Tap(uint8_t bank, Button button) { Down(bank, button, true); Down(bank, button, false); }
    void Observe(uint64_t mask) { input = "Hall_snapshot"; Check(session.ObserveHallMask(mask) == VIRTUAL_LITCH_OK, "production board observation accepted"); }
    void SampleLights(uint32_t lux0, uint32_t lux1) {
        modules::ManufacturingLightSample sample{3U, {lux0, lux1}};
        input = "ALS_sample";
        session.RunManufacturingIngress([](void* context, Mode& mode, uint32_t now) {
            mode.ObserveLightSample(*static_cast<modules::ManufacturingLightSample*>(context), now);
        }, &sample);
    }
    void QualifyBothLightResponses() {
        for (const uint32_t lux : {400U, 100U}) {
            for (uint8_t sample = 0U; sample < 3U; ++sample) SampleLights(lux, lux);
        }
    }
    void Save(bool saved = true) {
        modules::HardwareTestRecord record;
        Check(Test().BuildPendingRecord(record), "pending record exists only after required proof");
        Check(modules::IsVerifiedFactoryPass(record), "record is verified local+BLE PASS");
        session.RunManufacturingIngress([](void* p, Mode& m, uint32_t now) {
            m.ReportPersistenceResult(*static_cast<bool*>(p));
            if (m.IsAutomaticVerifier() && m.GetStage() == Stage::Passed) m.ReportVerifierProofComplete(now);
        }, &saved);
    }
    void Local() {
        for (uint8_t channel = 0U; channel < 4U; ++channel) Tap(channel % 2U + 1U, Button::O);
        Check(Test().GetStep() == Mode::Step::HallSensors, "LED fields lead to Hall stage");
        Observe(UINT64_MAX); Observe(0ULL);
        Tap(2U, Button::O);
        Check(Test().GetStep() == Mode::Step::Buttons, "Hall stage leads to buttons");
        for (uint8_t bank : {1U, 2U}) for (Button b : {Button::Up, Button::Down, Button::Left, Button::Right, Button::X, Button::O}) Tap(bank, b);
        QualifyBothLightResponses(); Observe(0ULL);
        Check(Test().GetProgress().buttonsComplete && Test().GetProgress().hallComplete, "all twelve controls and 64 low/high inputs complete and released");
    }
    const char* QualificationResult() const {
        return Test().IsOverridePending() ? "exit_accepted_storage_pending"
            : Test().GetStage() == Stage::Passed ? "saved" : "qualification_pending";
    }
    const char* CaptureAck() const {
        return requestOutcome != nullptr && input == requestInput ? requestOutcome
            : Test().IsOverridePending() ? "exit_accepted_storage_pending"
            : Test().GetStage() == Stage::Passed ? "saved" : "input_accepted_qualification_pending";
    }
    bool NavigationDark() const {
        const auto topology=hardware::BoardLedTopologyFor(revision);
        const auto& frame=session.PhysicalFrame();bool dark=true;
        for(uint8_t square:{57U,62U,1U,6U,49U,54U,9U,14U})for(uint8_t slot=0U;slot<topology.physicalLedsPerSquare;++slot){
            const auto c=frame.pixels[square*topology.physicalLedsPerSquare+slot];dark&=!(c.r||c.g||c.b||c.w);
        }
        if(revision==hardware::BoardRevision::V3_3S3)for(uint8_t bank=0U;bank<2U;++bank)for(uint8_t role=0U;role<6U;++role){
            const auto c=frame.pixels[256U+hardware::V3_3PhysicalIndexForControl(bank==1U,static_cast<hardware::ControlLedRole>(role))];dark&=!(c.r||c.g||c.b||c.w);
        }
        return dark;
    }
    void Capture(const char* name, const char* next) const {
        if(Test().IsActive()&&!Test().GetOperatorHoldProgress()&&(Test().GetStep()==Mode::Step::Radio||Test().GetStep()==Mode::Step::Persisting||Test().GetStep()==Mode::Step::Passed))
            Check(NavigationDark(),"production Radio/persistence/saved output contains no button marks through role motion, proof and recovery");
        const auto& f = session.PhysicalFrame(); const auto p = Test().GetProgress();
        std::printf("[FACTORY_JOURNEY] %s revision=%u time=%u active=%u stage=%u step=%u channel=%u verifier=%u buttons=%04x/%04x held=%04x halls_low=%016llx high=%016llx current=%016llx valid=%u light_responses=%u radios=%u leds=%u limited=%u ack=%s qualification=%s next=%s\n",
            name, unsigned(Test().GetRevision()), Now(), Test().IsActive(), unsigned(Test().GetStage()), unsigned(Test().GetStep()), Test().GetLedChannel(), Test().IsAutomaticVerifier(), p.observedButtonMask, p.expectedButtonMask, p.pressedButtonMask,
            (unsigned long long)p.hallActiveMask, (unsigned long long)p.hallClearMask, (unsigned long long)p.hallOccupiedMask, p.hallSnapshotValid, p.lightSensorResponseMask, p.radioChannelMask, f.led_count, f.power_limited, CaptureAck(), QualificationResult(), next);
        if (const char* path = std::getenv("LITCH_FACTORY_PLAYBACK_PATH")) {
            if (FILE* out = std::fopen(path, "a")) {
                std::fprintf(out, "{\"journey\":\"%s\",\"revision\":%u,\"time_ms\":%u,\"active\":%u,\"stage\":%u,\"step\":%u,\"channel\":%u,\"verifier\":%u,\"buttons\":%u,\"held\":%u,\"hall_low\":\"%016llx\",\"hall_high\":\"%016llx\",\"current\":\"%016llx\",\"hall_valid\":%u,\"radios\":%u,\"next\":\"%s\",\"display\":\"\",\"limited\":%u,\"pixels\":[",
                    name, unsigned(Test().GetRevision()), Now(), Test().IsActive(), unsigned(Test().GetStage()), unsigned(Test().GetStep()), Test().GetLedChannel(), Test().IsAutomaticVerifier(), p.observedButtonMask, p.pressedButtonMask,
                    (unsigned long long)p.hallActiveMask, (unsigned long long)p.hallClearMask, (unsigned long long)p.hallOccupiedMask, p.hallSnapshotValid, p.radioChannelMask, next, f.power_limited);
                for (uint16_t i = 0U; i < f.led_count; ++i) { const auto c = f.pixels[i]; std::fprintf(out, "%s[%u,%u,%u,%u]", i ? "," : "", c.r, c.g, c.b, c.w); }
                constexpr const char* STAGES[] = {"inactive","local_qualification","radio_proof","persisting","saved_PASS","historical_bypass","failed","saved_Override"};
                const char* semantics = !Test().IsActive() ? "ordinary_render; no_factory_PASS"
                    : Test().GetStep()==Mode::Step::LedRainbow ? "operator_owned_exact_LED_field"
                    : Test().GetOperatorHoldProgress() ? "same_bank_hold_progress"
                    : Test().GetStep() == Mode::Step::HallSensors ? "Hall_history; occupied_complete_square_moving_dark_slot"
                    : Test().GetStep() == Mode::Step::Buttons ? "untested_directions_on; tested_directions_off; X/O_orange_pending_green_observed; untested_X/O_extra_square; held_pair_brighter; A1_H8_ambient_pending_amber_pass_green; fixed32_quiet_except_fault_chord"
                    : Test().GetStep() == Mode::Step::Radio ? "pending_central4x4_amber_outline; DUT_breathes_verifier_clockwise_white; no_X/O_or_dedicated_control_marks; green_requires_saved_PASS"
                    : Test().GetStep() == Mode::Step::Passed ? "saved_DUT_filled_green4x4_or_verifier_white_boundary_spin"
                    : "local_inspection_or_recovery";
                std::fprintf(out, "],\"motion_present\":%u,\"motion_known\":%u,\"expander_links\":%u,\"forced_faults\":%u,\"failed_checks\":%u,\"light_responses\":%u,\"lux_driver0\":%u,\"lux_driver1\":%u", p.motionPresent,p.motionPresenceKnown,p.expanderLinkMask,p.diagnosticFaultMask,p.failedAutomatedCheckMask,p.lightSensorResponseMask,p.lightSensors[0].lastLux,p.lightSensors[1].lastLux);
                std::fprintf(out, ",\"qualification_result\":\"%s\",\"fault_ingress\":\"%s\",\"console_args_grammar\":\"%s\"", QualificationResult(),
                    requestOutcome != nullptr && input == requestInput ? "HardwareTestMode::SetDiagnosticFault" : "not_a_fault_request",
                    requestOutcome != nullptr && input == requestInput ? "bypassed_not_executed" : "not_applicable");
                std::fprintf(out, ",\"rotated\":%u,\"raw_held\":%u,\"step_elapsed_ms\":%u,\"input\":\"%s\",\"authoritative_mode\":\"%s\",\"authoritative_stage\":\"%s\",\"ack\":\"%s\",\"semantic_leds\":\"%s\",\"expiry\":\"%s\"}\n",
                    rotated, rawHeld, Test().GetStepElapsedMs(Now()), input.c_str(), Test().IsActive() ? "factory" : "normal", STAGES[unsigned(Test().GetStage())],
                    CaptureAck(),
                    semantics, Test().GetOperatorHoldProgress() ? "cancel_on_either_release; exit_at5s"
                        : !Test().IsActive() && rawHeld ? "held_tails_suppressed_until_release"
                        : Test().GetStep() == Mode::Step::Passed && Test().IsAutomaticVerifier() ? "continuous_clockwise250ms_until_factory_exit"
                        : Test().GetStep() == Mode::Step::Passed && Test().GetVerifierFeedback() == Mode::VerifierFeedback::Success ? "terminal_feedback_deadline_then_steady_saved_green"
                        : Test().GetStep() == Mode::Step::Passed && Test().GetVerifierFeedback() == Mode::VerifierFeedback::BleProof ? "serve_peer_then_terminal_feedback; phase_continuous"
                        : Test().GetStep() == Mode::Step::Passed ? "steady_saved_green_until_serving_or_exit"
                        : Test().GetStep() == Mode::Step::Radio ? "animated_until_proof_or_local_recovery" : "no_automatic_navigation");
                std::fclose(out);
            }
        }
    }
};
void Report(Board& b, modules::FactoryRadioChannel channel) {
    b.session.RunManufacturingIngress([](void* p, Mode& m, uint32_t now) { m.ReportRadioVerified(*static_cast<modules::FactoryRadioChannel*>(p), now); }, &channel);
}
void Prove(Board& dut, Board& verifier, uint32_t sessionId) {
    const uint32_t dutNonce = sessionId + 17U, verifierNonce = sessionId + 31U;
    for (auto channel : {modules::FactoryRadioChannel::Ble}) {
        modules::FactoryRadioDutHandshake d; modules::FactoryRadioVerifierHandshake v; v.SetEligible(true);
        modules::FactoryRadioFrame hello, challenge, response, terminal, ignored;
        Check(d.Start(channel, sessionId, dutNonce, hello, verifierNonce), "real v3 protocol starts exact session/nonce");
        Check(v.Handle(hello, verifierNonce, challenge) && d.Handle(challenge, response) && v.Handle(response, verifierNonce, terminal), "real bidirectional challenge response reaches terminal");
        auto stale = terminal; ++stale.sessionId;
        Check(!d.Handle(stale, ignored) && !d.IsVerified(), "stale peer proof cannot complete fresh session");
        modules::FactoryRadioWireFrame wire{}; modules::FactoryRadioFrame decoded;
        Check(modules::EncodeFactoryRadioFrame(terminal, wire) && modules::DecodeFactoryRadioFrame(wire.data(), wire.size(), decoded) && d.Handle(decoded, ignored) && d.IsVerified(), "bounded wire terminal validates real protocol owner");
        if (channel == modules::FactoryRadioChannel::Ble) { dut.role.AcceptBleProof(); dut.Advance(0U); }
        Report(dut, channel); Report(verifier, channel);
    }
    dut.Observe(0ULL); verifier.Observe(0ULL);
}
void NavigationAndOptics(hardware::BoardRevision revision) {
    Board b(revision, true);
    b.Capture("led_red_entry", "inspect all fitted red dies; O forward X back");
    auto before = b.session.PhysicalFrame();
    b.Advance(11000U);
    Check(b.Test().GetStep() == Mode::Step::LedRainbow && b.Test().GetLedChannel() == 0U, "LED inspection never advances or times out on operator delay");
    for (uint8_t channel = 0U; channel < 4U; ++channel) {
        Check(b.Test().GetLedChannel() == channel, "O navigates named channel order");
        const auto f = b.session.PhysicalFrame();
        for (uint16_t i = 0U; i < f.led_count; ++i) {
            const auto c = f.pixels[i];
            Check(channel == 0U ? c.r && !c.g && !c.b && !c.w : channel == 1U ? c.g && !c.r && !c.b && !c.w
                : channel == 2U ? c.b && !c.r && !c.g && !c.w : c.r && c.g && c.b && (revision == hardware::BoardRevision::V3_3S3 ? c.w == 0U : c.w > 0U),
                "every fitted output carries exact R/G/B or RGB+fitted W through current protection");
        }
        // Compare the real factory output against the exact prior safe baseline
        // through the same pipeline/capture owner, before the new shared accent.
        firmware::ManufacturingTestFrameInputs optical;optical.revision=revision;optical.step=Mode::Step::LedRainbow;optical.ledChannel=channel;
        const auto requested=firmware::ComposeManufacturingTestLedFrame(optical);
        const bool rgbw=revision!=hardware::BoardRevision::V3_3S3;
        runtime::virtual_litch_internal::CaptureLedRenderer prior(f.led_count,rgbw);
        modules::PowerBudgetPolicy policy(modules::led_render_policy::MakeConservativePowerBudgetPolicyConfig({true,false}));
        auto pipelineConfig=modules::led_render_policy::MakePipelineConfig(rgbw,32U);
        pipelineConfig.outputBrightnessScale256=hardware::BoardHardwareProfileFor(revision).ledOutputScale256;
        pipelineConfig.enableS3OpticalColorCalibration=false;if(!rgbw)pipelineConfig.channelMaxCurrentMa=20U;
        pipelineConfig.brightnessScale01=230.0f/256.0f;
        modules::BudgetAwareLedPipeline reference(prior,policy,pipelineConfig);reference.RenderFrame(requested.data(),f.led_count);
        bool dimmed=true;for(uint16_t n=0U;n<f.led_count;++n){const auto a=prior.Pixels()[n],c=f.pixels[n];
            const uint8_t oldChannels[]={a.r,a.g,a.b,a.w},newChannels[]={c.r,c.g,c.b,c.w};
            for(unsigned die=0U;die<4U;++die)dimmed&=std::abs(int(newChannels[die])-int((oldChannels[die]*3U+2U)/4U))<=1;
        }
        Check(f.power_limited&&dimmed,"production factory VirtualLitch actual board/status RGBW/RGB codes dim25% after safe limiting");
        std::printf("[FACTORY_OPTICAL_POST_BUDGET] revision=%u channel=%u board_first=%u,%u,%u,%u status_first=%u,%u,%u,%u old_first=%u,%u,%u,%u dimmed=%u\n",unsigned(revision),channel,f.pixels[0].r,f.pixels[0].g,f.pixels[0].b,f.pixels[0].w,f.pixels[hardware::BoardLedTopologyFor(revision).physicalBoardLedCount].r,f.pixels[hardware::BoardLedTopologyFor(revision).physicalBoardLedCount].g,f.pixels[hardware::BoardLedTopologyFor(revision).physicalBoardLedCount].b,f.pixels[hardware::BoardLedTopologyFor(revision).physicalBoardLedCount].w,prior.Pixels()[0].r,prior.Pixels()[0].g,prior.Pixels()[0].b,prior.Pixels()[0].w,dimmed);
        b.Capture(channel == 0U ? "red_field" : channel == 1U ? "green_field" : channel == 2U ? "blue_field" : "white_field", "inspect every fitted emitter; O advances");
        b.Tap(channel % 2U + 1U, Button::O);
    }
    Check(b.Test().GetStep() == Mode::Step::HallSensors && b.Test().GetProgress().ledEmissionConfirmed, "white O confirms optical review and enters Hall");
    b.Tap(2U, Button::X);
    Check(b.Test().GetLedChannel() == 3U && b.Test().GetStep() == Mode::Step::LedRainbow, "either-bank X backs Hall to white");
    Check((b.Test().GetProgress().observedButtonMask & (1U << 12U)) != 0U, "navigation independently records physical P2X");
    b.Tap(1U, Button::O);
    b.Observe(UINT64_MAX); b.Capture("hall_both_white", "remove jig to establish current clear");
    b.Observe(0ULL); b.Tap(1U, Button::O);
    b.Capture("buttons_navigation_evidence", "test every remaining physical control on both banks");
    Check(b.Test().GetProgress().lightSensorResponseMask == 0U && !b.Test().GetProgress().motionEventObserved,
        "removed manual ambient/motion characterization is truthfully unobserved");
}
void ConsecutiveXAndAutomaticHallJourney(hardware::BoardRevision revision, bool rotate) {
    Board b(revision,rotate);
    for(unsigned field=0U;field<4U;++field)b.Tap(1U,Button::O);
    const auto topology=hardware::BoardLedTopologyFor(revision);
    const auto pending=b.session.PhysicalFrame().pixels[24U*topology.physicalLedsPerSquare];
    Check(pending.r>0U&&pending.g>0U&&!pending.b&&!pending.w,"human regression: clear-only Hall remains orange pending both electrical states");
    b.Capture("Hall_pending_orange_before_sweep","sweep magnets across every square; orange remains until occupied and clear observed");
    b.Observe(UINT64_MAX);
    Check(b.Test().GetStep()==Mode::Step::HallSensors,"historically complete but occupied Hall cannot advance");
    b.session.SetManufacturingHallSnapshot(false,0ULL);b.Advance(0U);
    Check(b.Test().GetStep()==Mode::Step::HallSensors,"invalid current Hall cannot auto advance");
    b.Observe(0ULL);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"human regression: final current valid empty Hall automatically enters Buttons without O");
    b.Capture("Hall_complete_auto_buttons","test both physical banks; X/O start orange and turn green when observed");
    for(Button interruption:{Button::Up,Button::Down,Button::Left,Button::Right}) {
        b.Tap(1U,Button::Up); // Reset the previous case's completed-X candidate.
        b.Tap(1U,Button::X);b.Down(1U,interruption,true);b.Advance(1100U);b.Down(1U,interruption,false);
        b.Tap(1U,Button::X);
        Check(b.Test().GetStep()==Mode::Step::Buttons,"human regression: a different direction and its repeats reset consecutive X exit");
        if(b.Test().GetStep()==Mode::Step::HallSensors)b.Tap(1U,Button::O);
        b.Tap(2U,Button::X);b.Tap(1U,Button::X);
        Check(b.Test().GetStep()==Mode::Step::Buttons,"human regression: other-bank X breaks consecutive same-X sequence");
        if(b.Test().GetStep()==Mode::Step::HallSensors)b.Tap(1U,Button::O);
    }
    b.Tap(1U,Button::Up);b.Tap(1U,Button::X);b.Tap(2U,Button::O);b.Tap(1U,Button::X);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"intervening other-bank O also resets consecutive X");
    b.Capture("Buttons_interleaved_X_stays","only two consecutive fresh complete cycles of the same physical X go back");
    b.Tap(1U,Button::X);
    Check(b.Test().GetStep()==Mode::Step::HallSensors,"second consecutive same-X complete cycle returns Hall");
    b.Advance(1000U);
    Check(b.Test().GetStep()==Mode::Step::HallSensors,"explicit Hall reinspection does not auto bounce on repeated valid empty sensor polls");
    b.Capture("Hall_deliberate_reinspection_stays","inspect white historical coverage; O deliberately returns to Buttons");
    b.Tap(1U,Button::O);b.Tap(1U,Button::X);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"reinspection reentry requires two new complete same-X cycles");
    b.session.RunManufacturingIngress([](void*,Mode& mode,uint32_t now){mode.Stop();mode.Enter(mode.GetRevision(),now);},nullptr);
    for(unsigned field=0U;field<4U;++field)b.Tap(1U,Button::O);
    b.Advance(1000U);
    Check(b.Test().GetStep()==Mode::Step::HallSensors&&!b.Test().GetProgress().hallComplete,"fresh restart cannot reuse old two-state Hall evidence or previous auto-advance authority");
    b.Down(1U,Button::O,true);b.Observe(UINT64_MAX);b.Observe(0ULL);b.Down(1U,Button::O,false);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"fresh restart rearms automatic Hall completion; held O entry tail cannot advance Buttons");
    b.Capture("fresh_run_recollects_Hall_auto_advance_rearmed","newly collected empty Hall enters Buttons; test remaining orange controls");
}
void WaitingRoleMotionJourney(hardware::BoardRevision revision, bool rotate) {
    Board b(revision,rotate);b.Local();
    const auto topology=hardware::BoardLedTopologyFor(revision);
    // Discovery is a controlled routing hint, never proof. Repeated eligible
    // advertisements keep the existing role owner searching while we inspect DUT waiting.
    b.role.Begin(b.Now(),1500U);b.Advance(0U);
    unsigned lowest=256U,highest=0U;
    for(unsigned phase=0U;phase<5U;++phase) {
        if(phase){b.role.ObserveEligiblePeer(b.Now());b.Advance(750U);}
        const auto& f=b.session.PhysicalFrame();
        const auto amber=f.pixels[18U*topology.physicalLedsPerSquare];
        lowest=std::min(lowest,unsigned(amber.r));highest=std::max(highest,unsigned(amber.r));
        bool pending=!b.Test().IsAutomaticVerifier()&&b.Test().GetStage()==Stage::Radio;
        for(uint8_t row=2U;row<6U;++row)for(uint8_t col=2U;col<6U;++col){
            const auto color=f.pixels[(row*8U+col)*topology.physicalLedsPerSquare];
            const bool boundary=row==2U||row==5U||col==2U||col==5U;
            pending&=boundary?color.r>color.g&&color.g>0U&&!color.b&&!color.w:!(color.r||color.g||color.b||color.w);
        }
        Check(pending,"post-budget DUT/discovery outline remains amber with dark center through breathing cycle");
        b.Capture("radio_DUT_waiting_amber_breathes","wait for actual peer proof; slow amber breathing is pending, never saved green");
    }
    Check(highest>lowest,"production post-budget DUT outline visibly changes brightness over three seconds");
    b.role.Begin(b.Now(),0U);b.Advance(0U);
    constexpr uint8_t border[]={18U,19U,20U,21U,29U,37U,45U,44U,43U,42U,34U,26U};
    for(unsigned phase=0U;phase<4U;++phase){
        if(phase)b.Advance(250U);
        unsigned white=0U;bool correct=b.Test().IsAutomaticVerifier();
        for(uint8_t square:border){const auto c=b.session.PhysicalFrame().pixels[square*topology.physicalLedsPerSquare];
            const bool marker=c.r>0U&&c.r==c.g&&c.g==c.b;white+=marker;
            correct&=marker?square==border[(b.Test().GetStepElapsedMs(b.Now())/250U)%12U]:c.r>c.g&&c.g>0U&&!c.b&&!c.w;
        }
        Check(correct&&white==1U,"production unsaved automatic verifier advances exactly one clockwise white boundary marker");
        b.Capture("radio_unsaved_verifier_clockwise_white_boundary","keep verifier available; amber outline and dark center remain pending real BLE proof");
    }
    modules::HardwareTestRecord record;Check(!b.Test().BuildPendingRecord(record),"waiting motion and automatic election cannot manufacture saved PASS");
}
void EarlyReviewCannotStrandLocalJourney(hardware::BoardRevision revision, bool rotate) {
    Board b(revision,rotate,0U);
    b.QualifyBothLightResponses();
    Check(b.Test().GetProgress().lightSensorResponseMask == 0U,
          "manufacturing light changes during optical inspection cannot qualify the sensors");
    for(unsigned field=0U;field<4U;++field)b.Tap(1U,Button::O);
    b.Observe(UINT64_MAX);b.Observe(0ULL);
    b.Tap(1U,Button::O);
    b.QualifyBothLightResponses();
    for(uint8_t bank:{1U,2U})for(Button button:{Button::X,Button::O,Button::Up,Button::Down,Button::Left,Button::Right})
        if (bank != 2U || button != Button::Right) b.Tap(bank,button);
    b.Down(2U,Button::Right,true);
    Check(!b.Test().GetProgress().buttonsComplete && b.Test().GetStep()==Mode::Step::Buttons &&
          b.Test().GetStage()==Stage::Local && b.Test().GetProgress().lightSensorResponseMask==3U,
          "early O and both light responses still require the last held direction to release");
    b.Capture("early_O_last_direction_held","release the final direction to start the wireless test");
    b.Down(2U,Button::Right,false);
    Check(b.Test().GetStep()==Mode::Step::Radio&&b.Test().GetStage()==Stage::Radio,
        "human HIL regression: early O followed by the last direction release enters Radio without extra O");
    b.Capture("early_O_last_direction_enters_real_Radio","wait for peer BLE proof; animated amber cannot claim saved PASS");
}
void ButtonFirstXJourney(hardware::BoardRevision revision, uint8_t bank, bool rotate) {
    Board b(revision, rotate);
    const auto topology=hardware::BoardLedTopologyFor(revision);
    for(unsigned field=0U;field<4U;++field)b.Tap(bank,Button::O);
    b.Observe(UINT64_MAX);b.Observe(0ULL);
    b.Tap(bank,Button::X);b.Tap(bank,Button::O);b.Tap(bank,Button::O);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"fresh operator entry reaches Buttons despite earlier Hall X evidence");
    b.Down(bank,Button::X,true);b.Advance(1500U);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"held first X cannot navigate from Buttons");
    const uint8_t xSquare=bank==1U?57U:6U;
    const auto heldX=b.session.PhysicalFrame();
    const auto xLeft=heldX.pixels[xSquare*topology.physicalLedsPerSquare],xRight=heldX.pixels[xSquare*topology.physicalLedsPerSquare+2U];
    Check(xLeft.r>0U&&xLeft.r==xLeft.g&&xLeft.g==xLeft.b&&xLeft.r==xRight.r&&xLeft.g==xRight.g&&xLeft.b==xRight.b,
        "first held X has a visible matching neutral pair after gain and current limiting");
    b.Capture("buttons_first_X_held_no_escape","release X to count its test; remain on buttons");
    b.Down(bank,Button::X,false);
    const uint16_t xBit=1U<<(bank==1U?5U:12U);
    Check(b.Test().GetStep()==Mode::Step::Buttons && (b.Test().GetProgress().observedButtonMask&xBit),
          "human regression: first distinct X test release counts and stays on Buttons");
    bool xVisible=true;for(uint8_t side:{0U,2U}){const auto c=b.session.PhysicalFrame().pixels[xSquare*topology.physicalLedsPerSquare+side];xVisible&=!c.r&&c.g>0U&&!c.b&&!c.w;}
    const uint8_t upSquare=bank==1U?35U:27U;const auto remaining=b.session.PhysicalFrame().pixels[upSquare*topology.physicalLedsPerSquare];
    Check(xVisible&&remaining.r>0U&&remaining.g>0U&&!remaining.b&&!remaining.w,
        "first X release retains its green paired navigation mark while untested Up remains on; stage and history are retained");
    b.Capture("buttons_first_X_release_stays","test remaining lit controls; second fresh X press/release goes back");
    // Any unrelated direction, including its real repeats, resets the sequence.
    b.Down(bank,Button::Up,true);b.Advance(1000U);b.Down(bank,Button::Up,false);
    b.Capture("buttons_direction_repeat_resets_first_X","release completed; two new consecutive same-X cycles are required");
    b.Tap(bank,Button::X);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"intervening direction resets X; the next X remains on Buttons");
    b.Tap(bank,Button::X);
    Check(b.Test().GetStep()==Mode::Step::HallSensors,"second consecutive same-X release goes back exactly once");
    b.Capture("buttons_second_X_release_backs_to_Hall","O reenters buttons without losing physical input evidence");
    b.Tap(bank,Button::O);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"O reenters Buttons after deliberate back");
    b.Tap(bank,Button::X);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"reentry gives first X test cycle again");
    b.Capture("buttons_reentry_first_X_stays","fresh second X remains deliberate escape");
    // A canceled chord records both physical tests but never arms an escape.
    b.session.RunManufacturingIngress([](void*,Mode& m,uint32_t now){m.Stop();m.Enter(m.GetRevision(),now);},nullptr);
    for(unsigned field=0U;field<4U;++field)b.Tap(bank,Button::O);
    b.Tap(bank,Button::O);
    for(bool oFirst:{false,true}){
        const Button first=oFirst?Button::O:Button::X,second=oFirst?Button::X:Button::O;
        b.Down(bank,first,true);b.Down(bank,second,true);b.Advance(800U);
        Check(b.Test().GetOperatorHoldProgress()>0U,"button chord owns proportional status progress");
        b.Capture("buttons_chord_priority","release either to cancel; both tails consumed");
        b.Down(bank,first,false);b.Down(bank,second,false);
        Check(b.Test().GetStep()==Mode::Step::Buttons,"canceled button chord does not navigate");
        b.Capture("buttons_chord_canceled","fresh first X still counts test without escape");
    }
    b.Tap(bank,Button::X);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"canceled chord did not spend first X cycle");
    b.Capture("buttons_post_chord_first_X_stays","fresh second X backs to Hall");
    b.Tap(bank,Button::X);Check(b.Test().GetStep()==Mode::Step::HallSensors,"post-chord second X recovers deliberate back");
    b.Down(bank,Button::X,true);b.Tap(bank==1U?2U:1U,Button::O);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"other-bank O can enter Buttons while X was already held");
    b.Down(bank,Button::X,false);b.Tap(bank,Button::X);
    Check(b.Test().GetStep()==Mode::Step::Buttons,"pre-entry held X tail does not spend first fresh Buttons cycle");
    b.Capture("buttons_preentry_X_tail_ignored","fresh second X backs to Hall; historical tested bit cannot replace a fresh cycle");
    b.Tap(bank,Button::X);Check(b.Test().GetStep()==Mode::Step::HallSensors,"second fresh cycle still recovers after held entry tail");
    Board last(revision,rotate);
    for(unsigned field=0U;field<4U;++field)last.Tap(bank,Button::O);
    last.Observe(UINT64_MAX);last.Observe(0ULL);last.Tap(bank,Button::O);
    for(uint8_t side:{1U,2U})for(Button control:{Button::Up,Button::Down,Button::Left,Button::Right,Button::X,Button::O})
        if(!(side==bank&&control==Button::X))last.Tap(side,control);
    last.QualifyBothLightResponses();
    last.Down(bank,Button::X,true);
    modules::HardwareTestRecord record;
    Check(last.Test().GetStep()==Mode::Step::Buttons&&!last.Test().BuildPendingRecord(record),
        "first X as last missing contact stays on Buttons while held and cannot queue PASS");
    last.Down(bank,Button::X,false);
    Check(last.Test().GetStep()==Mode::Step::Radio && last.Test().GetStage()==Stage::Radio &&
          last.Test().GetProgress().buttonsComplete && !last.Test().BuildPendingRecord(record),
          "last missing X release starts Radio when the local checks pass, without back or extra O");
    last.Capture("buttons_last_missing_X_enters_Radio","wait for the actual wireless proof");
}
void ButtonFootprintAndCalmJourney(hardware::BoardRevision revision, bool rotate) {
    Board b(revision,rotate,0U);
    const auto topology=hardware::BoardLedTopologyFor(revision);
    for(unsigned field=0U;field<4U;++field)b.Tap(1U,Button::O);
    b.Observe(UINT64_MAX);b.Observe(0ULL);
    const auto hall=b.session.PhysicalFrame();
    b.Advance(333U);
    bool hallStable=true;for(unsigned n=0U;n<hall.led_count;++n){const auto c=hall.pixels[n],d=b.session.PhysicalFrame().pixels[n];hallStable&=c.r==d.r&&c.g==d.g&&c.b==d.b&&c.w==d.w;}
    Check(hallStable,"Hall local evidence is independent from automatic verifier animation");
    b.Capture("Hall_auto_advanced_to_steady_buttons","final empty Hall entered Buttons; test the remaining orange controls");
    b.Tap(1U,Button::O);
    const auto pending=b.session.PhysicalFrame();b.Advance(500U);
    bool pendingStable=true;for(unsigned n=0U;n<pending.led_count;++n){const auto c=pending.pixels[n],d=b.session.PhysicalFrame().pixels[n];pendingStable&=c.r==d.r&&c.g==d.g&&c.b==d.b&&c.w==d.w;}
    Check(pendingStable,"button pending controls are steady through former pulse/verifier boundaries after current limiting");
    bool quiet=true;for(uint8_t n=0U;n<32U;++n){const unsigned physical=revision==hardware::BoardRevision::V3_3S3?hardware::V3_3PhysicalIndexForSemanticStatus(n):n;const auto c=pending.pixels[topology.physicalBoardLedCount+physical];quiet&=!c.r&&!c.g&&!c.b&&!c.w;}
    Check(quiet,"local button fixed32 perimeter is quiet; V3.3 dedicated controls remain separate contact proof");
    b.Capture("buttons_steady_pending","press each physical control and observe both square sides");
    b.QualifyBothLightResponses();
    constexpr Button controls[]={Button::Up,Button::Down,Button::Left,Button::Right,Button::X,Button::O};
    constexpr uint8_t footprint[12][2]={{35U,36U},{59U,60U},{50U,42U},{53U,45U},{57U,255U},{62U,255U},
        {27U,28U},{3U,4U},{13U,21U},{10U,18U},{6U,255U},{1U,255U}};
    constexpr uint8_t offsets[]={0U,1U,2U,3U,5U,6U};
    constexpr uint8_t dedicated[]={27U,29U,30U,28U,31U,26U,5U,7U,8U,6U,9U,4U};
    for(uint8_t bank:{1U,2U})for(unsigned control=0U;control<6U;++control){
        const unsigned index=(bank-1U)*6U+control;const uint16_t bit=1U<<((bank-1U)*7U+offsets[control]);
        const bool alreadyTested=(b.Test().GetProgress().observedButtonMask&bit)!=0U;
        bool pendingVisible=true;for(uint8_t square:footprint[index])if(square<64U){
            for(uint8_t side:{0U,2U}){const auto c=b.session.PhysicalFrame().pixels[square*topology.physicalLedsPerSquare+side];pendingVisible&=c.r>0U&&c.g>0U&&!c.b&&!c.w;}
        }
        if(!alreadyTested)Check(pendingVisible,"raw post-budget untested pair remains visibly on in steady pending phase");
        b.Down(bank,controls[control],true);
        bool sides=true;for(uint8_t square:footprint[(bank-1U)*6U+control])if(square<64U){
            const unsigned base=square*topology.physicalLedsPerSquare;
            const auto left=b.session.PhysicalFrame().pixels[base],right=b.session.PhysicalFrame().pixels[base+2U];
            sides&=left.r&&left.g&&left.b&&left.r==left.g&&left.g==left.b&&
                left.r==right.r&&left.g==right.g&&left.b==right.b&&left.w==right.w;
            for(uint8_t piece=1U;piece<topology.physicalLedsPerSquare;piece+=2U){const auto c=b.session.PhysicalFrame().pixels[base+piece];sides&=!c.r&&!c.g&&!c.b&&!c.w;}
        }
        if(revision==hardware::BoardRevision::V3_3S3){const auto c=b.session.PhysicalFrame().pixels[256U+dedicated[index]];sides&=c.r>0U&&c.r==c.g&&c.g==c.b;}
        Check(sides,"raw physical button reaches correct diagram and matching two outward sides after gain/current limiting");
        b.Capture("button_held_both_sides_exact_footprint","release this control; tested directions go dark and X/O remain visible; first X does not escape");
        b.Down(bank,controls[control],false);
        bool dark=true;for(uint8_t square:footprint[index])if(square<64U){
            for(uint8_t side:{0U,2U}){const auto c=b.session.PhysicalFrame().pixels[square*topology.physicalLedsPerSquare+side];dark&=control<4U||b.Test().GetStep()!=Mode::Step::Buttons?!c.r&&!c.g&&!c.b&&!c.w:!c.r&&c.g>0U&&!c.b&&!c.w;}
        }
        if(revision==hardware::BoardRevision::V3_3S3){const auto c=b.session.PhysicalFrame().pixels[256U+dedicated[index]];dark&=control<4U||b.Test().GetStep()!=Mode::Step::Buttons?!c.r&&!c.g&&!c.b&&!c.w:!c.r&&c.g>0U&&!c.b&&!c.w;}
        Check(dark&&(b.Test().GetProgress().observedButtonMask&bit)&&!(b.Test().GetProgress().pressedButtonMask&bit),
            "released directions go dark; X/O remain green on Buttons and disappear at the actual Radio handoff");
        b.Capture("button_release_history_or_retained_navigation","test remaining lit controls; the final release completes local review");
    }
    Check(b.Test().GetStep()==Mode::Step::Radio&&b.Test().GetProgress().observedButtonMask==0x37EFU,"all twelve physical controls complete with no first-X stage interruption");
    const auto radio=b.session.PhysicalFrame();
    for(uint32_t ms:{250U,250U,500U,500U}){
        b.Advance(ms);bool stable=true;for(unsigned n=0U;n<radio.led_count;++n){const auto c=radio.pixels[n],d=b.session.PhysicalFrame().pixels[n];stable&=c.r==d.r&&c.g==d.g&&c.b==d.b&&c.w==d.w;}
        Check(!stable,"pending verifier white boundary marker moves at250/500ms boundaries");
        b.Capture("radio_pending_central_outline_no_XO","connect peer for BLE proof; amber outline with clockwise white marker identifies verifier; X returns Buttons");
    }
    b.session.SetManufacturingHallSnapshot(false,0ULL);b.Advance(0U);
    Check(b.Test().GetStep()==Mode::Step::HallSensors,"lost current Hall authority returns to required local recovery");
    b.Capture("radio_pending_invalid_Hall_local_recovery","restore valid current empty Hall; quiet radio group yields to local evidence");
    b.Observe(0ULL);Check(b.Test().GetStep()==Mode::Step::Radio,"valid Hall read restores pending radio without recollecting controls");
    b.Capture("radio_pending_Hall_recovered","new radio proof still required; no false saved PASS");
    Report(b,modules::FactoryRadioChannel::Ble);
    b.Capture("radio_BLE_proved_current_Hall_sampled_persisting","current valid empty Hall follows BLE proof through production frame update; durable save still pending");
    const auto frame=b.session.PhysicalFrame();const auto waiting=frame.pixels[19U*topology.physicalLedsPerSquare];
    Check(b.Test().GetStage()==Stage::Persisting&&waiting.r>0U&&waiting.g>0U&&!waiting.b&&!waiting.w&&
          frame.pixels[27U*topology.physicalLedsPerSquare].g==0U,
          "complete BLE proof and current Hall enter amber persistence with dark center; save alone grants green");
}
void FinalControlsToRadioSquareJourney(hardware::BoardRevision revision, bool rotate) {
    Board b(revision,rotate,0U);
    for(unsigned field=0U;field<4U;++field)b.Tap(1U,Button::O);
    b.Observe(UINT64_MAX);b.Observe(0ULL);b.Tap(1U,Button::O);
    for(uint8_t bank:{1U,2U})for(Button control:{Button::Up,Button::Down,Button::Left,Button::Right,Button::X})b.Tap(bank,control);
    b.QualifyBothLightResponses();
    const auto topology=hardware::BoardLedTopologyFor(revision);
    const auto litPair=[&](const Board& board,uint8_t square){const auto& f=board.session.PhysicalFrame();const auto a=f.pixels[square*topology.physicalLedsPerSquare],c=f.pixels[square*topology.physicalLedsPerSquare+2U];return(a.r||a.g||a.b||a.w)&&a.r==c.r&&a.g==c.g&&a.b==c.b&&a.w==c.w;};
    const auto darkPair=[&](const Board& board,uint8_t square){const auto& f=board.session.PhysicalFrame();const auto a=f.pixels[square*topology.physicalLedsPerSquare],c=f.pixels[square*topology.physicalLedsPerSquare+2U];return!(a.r||a.g||a.b||a.w||c.r||c.g||c.b||c.w);};
    const auto central=[&](const Board& board,bool passed,bool spin){
        const auto& f=board.session.PhysicalFrame();bool correct=true;unsigned white=0U;
        for(uint8_t row=2U;row<6U;++row)for(uint8_t col=2U;col<6U;++col){const auto c=f.pixels[(row*8U+col)*topology.physicalLedsPerSquare];const bool boundary=row==2U||row==5U||col==2U||col==5U;
            if(!passed&&!boundary)correct&=darkPair(board,row*8U+col);
            else if(passed){const bool neutral=c.r>0U&&c.r==c.g&&c.g==c.b;white+=neutral;
                constexpr uint8_t clockwise[]={18U,19U,20U,21U,29U,37U,45U,44U,43U,42U,34U,26U};
                correct&=litPair(board,row*8U+col)&&(neutral?spin&&row*8U+col==clockwise[(board.Test().GetStepElapsedMs(board.Now())/250U)%12U]:!c.r&&c.g>0U&&!c.b&&!c.w);}
            else {const bool neutral=c.r>0U&&c.r==c.g&&c.g==c.b;white+=neutral;
                constexpr uint8_t clockwise[]={18U,19U,20U,21U,29U,37U,45U,44U,43U,42U,34U,26U};
                correct&=litPair(board,row*8U+col)&&(neutral?board.Test().IsAutomaticVerifier()&&row*8U+col==clockwise[(board.Test().GetStepElapsedMs(board.Now())/250U)%12U]:c.r>c.g&&c.g>0U&&!c.b&&!c.w);
            }
            if(topology.physicalLedsPerSquare==4U)for(uint8_t piece:{1U,3U}){const auto p=f.pixels[(row*8U+col)*4U+piece];correct&=!(p.r||p.g||p.b||p.w);}
        }
        return correct&&white==((passed?spin:board.Test().IsAutomaticVerifier())?1U:0U);
    };
    Check(b.Test().GetStep()==Mode::Step::Buttons&&!(b.Test().GetProgress().observedButtonMask&(1U<<13U)),"last button state really lacks far-bank O after all arrows disappear");
    for(uint8_t square:{57U,62U,1U,6U})Check(litPair(b,square),"both banks retain paired X/O navigation on last Buttons state");
    Check(litPair(b,9U)&&darkPair(b,49U)&&darkPair(b,54U)&&darkPair(b,14U),"only untested far-bank O has extra inward-square pending extent");
    b.Capture("final_buttons_far_O_missing_XO_visible","test remaining far-bank O; retained X/O remain usable at both ends");
    b.Down(2U,Button::O,true);b.Capture("final_buttons_O_held_visible","release O; real final control release and current empty Hall complete local review");
    b.Down(2U,Button::O,false);
    Check(b.Test().GetStep()==Mode::Step::Radio&&b.Test().GetProgress().buttonsComplete,"actual final O release enters radio wait");
    Check(central(b,false,false),"actual Radio handoff is amber central4x4 outline, not four unexplained yellow status LEDs");
    Check(b.NavigationDark(),"both banks X/O and dedicated controls are absent in actual Radio state");
    for(uint8_t square:{49U,54U,9U,14U})Check(darkPair(b,square),"all12 observed Radio has no extra pending-contact squares");
    modules::HardwareTestRecord record;Check(b.Test().IsAutomaticVerifier()&&!b.Test().BuildPendingRecord(record),"role election never supplies proof, green success or PASS");
    b.Capture("radio_wait_elected_verifier_amber_outline","wait for a real peer; X returns Buttons, O cannot waive radio proof");
    b.Tap(1U,Button::O);Check(b.Test().GetStep()==Mode::Step::Radio&&!b.Test().BuildPendingRecord(record)&&central(b,false,false),"Radio O cannot complete missing BLE proof");
    for(bool oFirst:{false,true}){
        const Button first=oFirst?Button::O:Button::X,second=oFirst?Button::X:Button::O;
        b.Down(1U,first,true);b.Down(1U,second,true);b.Advance(800U);
        Check(b.Test().GetStep()==Mode::Step::Radio&&b.Test().GetOperatorHoldProgress()>0U,"Radio same-bank chord retains waiting mode and owns proportional exit intent");
        b.Down(1U,first,false);b.Down(1U,second,false);
        Check(b.Test().GetStep()==Mode::Step::Radio&&central(b,false,false)&&!b.Test().BuildPendingRecord(record),"canceled Radio chord consumes both tails without navigation, review reopening or false PASS");
        b.Capture("radio_canceled_chord_restores_outline","waiting remains; a fresh single X release returns to Buttons review");
    }
    b.Down(1U,Button::X,true);Check(b.Test().GetStep()==Mode::Step::Radio,"Radio X stays on waiting view until deliberate release");
    Check(b.NavigationDark(),"held Radio X cannot reintroduce button marks before release");
    b.Capture("radio_held_X_no_marks","release X to return to Buttons; role outline remains recognizable while held");
    Board proofPeer(revision,rotate,0U);proofPeer.Local();Prove(b,proofPeer,8765U);
    Check(!b.Test().BuildPendingRecord(record),"real BLE protocol proof interleaved with held X cannot PASS");
    b.Down(1U,Button::X,false);Check(b.Test().GetStep()==Mode::Step::Buttons&&!b.Test().BuildPendingRecord(record),"one fresh Radio X release returns Buttons and invalidates prior operator review before proof admission");
    Check(b.Test().GetProgress().radioChannelMask==modules::FACTORY_RADIO_ALL_CHANNELS,"deliberate reinspection preserves real collected proof");
    b.Capture("radio_X_release_returns_review_proofs_preserved","fresh Buttons entry: first X stays, second backs to Hall; fresh O review required");
    b.Tap(1U,Button::X);Check(b.Test().GetStep()==Mode::Step::Buttons&&!b.Test().BuildPendingRecord(record),"Radio return reentry's first X stays despite complete proof/history");
    b.Tap(1U,Button::X);Check(b.Test().GetStep()==Mode::Step::HallSensors,"Radio return reentry's second X backs to Hall");
    b.Tap(1U,Button::O);b.session.SetManufacturingHallSnapshot(false,0ULL);b.Tap(1U,Button::O);
    Check(!b.Test().BuildPendingRecord(record)&&b.Test().GetStep()==Mode::Step::HallSensors,"fresh O review still cannot waive invalid current Hall authority");
    b.Capture("radio_reinspection_invalid_Hall_no_green","restore current valid empty Hall; lost electronics clears proof and requires a new exchange");
    b.Observe(0ULL);Check(b.Test().GetStage()==Stage::Radio&&central(b,false,false)&&b.Test().GetProgress().radioChannelMask==0U,"valid empty Hall recovers amber wait but real electronics loss requires new radio proof");
    Prove(b,proofPeer,8766U);Check(b.Test().GetStage()==Stage::Persisting&&central(b,false,false),"new real BLE proof and valid empty Hall after fresh O enter amber persistence without fake green");
    b.Capture("radio_real_BLE_proof_persisting_amber_outline","save exact proof record; green waits for durable receipt");
    b.Save(false);Check(b.Test().GetStage()==Stage::Failed&&!central(b,true,true),"failed save cannot show green/spinning verifier success");
    b.Capture("radio_save_failure_truthful","repair persistence and restart qualification; failed receipt is not PASS");
    Board verifier(revision,rotate,0U),dut(revision,rotate,1500U);verifier.Local();dut.Local();
    // Use the existing real v3 challenge/response/terminal helpers; RF and flash remain controlled.
    Prove(dut,verifier,9876U);Check(verifier.Test().GetStage()==Stage::Persisting&&dut.Test().GetStage()==Stage::Persisting,"real BLE protocol proof requests persistence, never presentation success by itself");
    verifier.Save();dut.Save();
    Check(central(verifier,true,true)&&central(dut,true,false),"durably saved verifier spins green/white; saved DUT is steady filled green4x4");
    dut.Capture("radio_saved_DUT_filled_green_square","verified DUT complete; no full-board sparkle");
    Check(!dut.Test().IsAutomaticVerifier(),
        "saved DUT completion retains its non-verifier role before normal reboot");
    // Presentation-only saved-verifier feedback follows. The real minimal-boot
    // journey owns explicit saved-verifier entry and completion reboot routing.
    dut.Advance(750U);dut.session.RunManufacturingIngress([](void*,Mode& m,uint32_t now){m.SetVerifierBleProofActive(true,now);},nullptr);
    Check(central(dut,true,true),"saved DUT's real server-active feedback shows verifier spin without inventing a new role");
    dut.Capture("radio_saved_DUT_later_peer_server_active_spin","serve later board; white marker uses continuous saved-step phase");
    const auto phaseBefore=dut.Test().GetStepElapsedMs(dut.Now());
    Board later(revision,rotate,1500U);later.Local();Prove(later,dut,9877U);
    dut.session.RunManufacturingIngress([](void*,Mode& m,uint32_t now){m.ReportVerifierProofComplete(now);},nullptr);
    Check(dut.Test().GetStepElapsedMs(dut.Now())==phaseBefore&&central(dut,true,true),"terminal-success feedback preserves the same white boundary phase");
    dut.Capture("radio_saved_DUT_server_terminal_success_continuous_spin","brief terminal feedback retains spin; then return to steady verified square");
    dut.Advance(Mode::VERIFIER_SUCCESS_CUE_DURATION_MS-1U);Check(central(dut,true,true),"saved DUT terminal serving feedback remains until existing deadline");
    dut.Advance(1U);Check(central(dut,true,false),"saved DUT returns to steady filled green after server terminal feedback expires");
    dut.Capture("radio_saved_DUT_server_feedback_expired_steady","verified DUT remains steady; later server activity may spin again");
    for(unsigned phase=0U;phase<12U;++phase){if(phase)verifier.Advance(250U);Check(central(verifier,true,true),"saved verifier retains one white boundary cell and filled green center at every250ms phase");verifier.Capture("radio_saved_verifier_boundary_spin","saved-verifier presentation: white marker rotates; real service entry is explicit");}
}
void PendingProofRecoveryAndBatch() {
    Board first(hardware::BoardRevision::V3_3S3, false, 0U), second;
    first.Advance(0U); first.Local();
    Check(first.Test().GetStage() == Stage::Radio, "local-only verifier waits for actual BLE proof");
    modules::HardwareTestRecord none; Check(!first.Test().BuildPendingRecord(none), "absent peer cannot create saved PASS or bypass");
    uint32_t sampled = 0U;
    for (uint32_t phase : {0U, 250U, 500U, 750U, 1000U, 1500U}) {
        first.Advance(phase - sampled); sampled = phase;
        first.Capture(phase == 1500U ? "missing_radios_pending_1500ms" : "missing_radios_animated_outline", "connect second board; missing proof does not establish radio fault");
    }
    second.Local(); Prove(second, first, 1234U);
    Check(second.Test().GetStage() == Stage::Persisting && first.Test().GetStage() == Stage::Persisting, "real local and BLE observations request saved completion on both boards");
    first.Save(); second.Save(); first.Capture("first_saved_success", "presentation owner: responder stays minimal serving; DUT completion reboots normal"); second.Capture("dut_persistent_green_white", "successful board may leave jig");
    first.Advance(1200U);
    for (uint32_t ms : {333U, 333U, 333U}) { first.Advance(ms); first.Capture("quiet_verifier_steady_marks", "presentation/election fixture; actual saved responder retains batch service"); }
    Board third; third.Local(); Prove(third, first, 2222U); third.Save(); third.Capture("late_board_saved", "saved-verifier presentation; real retained service lifecycle covered by minimal journey");
    Board bad; bad.Local();
    bad.session.SetManufacturingHallSnapshot(false, 0ULL);
    bad.session.RunManufacturingIngress([](void*, Mode& m, uint32_t now) { m.Tick(now); }, nullptr);
    Check(!bad.Test().GetProgress().hallSnapshotValid && bad.Test().GetStage() == Stage::Local, "invalid current Hall reading revokes local completion");
    bad.Capture("invalid_hall_authority", "restore expander reading; historical coverage stays retained");
    bad.Observe(0ULL); Check(bad.Test().GetStage() == Stage::Radio, "fresh clear Hall observation recovers without recollecting history");
    bad.session.RunManufacturingIngress([](void*, Mode& m, uint32_t now) { m.ResetPendingRadioProof(); }, nullptr);
    Check(bad.Test().GetProgress().radioChannelMask == 0U, "missing peer retry cannot manufacture completed BLE proof");
    Prove(bad, first, 3333U); bad.Save(false);
    Check(bad.Test().GetStage() == Stage::Failed, "storage failure cannot sparkle PASS"); bad.Capture("storage_failed", "inspect Console storage; use restart after repair or override");
}
void SimultaneousBatch(unsigned count, bool sameDeadline) {
    std::array<std::unique_ptr<Board>, 6U> boards;
    for (unsigned index = 0U; index < count; ++index) {
        boards[index] = std::make_unique<Board>(index % 2U ? hardware::BoardRevision::V3S3 : hardware::BoardRevision::V3_3S3,
                                               false, sameDeadline ? 0U : (index * 367U) % 1501U);
        boards[index]->Advance(1500U); boards[index]->Local();
        Check(boards[index]->role.IsVerifier() && boards[index]->Test().GetStage() == Stage::Radio,
              "simultaneous no-peer election remains pending without manufacturing success");
    }
    for (unsigned index = 1U; index < count; ++index) {
        Check(modules::FactoryRadioRoleGate::PeerEligible(index + 1U, 1U, true) &&
              !modules::FactoryRadioRoleGate::PeerEligible(1U, index + 1U, true),
              "simultaneous verifier routing descends stable identities without cycle");
        Prove(*boards[index], *boards[0], 10000U + index * 100U);
        boards[index]->Save(); if (index == 1U) boards[0]->Save();
        Check(boards[index]->Test().GetStage() == Stage::Passed && boards[0]->Test().GetStage() == Stage::Passed,
              "isolated election/presentation fixture retains verifier role across serialized proof observations");
        boards[index]->Capture("simultaneous_batch_saved", "election fixture only; actual responder stays serving, completed DUTs reboot normal");
    }
}
void FinalHallSinglePieceRecovery(hardware::BoardRevision revision, bool rotate) {
    Board b(revision, rotate); b.Local(); b.Observe(1ULL); // Canonical A8 remains occupied after all history/review.
    Report(b, modules::FactoryRadioChannel::Ble);
    modules::HardwareTestRecord noPass;
    Check(b.Test().GetStep() == Mode::Step::HallSensors && !b.Test().BuildPendingRecord(noPass),
          "all-history final review with A8 magnet cannot create PASS");
    const auto topology = hardware::BoardLedTopologyFor(revision);
    for(unsigned phase=0U;phase<topology.physicalLedsPerSquare;++phase){
        if(phase)b.Advance(250U);
        const auto& frame=b.session.PhysicalFrame();unsigned dark=0U,lit=0U;
        for(unsigned slot=0U;slot<topology.physicalLedsPerSquare;++slot){
            const auto c=frame.pixels[slot];dark+=!(c.r||c.g||c.b||c.w);lit+=bool(c.r||c.g||c.b||c.w);
            const auto clear=frame.pixels[topology.physicalLedsPerSquare+slot];
            Check(clear.r||clear.g||clear.b||clear.w,"clear complete neighbor stays white after conservative current budget");
        }
        Check(dark==1U&&lit==topology.physicalLedsPerSquare-1U,
              "A8 localized remove cue retains a dark gap at every post-budget animation phase in either orientation");
        b.Capture("all_history_A8_remove_magnet_least_phase", "remove the one magnet at A8; all historical coverage retained");
    }
    b.Observe(0ULL);
    Check(b.Test().GetStage()==Stage::Persisting && b.Test().GetProgress().hallActiveMask==UINT64_MAX &&
          b.Test().GetProgress().hallClearMask==UINT64_MAX,"fresh empty read recovers without recollecting64-square history");
    b.Capture("all_history_A8_clear_recovered", "wait for exact settings receipt before saved PASS");
    b.Save();b.Capture("all_history_A8_recovery_saved", "qualification complete; saved PASS");
}
void OverrideAndChordArbitration(hardware::BoardRevision revision, uint8_t bank, bool rotate, bool pressOFirst) {
    const Button first = pressOFirst ? Button::O : Button::X;
    const Button second = pressOFirst ? Button::X : Button::O;
    auto admitChord = [&](Board& board) {
        board.Down(bank, first, true);
        board.Capture(pressOFirst ? "same_bank_O_first_PRESS" : "same_bank_X_first_PRESS",
                      "keep first control held; press other control on same bank");
        Check(board.Test().IsActive(), "first same-bank press cannot exit factory");
        const auto channelBeforeSecond = board.Test().GetLedChannel();
        board.Down(bank, second, true);
        Check(board.Test().IsActive() && board.Test().GetLedChannel() == channelBeforeSecond,
              "second same-bank press admits chord without navigation or exit");
        board.Capture(pressOFirst ? "same_bank_O_then_X_PRESS_admitted" : "same_bank_X_then_O_PRESS_admitted",
                      "both pressed; keep held to5s or release either to cancel");
    };
    Board b(revision, rotate);
    admitChord(b);
    b.Capture("same_bank_hold_begin", "keep same-bank X+O held to5s; release either to cancel");
    b.Advance(4900U); b.Capture("same_bank_hold_before_threshold", "keep held to5s or cancel"); Check(b.Test().IsActive(), "same-bank override does not exit before5sec");
    b.Advance(100U); Check(!b.Test().IsActive() && b.Test().IsOverridePending(), "five-second threshold exits immediately before release/save");
    modules::HardwareTestRecord record;
    Check(b.Test().BuildPendingRecord(record) && record.radioResult == modules::ManufacturingTestRadioResult::Override && !modules::IsVerifiedFactoryPass(record) && !record.factoryVerifierEnabled && record.radioChannelMask == 0U, "override is distinct from verified PASS and never peer eligibility");
    b.Capture("override_immediate_normal", "normal firmware now; saved override pending until actual receipt");
    const auto before = b.session.State();
    b.Advance(3000U); b.Capture("override_held_tail_normal", "release both held controls; no ordinary navigation from tails");
    b.Down(bank, pressOFirst ? Button::O : Button::X, false); b.Capture("override_first_release_normal", "release remaining control; tail suppressed");
    b.Down(bank, pressOFirst ? Button::X : Button::O, false); b.Advance(100U); b.Capture("override_all_released_normal", "fresh ordinary controls may act now");
    const auto after = b.session.State();
    Check(before.interaction_phase == after.interaction_phase && before.required_action == after.required_action, "held override tails cannot navigate Play after exit");
    Board canceled(revision, rotate);
    admitChord(canceled); canceled.Advance(100U);
    canceled.Capture("same_bank_cancel_progress", "release either same-bank control to cancel hold");
    canceled.Down(bank, pressOFirst ? Button::O : Button::X, false); canceled.Capture("same_bank_cancel_first_release", "release remaining control; chord release cannot navigate");
    canceled.Down(bank, pressOFirst ? Button::X : Button::O, false); canceled.Capture("same_bank_cancel_all_released", "fresh O can navigate again");
    Check(canceled.Test().IsActive() && canceled.Test().GetLedChannel() == 0U, "canceled chord consumes both releases without navigation");
    canceled.Tap(bank, Button::O); Check(canceled.Test().GetLedChannel() == 1U, "fresh release after canceled chord restores ordinary navigation");
    Board progress(revision,rotate);
    for(unsigned field=0U;field<4U;++field)progress.Tap(bank,Button::O);
    admitChord(progress);progress.Advance(2000U);
    Check(progress.Test().GetOperatorHoldProgress()>0U,"non-optical stage publishes real proportional hold progress");
    progress.Capture("Hall_same_bank_hold_visible_progress", "continue same-bank hold to5s or release either to cancel");
    progress.Down(bank,pressOFirst?Button::O:Button::X,false);
    Check(progress.Test().GetOperatorHoldProgress()==0U&&progress.Test().GetStep()==Mode::Step::HallSensors,"cancel restores local Hall cue without navigation");
    progress.Capture("Hall_same_bank_cancel_restores_local_cue", "release remaining control; historical Hall work remains");
    progress.Down(bank,pressOFirst?Button::X:Button::O,false);
    progress.Capture("Hall_same_bank_cancel_fully_released", "fresh control releases may navigate now");
    if (bank == 1U && !pressOFirst) for(bool crossOFirst : {false,true}){
        Board mixed(revision, rotate);
        mixed.Tap(1U,Button::O); // Green: either release order can be observed without red-boundary saturation.
        mixed.Down(1U, Button::X, true); mixed.Down(2U, Button::O, true); mixed.Advance(5100U);
        Check(mixed.Test().IsActive(), "cross-bank X/O cannot grant same-bank override");
        mixed.Capture("cross_bank_held_no_override", "release controls; independent fresh X/O releases may navigate");
        mixed.Down(crossOFirst?2U:1U,crossOFirst?Button::O:Button::X,false);
        Check(mixed.Test().GetLedChannel()==(crossOFirst?2U:0U),"first cross-bank fresh release performs ordinary navigation");
        mixed.Capture(crossOFirst?"cross_bank_O_first_release":"cross_bank_X_first_release", "release remaining control; ordinary navigation remains eligible");
        mixed.Down(crossOFirst?1U:2U,crossOFirst?Button::X:Button::O,false);
        Check(mixed.Test().IsActive()&&mixed.Test().GetLedChannel()==1U,"both cross-bank release orders finish ordinary independent navigation");
        mixed.Capture(crossOFirst?"cross_bank_O_then_X_released":"cross_bank_X_then_O_released", "continue ordinary factory inspection; no same-bank chord accepted");
    }
}

struct MotionTransport {
    enum class Failure { None, Nack, ShortResponse, WrongIdentity };
    std::array<uint8_t, 256> registers{};
    Failure failure = Failure::None;
    bool failWrites = false;
    uint32_t reads = 0U;
    uint32_t writes = 0U;
    uint32_t delays = 0U;
    bool interruptHigh = true;

    static bool Read(void* context, uint8_t address, uint8_t reg, uint8_t* data, size_t length) {
        auto& self = *static_cast<MotionTransport*>(context);
        ++self.reads;
        if (address != hardware::Lis2dw12::I2C_ADDRESS ||
            self.failure == Failure::Nack || self.failure == Failure::ShortResponse) return false;
        for (size_t i = 0U; i < length; ++i) data[i] = self.registers[static_cast<uint8_t>(reg + i)];
        if (reg == hardware::lis2dw12_register::WHO_AM_I && self.failure == Failure::WrongIdentity) data[0] = 0U;
        return true;
    }
    static bool Write(void* context, uint8_t, uint8_t reg, const uint8_t* data, size_t length) {
        auto& self = *static_cast<MotionTransport*>(context);
        ++self.writes;
        if (self.failWrites) return false;
        for (size_t i = 0U; i < length; ++i) self.registers[static_cast<uint8_t>(reg + i)] = data[i];
        return true;
    }
    static void Delay(void* context, uint32_t ms) { static_cast<MotionTransport*>(context)->delays += ms; }
    static void Configure(void*, uint8_t) {}
    static bool InterruptHigh(void* context, uint8_t) { return static_cast<MotionTransport*>(context)->interruptHigh; }
    hardware::Lis2dw12Transport Transport() { return {this, &Read, &Write, &Delay}; }
    firmware::MotionWakePlatform Platform() { return {this, &Configure, &InterruptHigh}; }
};

struct FactoryMotion {
    MotionTransport bus;
    fxe::SettingsStore settings;
    firmware::MotionWakeController controller;
    FactoryMotion() : settings([] {
        fxe::SettingsStoreConfig c;
        c.path = "build/test_factory_motion_currentness.json";
        return c;
    }()) {
        bus.registers[hardware::lis2dw12_register::WHO_AM_I] = hardware::Lis2dw12::EXPECTED_DEVICE_ID;
    }
    bool Begin(hardware::BoardRevision revision) {
        return controller.Begin(revision, bus.Transport(), bus.Platform(), settings);
    }
    bool CurrentPresence() {
        const bool updateOk = controller.Update(settings);
        return controller.CheckFactoryPresence(updateOk);
    }
    void Observe(Board& board, const char* input, bool completeRadioProof = false) {
        board.input = input;
        struct Ingress { FactoryMotion* motion; bool completeRadioProof; } ingress{this, completeRadioProof};
        board.session.RunManufacturingIngress([](void* context, Mode& mode, uint32_t now) {
            const auto& ingress = *static_cast<Ingress*>(context);
            auto& motion = *ingress.motion;
            if (ingress.completeRadioProof) {
                mode.ReportRadioVerified(modules::FactoryRadioChannel::Ble, now);
            }
            mode.ReportMotionPresence(motion.CurrentPresence(), now);
        }, &ingress);
        std::printf("[FACTORY_MOTION] revision=%u time=%u input=%s IRQ_high=%u reads=%u writes=%u present=%u failed=%u radios=%u stage=%u\n",
            unsigned(board.revision), board.Now(), input, bus.interruptHigh, bus.reads, bus.writes,
            board.Test().GetProgress().motionPresent, board.Test().GetProgress().failedAutomatedCheckMask,
            board.Test().GetProgress().radioChannelMask, unsigned(board.Test().GetStage()));
    }
};

void MotionCurrentnessJourney(hardware::BoardRevision revision, bool rotate) {
    for (auto fault : {MotionTransport::Failure::Nack, MotionTransport::Failure::ShortResponse,
                       MotionTransport::Failure::WrongIdentity}) {
        Board b(revision, rotate);
        FactoryMotion motion;
        Check(motion.Begin(revision), "motion currentness: actual controller boot/configuration succeeds");
        motion.Observe(b, "motion_actual_boot_healthy");
        b.Local();
        const auto reads = motion.bus.reads, writes = motion.bus.writes, delays = motion.bus.delays;
        const auto settingsRevision = motion.settings.GetRevision();
        Check(motion.controller.Update(motion.settings) && motion.bus.reads == reads,
              "ordinary high-IRQ update remains free of extra I2C reads");
        motion.bus.failure = fault;
        const char* input = fault == MotionTransport::Failure::Nack ? "motion_WHO_AM_I_NACK_after_good_boot"
            : fault == MotionTransport::Failure::ShortResponse ? "motion_checked_transport_short_response_after_good_boot"
            : "motion_wrong_WHO_AM_I_after_good_boot";
        // Shipping order: queued radio proof, current sensor check, fresh Hall,
        // persistence admission. The ingress supplies that Hall after callback.
        motion.Observe(b, input, true);
        b.Advance(0U);
        const auto p = b.Test().GetProgress();
        const uint8_t motionBit = 1U << static_cast<uint8_t>(modules::ManufacturingTestLocalCheck::MotionSensor);
        modules::HardwareTestRecord record;
        Check(motion.bus.reads == reads + 1U && motion.bus.writes == writes && motion.bus.delays == delays &&
              motion.settings.GetRevision() == settingsRevision,
              "factory currentness probes one checked identity read without reconfiguration or settings mutation");
        Check(!p.motionPresent && (p.failedAutomatedCheckMask & motionBit) && p.radioChannelMask == 0U &&
              b.Test().GetStage() != Stage::Persisting && !b.Test().BuildPendingRecord(record),
              "post-good-boot loss invalidates motion/final radio authority and cannot queue PASS");
        const auto topology = hardware::BoardLedTopologyFor(revision);
        const auto pixel = b.session.PhysicalFrame().pixels[40U * topology.physicalLedsPerSquare];
        Check(pixel.r && !pixel.g && !pixel.b && !pixel.w, "actual motion observation paints fixed physical A3 red");
        b.Capture("motion_transport_loss_after_good_boot_blocks_PASS", "repair motion communication; new good read and fresh Hall/radio proof required");
        b.session.RunManufacturingIngress([](void*, Mode& mode, uint32_t now) {
            mode.SetDiagnosticFault("motion", now); mode.SetDiagnosticFault("clear", now);
        }, nullptr);
        Check(!b.Test().GetProgress().motionPresent && !b.Test().BuildPendingRecord(record),
              "fault clear cannot erase actual transport loss or fabricate PASS");
        b.Advance(11000U);
        motion.Observe(b, "motion_still_missing_after_deadline");
        Check(!b.Test().GetProgress().motionPresent && !b.Test().BuildPendingRecord(record),
              "failed identity stays unsuccessful beyond the qualification deadline");
        b.Capture("motion_transport_loss_after_deadline", "repair communication; transient failure must remain retryable");
        motion.bus.failure = MotionTransport::Failure::None;
        b.session.SetManufacturingHallSnapshot(false, 0ULL);
        motion.Observe(b, "motion_actual_identity_recovered");
        Check(b.Test().GetProgress().motionPresent && motion.controller.IsAvailable() &&
              motion.bus.writes == writes && motion.bus.delays == delays && !b.Test().BuildPendingRecord(record),
              "fresh identity recovers prior valid configuration without reconfiguration or stale radio PASS");
        b.Capture("motion_transport_recovery_requires_fresh_authority", "repeat BLE proof and obtain a fresh empty Hall observation");
        Report(b, modules::FactoryRadioChannel::Ble);
        Check(!b.Test().GetProgress().hallSnapshotValid && !b.Test().BuildPendingRecord(record),
              "recovered motion and new radio proof cannot reuse a stale empty Hall observation");
        b.Capture("motion_recovery_radio_waits_for_valid_empty_Hall", "obtain a fresh valid empty board observation before save");
        b.Observe(0ULL);
        motion.Observe(b, "motion_current_links_and_BLE_reconfirmed", true);
        Check(b.Test().GetStage() == Stage::Persisting, "recovery requires new radio proof and final empty Hall before save");
        b.Save(); b.Capture("motion_transport_recovery_saved_PASS", "genuine current sensor qualification durably acknowledged");
        b.session.RunManufacturingIngress([](void*, Mode& mode, uint32_t now) { mode.Stop(); mode.Enter(mode.GetRevision(), now); }, nullptr);
        motion.bus.failure = fault;
        motion.Observe(b, "motion_new_run_actual_transport_missing");
        Check(!b.Test().GetProgress().motionPresent && !b.Test().BuildPendingRecord(record),
              "new run cannot inherit successful prior identity evidence");
    }
}

void MotionInitializationAndRevisionGates() {
    FactoryMotion eventFailure;
    Check(eventFailure.Begin(hardware::BoardRevision::V3S3), "event-failure fixture initializes");
    eventFailure.bus.interruptHigh = false;
    eventFailure.bus.failure = MotionTransport::Failure::Nack;
    const auto eventReads = eventFailure.bus.reads;
    Check(!eventFailure.CurrentPresence() && eventFailure.bus.reads == eventReads + 1U,
          "failed current runtime update blocks qualification without a second transaction");
    eventFailure.bus.interruptHigh = true;
    Check(!eventFailure.CurrentPresence(), "released IRQ cannot erase continuing transport failure");
    eventFailure.bus.failure = MotionTransport::Failure::None;
    Check(eventFailure.CurrentPresence(), "current successful identity recovers transient event-read failure");
    FactoryMotion unsupported;
    Check(unsupported.Begin(hardware::BoardRevision::V2S3) && !unsupported.CurrentPresence() &&
          unsupported.bus.reads == 0U && unsupported.bus.writes == 0U,
          "V2 never probes an unfitted motion sensor");
    FactoryMotion failed;
    failed.bus.failWrites = true;
    Check(!failed.Begin(hardware::BoardRevision::V3S3), "failed motion initialization is rejected");
    failed.bus.failWrites = false;
    const auto reads = failed.bus.reads;
    Check(!failed.CurrentPresence() && failed.bus.reads == reads,
          "identity alone cannot convert failed configuration into qualified presence");
    Check(failed.Begin(hardware::BoardRevision::V3S3) && failed.CurrentPresence(),
          "explicit successful initialization restores valid qualification");
    FactoryMotion disabled;
    disabled.settings.Doc()["MOTION_WAKE_ENABLED"] = false;
    disabled.settings.MarkDirty();
    Check(disabled.Begin(hardware::BoardRevision::V3_3S3) && disabled.CurrentPresence() &&
          disabled.controller.GetSensor().GetAppliedConfig().odrTenthsHz == 0U,
          "user-disabled wake remains a fitted sensor and requires current identity");
    disabled.bus.failure = MotionTransport::Failure::Nack;
    Check(!disabled.CurrentPresence(), "disabled wake cannot hide current sensor communication loss");
}

void AmbientCommunicationFailureJourney(hardware::BoardRevision revision,bool rotate){
    Board b(revision,rotate);const auto topology=hardware::BoardLedTopologyFor(revision);
    for(unsigned field=0U;field<4U;++field)b.Tap(1U,Button::O);
    b.Observe(UINT64_MAX);b.Observe(0ULL);b.Tap(1U,Button::O);
    const auto observe=[&](uint8_t mask,uint32_t lux0,uint32_t lux1){
        struct Sample{uint8_t mask;uint32_t lux0,lux1;} sample{mask,lux0,lux1};
        b.input="ALS_current_links_"+std::to_string(mask);
        b.session.RunManufacturingIngress([](void* p,Mode& mode,uint32_t now){const auto& s=*static_cast<Sample*>(p);
            mode.ReportLightSensorLinks(s.mask&1U,s.mask&2U,now);mode.ObserveLightSample({s.mask,{s.lux0,s.lux1}},now);},&sample);
    };
    const auto red=[&](uint8_t square){bool yes=true;for(uint8_t slot=0U;slot<topology.physicalLedsPerSquare;++slot){const auto c=b.session.PhysicalFrame().pixels[square*topology.physicalLedsPerSquare+slot];yes&=c.r>0U&&!c.g&&!c.b&&!c.w;}return yes;};
    const auto cornerCue = [&](uint8_t square, bool qualified) {
        for (uint8_t slot = 0U; slot < topology.physicalLedsPerSquare; ++slot) {
            const auto c = b.session.PhysicalFrame().pixels[square * topology.physicalLedsPerSquare + slot];
            const bool inward = (slot % 2U) != 0U;
            if (qualified && inward) {
                if (c.r || !c.g || c.b || c.w) return false;
            } else if (!qualified && !inward) {
                if (c.r <= c.g || !c.g || c.b || c.w) return false;
            } else if (c.r || c.g || c.b || c.w) {
                return false;
            }
        }
        return true;
    };
    const auto dark=[&](uint8_t square){const auto c=b.session.PhysicalFrame().pixels[square*topology.physicalLedsPerSquare];return !(c.r||c.g||c.b||c.w);};
    for(uint8_t missing:{0U,1U}){
        const uint8_t corners[]={7U,56U};const uint8_t healthy=1U-missing,mask=3U&~(1U<<missing);
        observe(mask,400U,800U);
        Check(red(corners[missing]) && cornerCue(corners[healthy], false),
              "the failed ALS corner is all red while the healthy pending corner uses amber rails");
        modules::HardwareTestRecord record;Check(!b.Test().BuildPendingRecord(record),"missing communicating ALS cannot create PASS");
        b.Capture(missing?"ALS_missing_0x48_A1_red_other_H8_pending":"ALS_missing_0x10_H8_red_other_A1_pending",
                  "repair named sensor communication; amber rails still request a light response on the other sensor");
        observe(mask,0U,0U);
        Check(red(corners[missing]) && cornerCue(corners[healthy], false),
              "valid zero lux remains visibly pending and is distinct from a link fault");
        observe(3U,400U,800U);
        Check(cornerCue(7U, false) && cornerCue(56U, false), "renewed communication restores both pending cues");
        b.Capture("ALS_real_links_recovered_pending","continue Buttons; renewed communication alone does not grant a response");
    }
    for(uint8_t bank:{1U,2U})for(Button button:{Button::Up,Button::Down,Button::Left,Button::Right,Button::X,Button::O})b.Tap(bank,button);
    Check(b.Test().GetStep() == Mode::Step::Buttons && b.Test().GetStage() == Stage::Local &&
          b.Test().GetProgress().buttonsComplete && cornerCue(7U, false) && cornerCue(56U, false),
          "complete controls stay local while the physical A1 and H8 response cues are both pending");
    b.Capture("all_buttons_released_both_ALS_pending", "change the light at each corner sensor");
    b.Down(1U, Button::X, true);
    b.Down(1U, Button::O, true);
    for (uint8_t sample = 0U; sample < 3U; ++sample) observe(3U, 400U, 400U);
    b.Advance(1200U);
    for (uint8_t sample = 0U; sample < 3U; ++sample) observe(3U, 100U, 100U);
    Check(b.Test().GetOperatorHoldProgress() != 0U && b.Test().GetProgress().lightSensorResponseMask == 0U,
          "even a sustained change during the visible X+O hold cannot bank either response");
    b.Capture("held_XO_visible_progress_no_ALS_proof", "release both controls to cancel the override hold");
    b.Down(1U, Button::X, false);
    for (uint8_t sample = 0U; sample < 3U; ++sample) observe(3U, 400U, 400U);
    b.Down(1U, Button::O, false);
    for (uint8_t sample = 0U; sample < 3U; ++sample) observe(3U, 400U, 400U);
    Check(b.Test().IsActive() && b.Test().GetStep() == Mode::Step::Buttons && b.Test().GetStage() == Stage::Local &&
          b.Test().GetProgress().lightSensorResponseMask == 0U && cornerCue(7U, false) && cornerCue(56U, false),
          "canceling the chord leaves both amber cues pending after the lights settle on the same valid Hall");
    b.Capture("canceled_XO_both_ALS_still_pending", "with controls released, make a fresh light change at each sensor");
    for (uint8_t sample = 0U; sample < 3U; ++sample) observe(2U, 0U, 100U);
    Check(red(7U) && b.Test().GetProgress().lightSensorResponseMask == 0U && cornerCue(56U, false),
          "a current ALS fault blocks response collection even on the communicating sensor");
    for (uint8_t sample = 0U; sample < 3U; ++sample) observe(3U, 400U, 400U);
    Check(b.Test().GetProgress().lightSensorResponseMask == 0U && cornerCue(7U, false) && cornerCue(56U, false),
          "recovering the fault cue gives both sensors a new baseline and neither a completed response");
    b.Capture("ALS_fault_cleared_responses_still_pending", "perform a fresh light change after the fault clears");
    b.Tap(1U, Button::Up);
    for (uint8_t sample = 0U; sample < 3U; ++sample) observe(3U, 0U, 400U);
    Check(b.Test().GetProgress().lightSensorResponseMask == 0U,
          "the button light change resets pending baselines instead of granting an H8 response");
    for (uint8_t sample = 0U; sample < 3U; ++sample) observe(3U, 100U, 400U);
    Check(b.Test().GetProgress().lightSensorResponseMask == 0x01U && cornerCue(7U, true) && cornerCue(56U, false),
          "only H8 proof qualifies and lights the inward green marker. A1 stays amber and local");
    b.Capture("H8_response_qualified_A1_pending", "keep controls released and change the light at A1");
    for (uint8_t sample = 0U; sample < 3U; ++sample) observe(3U, 100U, 100U);
    Check(b.Test().GetProgress().lightSensorResponseMask == 0x01U && b.Test().GetStep() == Mode::Step::Buttons,
          "the first marker change rebaselines the other sensor without crediting the display change as a response");
    for (uint8_t sample = 0U; sample < 3U; ++sample) observe(3U, 100U, 400U);
    Check(b.Test().GetStep() == Mode::Step::Radio && b.Test().GetStage() == Stage::Radio &&
          b.Test().GetProgress().lightSensorResponseMask == 0x03U && b.NavigationDark(),
          "second ALS response advances to authoritative Radio on fresh empty Hall without another O");
    b.Capture("both_ALS_responses_start_Radio", "wait for the actual wireless proof");
    observe(0U,400U,800U);Check(red(7U)&&red(56U),"Radio lost ambient links show both physical red fault corners");
    b.Capture("ALS_Radio_both_missing_red","repair both light links; central radio cue cannot grant PASS while electronics are missing");
    b.Advance(11000U);Check(b.Test().GetStep()==Mode::Step::Failed&&red(7U)&&red(56U),"failed-only ambient check has explicit red corners after deadline");
    b.Capture("ALS_failed_deadline_both_red","repair communicating sensors; failed-only view is not blank");
    observe(3U,400U,800U);b.Observe(0ULL);
    Check(b.Test().GetStep()==Mode::Step::Radio&&dark(7U)&&dark(56U),
          "recovered healthy sensors leave Radio corners dark. Local response markers belong to Buttons");
    b.Capture("ALS_Radio_recovered_no_corner_marks","wait for new actual peer proof");
}
void SensorFaultAndAmbientJourney(hardware::BoardRevision revision, bool rotate) {
    Board b(revision, rotate);
    struct Observation { uint8_t expanders; bool motion; uint32_t lux0; uint32_t lux1; };
    const auto observe = [&](Observation value) {
        b.input = "current_sensor_observation";
        b.session.RunManufacturingIngress([](void* p, Mode& mode, uint32_t now) {
            const auto& o = *static_cast<Observation*>(p);
            mode.ReportExpanderLinks(o.expanders, now);
            mode.ReportMotionPresence(o.motion, now);
            mode.ReportLightSensorLinks(true, true, now);
            mode.ObserveLightSample({3U, {o.lux0, o.lux1}}, now);
        }, &value);
    };
    const auto fault = [&](const char* target, bool expected = true) {
        struct Request { const char* target; bool accepted = false; } request{target};
        b.input = std::string("hwtest_fault_") + target;
        b.session.RunManufacturingIngress([](void* p, Mode& mode, uint32_t now) {
            auto& r = *static_cast<Request*>(p);
            r.accepted = mode.SetDiagnosticFault(r.target, now);
        }, &request);
        b.requestInput = b.input;
        b.requestOutcome = request.accepted ? "request_accepted" : "request_rejected";
        Check(request.accepted == expected, "manufacturing fault owner accepts only defined unsaved targets; Console grammar bypassed");
        Check(std::string(b.CaptureAck()) == (expected ? "request_accepted" : "request_rejected"),
              "readable playback ACK records actual fault request outcome independently from qualification stage");
    };
    const auto phase = [&](uint32_t desired) {
        const uint32_t current = b.Now() % 800U;
        b.Advance((desired + 800U - current) % 800U);
    };
    const auto red = [&](uint16_t pixel) {
        const auto c = b.session.PhysicalFrame().pixels[pixel];
        return c.r != 0U && c.g == 0U && c.b == 0U && c.w == 0U;
    };
    const auto black = [&](uint16_t pixel) {
        const auto c = b.session.PhysicalFrame().pixels[pixel];
        return c.r == 0U && c.g == 0U && c.b == 0U && c.w == 0U;
    };
    const auto topology = hardware::BoardLedTopologyFor(revision);
    observe({15U, true, 100U, 200U});
    fault("motion"); fault("expander1");
    b.Capture("sensor_fault_optical_field_exclusive", "R/G/B/W inspection owns every output; failures still block PASS");
    Check(b.Test().GetStep() == Mode::Step::LedRainbow, "negative hardware flags do not obscure optical inspection");
    const auto optical = b.session.PhysicalFrame();
    for(uint16_t n=0U;n<optical.led_count;++n) Check(red(n), "every fitted optical red pixel remains red under negative fault");
    fault("clear");
    for(unsigned n=0U;n<4U;++n) b.Tap(1U, Button::O);
    b.Observe(UINT64_MAX); b.Observe(0ULL); b.Tap(1U, Button::O);
    Check(b.Test().GetStep() == Mode::Step::Buttons, "actual raw O and Hall input reach button test");
    // Individual lux readings remain available without granting response proof.
    for(uint32_t lux : {100U, 400U, 900U}) {
        observe({15U, true, lux, 200U});
        const auto progress = b.Test().GetProgress();
        Check(progress.lightSensors[0].lastLux == lux && progress.lightSensors[1].lastLux == 200U,
              "0x10 live H8 lux remains independent from the 0x48 A1 reading");
        b.Capture("ALS_0x10_H8_intermediate_lux", "keep a stable light change at the pending H8 sensor");
    }
    for(uint32_t lux : {100U, 400U, 900U}) {
        observe({15U, true, 200U, lux});
        const auto progress = b.Test().GetProgress();
        Check(progress.lightSensors[1].lastLux == lux && progress.lightSensors[0].lastLux == 200U,
              "0x48 live A1 lux remains independent from the 0x10 H8 reading");
        b.Capture("ALS_0x48_A1_intermediate_lux", "keep a stable light change at the pending A1 sensor");
    }
    Check(b.Test().GetProgress().lightSensorResponseMask == 0U,
          "unsettled lux readings cannot fabricate a completed response on either sensor");
    fault("motion"); b.Capture("motion_forced_missing_A3_solid_red", "motion presence failed; clear fault to restore actual presence");
    Check(red(40U*topology.physicalLedsPerSquare), "missing motion paints fixed physical A3 solid red");
    phase(400U); Check(red(40U*topology.physicalLedsPerSquare), "motion A3 remains solid across expander blink cadence");
    fault("clear"); Check(b.Test().GetProgress().motionPresent, "clear restores only actual present motion");
    observe({15U, false, 200U, 200U}); fault("motion"); fault("clear");
    Check(!b.Test().GetProgress().motionPresent && red(40U*topology.physicalLedsPerSquare), "clear cannot fabricate presence for an actually missing motion sensor");
    observe({15U, true, 200U, 200U});
    constexpr uint8_t squares[] = {8U,15U,48U,55U};
    constexpr uint8_t status[] = {30U,9U,25U,14U};
    for(unsigned index=0U;index<4U;++index) {
        const std::string target="expander"+std::to_string(index+1U);
        fault(target.c_str()); phase(0U);
        const uint8_t chain=revision==hardware::BoardRevision::V3_3S3?hardware::V3_3PhysicalIndexForSemanticStatus(status[index]):status[index];
        const uint16_t square=squares[index]*topology.physicalLedsPerSquare, adjacent=topology.physicalBoardLedCount+chain;
        Check(red(square)&&black(adjacent), "missing expander first phase: named square red, adjacent status dark");
        b.Capture("expander_forced_missing_square_phase", "inspect named A7/H7/A2/H2 marker; adjacent perimeter dark");
        phase(400U); Check(black(square)&&red(adjacent), "missing expander second phase: square dark, adjacent status red after production map");
        b.Capture("expander_forced_missing_adjacent_status_phase", "adjacent perimeter red after400ms; square dark");
        modules::HardwareTestRecord record; Check(!b.Test().BuildPendingRecord(record), "forced negative health never queues a PASS record");
        fault("clear"); observe({15U,true,200U,200U});
        Check(b.Test().GetProgress().expanderLinkMask==15U, "clear recovers only real communicating expander observations");
        b.Capture("expander_fault_clear_real_health_recovered", "resume button qualification; historical input evidence retained");
    }
    observe({14U,true,200U,200U}); fault("clear");
    Check(b.Test().GetProgress().expanderLinkMask==14U, "clear cannot make real missing expander communicate");
    observe({15U,true,200U,200U});
    fault("motion"); for(const char* target : {"expander1","expander2","expander3","expander4"}) fault(target);
    phase(0U); Check(b.Test().GetProgress().diagnosticFaultMask==31U, "all negative fault flags accumulate without clearing one another");
    const uint8_t before=b.Test().GetProgress().diagnosticFaultMask;
    fault("expander0",false);
    b.Capture("invalid_expander_target_rejected", "undefined target rejected; existing faults unchanged; use hwtest fault clear to recover");
    fault("pass",false);
    b.Capture("invalid_pass_target_rejected", "PASS target rejected; no success granted; extra-argument Console grammar is not executed here");
    Check(b.Test().GetProgress().diagnosticFaultMask==before, "invalid fault target is atomic without state mutation");
    b.Capture("all_sensor_faults_no_false_PASS", "all four mapped markers alternate and A3 stays solid; clear or exit");
    fault("clear");
    Check(b.Test().GetProgress().diagnosticFaultMask==0U, "valid clear recovers after rejected requests without fabricating health");
    b.Capture("invalid_fault_request_clear_recovery", "clear accepted by manufacturing owner; physical Console extra-argument rejection remains a separate ingress check");
    fault("motion"); for(const char* target : {"expander1","expander2","expander3","expander4"}) fault(target);

    b.Down(1U,Button::X,true); b.Down(1U,Button::O,true); b.Advance(5100U);
    Check(!b.Test().IsActive()&&b.Test().GetProgress().diagnosticFaultMask==0U&&b.Test().IsOverridePending(), "threshold exit clears RAM faults and preserves truthful Override receipt");
    b.Capture("sensor_fault_override_exit_held_tail", "normal firmware now; release both held controls; Override save still pending");
    b.Down(1U,Button::X,false); b.Down(1U,Button::O,false);
    b.session.RunManufacturingIngress([](void*,Mode& m,uint32_t now){m.Enter(m.GetRevision(),now);},nullptr);
    Check(b.Test().GetProgress().diagnosticFaultMask==0U, "new run cannot inherit RAM fault flags");
    b.Capture("sensor_fault_new_run_clear", "fresh R/G/B/W qualification; no synthetic failure remains");
    observe({15U,false,200U,200U}); b.Advance(11000U);
    for(unsigned n=0U;n<4U;++n) b.Tap(1U,Button::O);
    Check(b.Test().GetStep()==Mode::Step::Failed && red(40U*topology.physicalLedsPerSquare),
          "missing-at-start motion after deadline permits optical review then shows A3 fault");
    b.Capture("missing_at_start_motion_after_optical_review", "actual missing motion remains A3 red; repair sensor and restart qualification");

}
}
int main() {
    for(auto revision:{hardware::BoardRevision::V2S3,hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3})
        for(uint8_t bank:{1U,2U}) for(bool rotate:{false,true}) ButtonFirstXJourney(revision,bank,rotate);
    for(auto revision:{hardware::BoardRevision::V2S3,hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3})
        for(bool rotate:{false,true})ButtonFootprintAndCalmJourney(revision,rotate);
    for(auto revision:{hardware::BoardRevision::V2S3,hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3})for(bool rotate:{false,true})FinalControlsToRadioSquareJourney(revision,rotate);
    for(auto revision:{hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3})for(bool rotate:{false,true})ConsecutiveXAndAutomaticHallJourney(revision,rotate);
    for(auto revision:{hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3})for(bool rotate:{false,true})EarlyReviewCannotStrandLocalJourney(revision,rotate);
    for(auto revision:{hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3})for(bool rotate:{false,true})WaitingRoleMotionJourney(revision,rotate);
    MotionInitializationAndRevisionGates();
    for(auto revision : {hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3})
        for(bool rotate : {false,true}) MotionCurrentnessJourney(revision,rotate);
    for (auto revision : {hardware::BoardRevision::V2S3, hardware::BoardRevision::V3S3, hardware::BoardRevision::V3_3S3}) NavigationAndOptics(revision);
    for(auto revision : {hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3})
        for(bool rotate : {false,true}) FinalHallSinglePieceRecovery(revision,rotate);
    for(auto revision : {hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3})
        for(bool rotate : {false,true}) SensorFaultAndAmbientJourney(revision,rotate);
    for(auto revision:{hardware::BoardRevision::V3S3,hardware::BoardRevision::V3_3S3})for(bool rotate:{false,true})AmbientCommunicationFailureJourney(revision,rotate);
    PendingProofRecoveryAndBatch();
    for (unsigned count : {2U, 4U, 6U}) for (bool collision : {false, true}) SimultaneousBatch(count, collision);
    for(auto revision : {hardware::BoardRevision::V3S3, hardware::BoardRevision::V3_3S3})
        for (uint8_t bank : {1U, 2U}) for (bool rotate : {false, true})
            for(bool pressOFirst : {false,true}) OverrideAndChordArbitration(revision, bank, rotate, pressOFirst);
    std::printf("[FACTORY_ROUND2] named production-equivalent journeys; GPIO electrical/RF scheduling/storage acknowledgement controlled; failures=%d\n", failures);
    return failures ? 1 : 0;
}
