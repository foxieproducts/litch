#include "bounded_hint_engine.hpp"
#include "virtual_litch_session.hpp"
#include "../../src/virtual_litch_session_internal.hpp"
#include "firmware/startup_led_sequence.hpp"
#include "modules/startup_piece_accent.hpp"
#include "modules/boot_display.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
#include "modules/boot_fox_asset.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "firmware/startup_console_policy.hpp"
#include "foxie_embedded/console_transport_memory.hpp"
#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/brightness_scale_policy.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/white_squares_setting.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/game_state_machine_render_state.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] %s\n", message); }
}
bool IsBlack(const virtual_litch_color& c) {
    return c.r == 0U && c.g == 0U && c.b == 0U && c.w == 0U;
}
uint32_t Sum(const virtual_litch_physical_frame& frame) {
    uint32_t total = 0U;
    for (uint16_t i = 0U; i < frame.led_count; ++i) {
        const auto& p = frame.pixels[i];
        total += p.r + p.g + p.b + p.w;
    }
    return total;
}

uint16_t PieceLevel(const virtual_litch_physical_frame& frame,
                    const hardware::BoardRevision revision,
                    const uint8_t square) {
    const auto targets = hardware::MapSemanticSquareLedTargets(
        revision, square, hardware::SquareLedSlot::Piece, false);
    uint16_t total = 0U;
    for (uint8_t target = 0U; target < targets.count; ++target) {
        const auto& pixel = frame.pixels[targets.indices[target]];
        total = static_cast<uint16_t>(
            total + pixel.r + pixel.g + pixel.b + pixel.w);
    }
    return total;
}
void EmptyBootFoxToLiveBoardV33(const bool duck = false) {
    constexpr uint64_t KING_SQUARES = (UINT64_C(1) << 4U) | (UINT64_C(1) << 60U);
    virtual_litch_config config{};virtual_litch_default_config(&config);
    config.hardware=VIRTUAL_LITCH_HARDWARE_V3_3;
    config.strong_power_source=1U;config.brightness_scale_256=128U;
    modules::litchcraft_frame_stream::FrameStream pendingMode;
    test_support::BoundedHintSession fixture(config), normalFixture(config);
    auto& board=fixture.session;auto& normal=normalFixture.session;
    board.SettingsForPersistenceTest().Doc()["MINIMAL_ANIMATIONS"]=true;
    normal.SettingsForPersistenceTest().Doc()["MINIMAL_ANIMATIONS"]=true;
    board.SettingsForPersistenceTest().Doc()["P1_AUTO_HINTS"]=true;
    const uint64_t opening = UINT64_C(0xFFFF00000000FFFF) |
        (duck ? (UINT64_C(1) << 27U) : 0ULL);
    Check(board.ObserveHallMask(opening)==VIRTUAL_LITCH_OK &&
              normal.ObserveHallMask(opening)==VIRTUAL_LITCH_OK,
          "firmware's first real Hall scan precedes boot-display initialization");
    const bool savedMode=board.Litchcraft().IsActive();
    const auto artGeneration=board.Litchcraft().CanvasGeneration();
    const auto initialSettingsRevision=board.SettingsForPersistenceTest().GetRevision();
    Check(board.Advance(100U)==VIRTUAL_LITCH_OK,"service handoff keeps input/render services running");
    pendingMode.BeginInitialRecovery();
    board.Litchcraft().AttachFrameStream(&pendingMode);
    board.BeginBootDisplay();
    board.BeginStartupPieceAccent();
    Check(board.ObserveHallMask(opening)==VIRTUAL_LITCH_OK,"raw complete initial Hall admits selected Fox");
    Check(normal.Advance(100U)==VIRTUAL_LITCH_OK && normal.ObserveHallMask(opening)==VIRTUAL_LITCH_OK,"baseline sees same occupancy");
    const auto ordinaryEdgeEnergy = [](const virtual_litch_physical_frame& frame) {
        uint32_t energy = 0U;
        for (uint8_t edge = 0U; edge < modules::EDGE_LED_COUNT; ++edge) {
            const auto& pixel = frame.pixels[256U + hardware::V3_3PhysicalIndexForSemanticStatus(edge)];
            energy += pixel.r + pixel.g + pixel.b + pixel.w;
        }
        return energy;
    };
    for (unsigned step = 0U; step < 20U; ++step) {
        Check(board.Advance(100U)==VIRTUAL_LITCH_OK && normal.Advance(100U)==VIRTUAL_LITCH_OK,
              "existing Store authority can remain unresolved for two seconds");
        Check(ordinaryEdgeEnergy(board.PhysicalFrame()) == 0U,
              "ordinary edge turn animation cannot precede Fox while saved mode is pending");
    }
    std::printf("[BOOT_EDGE_ORDER] t=%u mode=pending held_ms=2000 ordinary_edge=%lu fox_eye=%u next=resolve-mode bypass=SDK/storage-worker-duration/optics\n",
        board.State().now_ms, static_cast<unsigned long>(ordinaryEdgeEnergy(board.PhysicalFrame())),
        board.PhysicalFrame().pixels[102U].r);
    pendingMode.CompleteInitialRecovery();
    Check(board.ObserveHallMask(opening)==VIRTUAL_LITCH_OK,
          "resolved existing mode authority admits Fox from the unchanged Hall observation");
    bool boardBlack=true;
    for(uint16_t index=0U;index<256U;++index) boardBlack=boardBlack && IsBlack(board.PhysicalFrame().pixels[index]);
    Check(boardBlack && ordinaryEdgeEnergy(board.PhysicalFrame()) == 0U,
          "first accepted Fox frame keeps ordinary edge turn output behind the display");
    Check(board.Advance(100U)==VIRTUAL_LITCH_OK && normal.Advance(100U)==VIRTUAL_LITCH_OK,
          "the accepted black lead-in lasts 100ms before the Fox fade begins");
    Check(!duck || board.SettingsForPersistenceTest().GetRevision()>initialSettingsRevision,
          "derived Duck activation advances storage revision without cancelling the boot episode");
    for(uint16_t index=0U;index<256U;++index)
        Check(IsBlack(board.PhysicalFrame().pixels[index]), "board stays black through the 100ms lead-in");
    Check(board.Advance(125U)==VIRTUAL_LITCH_OK && normal.Advance(125U)==VIRTUAL_LITCH_OK,"time reaches the 250ms fade-in midpoint");
    const auto fadeInMidpoint=board.PhysicalFrame().pixels[102U];
    Check(board.Advance(125U)==VIRTUAL_LITCH_OK && normal.Advance(125U)==VIRTUAL_LITCH_OK,"time reaches full peak fox at 250ms");
    const auto full=board.PhysicalFrame();
    Check(ordinaryEdgeEnergy(full) == 0U,
          "visible Fox precedes ordinary edge turn animation");
    Check(board.EdgeCueIntent().kind != modules::EdgeLedCueKind::SetupComplete && !board.HintDisplayRequested(),
          "queued setup acknowledgement cannot replace the selected boot episode");
    Check(fadeInMidpoint.r > 0U && fadeInMidpoint.r < full.pixels[102U].r,
          "250ms fade-in has an actual intermediate V3.3 eye before peak");
    const auto mapped=modules::boot_fox_asset::PIXELS[102U];
    Check(full.pixels[102U].r!=0U || full.pixels[102U].g!=0U,"yellow eye survives actual V3.3 mapping and output gain");
    Check(full.pixels[102U].w==0U && mapped.r!=0U,"V3.3 output uses RGB");
    const auto expected=[](uint8_t channel) {
        const uint16_t bright=(uint16_t(channel)*128U+128U)/256U;
        return uint8_t((bright*128U+128U)/256U);
    };
    Check(!full.power_limited && full.pixels[102U].r==expected(mapped.r) &&
          full.pixels[102U].g==expected(mapped.g) && full.pixels[102U].b==expected(mapped.b),
          "peak logo passes exact normal brightness and independent V3.3 half-gain bytes");
    Check(board.Advance(500U)==VIRTUAL_LITCH_OK && normal.Advance(500U)==VIRTUAL_LITCH_OK,"ordinary loop reaches the old short-episode cutoff");
    const auto held = board.PhysicalFrame().pixels[102U];
    Check(held.r == full.pixels[102U].r && held.g == full.pixels[102U].g &&
              held.b == full.pixels[102U].b,
          "Fox is still fully visible at 750ms after actual output admission");
    Check(board.Advance(500U)==VIRTUAL_LITCH_OK && normal.Advance(500U)==VIRTUAL_LITCH_OK,
          "ordinary render reaches the longer fade midpoint");
    bool intermediate = false;
    for (uint16_t index=0U; index<256U; ++index) {
        const auto pixel = board.PhysicalFrame().pixels[index];
        const auto logo = full.pixels[index];
        const auto live = normal.PhysicalFrame().pixels[index];
        intermediate = intermediate ||
            (pixel.r > std::min(logo.r,live.r) && pixel.r < std::max(logo.r,live.r)) ||
            (pixel.g > std::min(logo.g,live.g) && pixel.g < std::max(logo.g,live.g)) ||
            (pixel.b > std::min(logo.b,live.b) && pixel.b < std::max(logo.b,live.b));
    }
    Check(intermediate,"500ms crossfade presents an actual intermediate V3.3 output");
    Check(ordinaryEdgeEnergy(board.PhysicalFrame()) == 0U,
          "ordinary edge turn output waits through the Fox crossfade");
    Check((normal.SidePrioritySquareMask() & KING_SQUARES) == KING_SQUARES &&
              (board.SidePrioritySquareMask() & KING_SQUARES) == 0ULL,
          "Fresh Setup king brackets cannot enter the live frame during Fox crossfade");
    std::printf("[BOOT_KING_HANDOFF] t=%u Hall=opening phase=fox-crossfade king_brackets=%u next=piece-introduction\n",
        board.State().now_ms, (board.SidePrioritySquareMask() & KING_SQUARES) != 0ULL);
    Check(board.Advance(250U)==VIRTUAL_LITCH_OK && normal.Advance(250U)==VIRTUAL_LITCH_OK,"ordinary loop reaches exact longer expiry");
    Check(board.EdgeCueIntent().kind != modules::EdgeLedCueKind::SetupComplete && !board.HintDisplayRequested(),
          "setup acknowledgement still waits for the ordinary piece introduction");
    Check(board.Advance(0U)==VIRTUAL_LITCH_OK,
          "the next ordinary frame starts piece introduction after Fox relinquishes output");
    Check(ordinaryEdgeEnergy(board.PhysicalFrame()) > 0U,
          "ordinary edge turn output resumes after Fox relinquishes output");
    std::printf("[BOOT_EDGE_ORDER] t=%u mode=Play fox=complete ordinary_edge=%lu next=piece-introduction\n",
        board.State().now_ms, static_cast<unsigned long>(ordinaryEdgeEnergy(board.PhysicalFrame())));
    std::printf("[BOOT_ORDER_HANDOFF] t=%u phase=%u cue=%u hint=%u first=%u last=%u\n",
        board.State().now_ms,board.State().interaction_phase,static_cast<unsigned>(board.EdgeCueIntent().kind),
        board.HintDisplayRequested(),PieceLevel(board.PhysicalFrame(),hardware::BoardRevision::V3_3S3,0U),
        PieceLevel(board.PhysicalFrame(),hardware::BoardRevision::V3_3S3,63U));
    Check(board.Advance(200U)==VIRTUAL_LITCH_OK &&
              board.EdgeCueIntent().kind != modules::EdgeLedCueKind::SetupComplete &&
              !board.HintDisplayRequested() &&
              PieceLevel(board.PhysicalFrame(),hardware::BoardRevision::V3_3S3,0U)>0U &&
              PieceLevel(board.PhysicalFrame(),hardware::BoardRevision::V3_3S3,63U)==0U,
          "pieces enter sequentially after Fox, before setup acknowledgement");
    Check((board.SidePrioritySquareMask() & KING_SQUARES) == 0ULL,
          "Fresh Setup king brackets wait for the sequential piece introduction");
    std::printf("[BOOT_ORDER_HANDOFF] t=%u phase=%u cue=%u hint=%u first=%u last=%u\n",
        board.State().now_ms,board.State().interaction_phase,static_cast<unsigned>(board.EdgeCueIntent().kind),
        board.HintDisplayRequested(),PieceLevel(board.PhysicalFrame(),hardware::BoardRevision::V3_3S3,0U),
        PieceLevel(board.PhysicalFrame(),hardware::BoardRevision::V3_3S3,63U));
    Check(board.Advance(250U)==VIRTUAL_LITCH_OK && board.Advance(0U)==VIRTUAL_LITCH_OK &&
              board.EdgeCueIntent().kind == modules::EdgeLedCueKind::SetupComplete &&
              board.EdgeCueIntent().startMs >= 2100U &&
              board.State().now_ms-board.EdgeCueIntent().startMs<100U,
          "retained Ready starts its full cue only after boot and piece introduction complete");
    Check((board.SidePrioritySquareMask() & KING_SQUARES) == KING_SQUARES,
          "both king brackets appear when startup hands the frame to Fresh Setup");
    std::printf("[BOOT_KING_HANDOFF] t=%u Hall=opening phase=ready king_brackets=%u next=opening-move\n",
        board.State().now_ms, (board.SidePrioritySquareMask() & KING_SQUARES) == KING_SQUARES);
    Check(board.Litchcraft().IsActive()==savedMode && board.Litchcraft().CanvasGeneration()==artGeneration,"intro does not Enter Litchcraft or mutate artwork");
    std::printf("[EmptyBootFoxToLiveBoardV33] Hall=complete_initial32 duck=%u mode=%s accepted=1 black=100ms fade=250ms logo=1000ms crossfade=500ms pieces=400ms ready_start=%u current_ma=%u text=%.*s next=normal-mode art_unchanged=1 bypass=SDK/radio/optics\n", duck, savedMode?"Litchcraft":"Play", board.EdgeCueIntent().startMs,full.applied_led_current_ma,static_cast<int>(board.DisplayText().size()),board.DisplayText().data());
    // A first occupied observation must stay retired even if later emptied.
    board.BeginBootDisplay();
    Check(board.ObserveHallMask(1ULL)==VIRTUAL_LITCH_OK && board.ObserveHallMask(0ULL)==VIRTUAL_LITCH_OK,"occupied/empty scans remain ordinary authoritative input");
    Check(board.Advance(100U)==VIRTUAL_LITCH_OK,"occupied boot does not rearm on removal");
}

void BootFoxZeroAndResumeV33() {
    for (const bool rotate : {false, true}) {
        virtual_litch_config config{};
        virtual_litch_default_config(&config);
        config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
        config.board_rotate = rotate ? 1U : 0U;
        config.square_rotate_for_output = rotate ? 1U : 0U;
        config.strong_power_source = 1U;
        config.brightness_scale_256 = modules::brightness_scale_policy::ComputeEffectiveScale256(
            0U, false, 0.0F, 256U);
        test_support::BoundedHintSession fixture(config);
        auto& board = fixture.session;
        board.SettingsForPersistenceTest().Doc()["BRIGHTNESS"] = 0U;
        board.SettingsForPersistenceTest().Doc()["MINIMAL_ANIMATIONS"] = true;
        board.BeginBootDisplay();
        board.ObserveBootHallMask(UINT64_C(1) << 18U);
        Check(board.Advance(100U) == VIRTUAL_LITCH_OK, "zero-level startup retains its black lead-in");
        Check(board.Advance(125U) == VIRTUAL_LITCH_OK, "zero-level startup reaches its fade midpoint");
        const auto midpoint = board.PhysicalFrame();
        Check(board.Advance(125U) == VIRTUAL_LITCH_OK, "zero-level startup reaches the full fox");
        const auto peak = board.PhysicalFrame();
        const uint16_t eye = rotate ? (63U - 102U / 4U) * 4U + (102U % 4U + 2U) % 4U : 102U;
        unsigned nonblack = 0U;
        uint32_t peakEnergy = 0U, midpointEnergy = 0U;
        for (uint16_t led = 0U; led < 256U; ++led) {
            if (!IsBlack(peak.pixels[led])) ++nonblack;
            peakEnergy += peak.pixels[led].r + peak.pixels[led].g + peak.pixels[led].b;
            midpointEnergy += midpoint.pixels[led].r + midpoint.pixels[led].g + midpoint.pixels[led].b;
        }
        Check(nonblack >= 32U && peak.pixels[eye].r > 0U && peakEnergy > midpointEnergy,
              "actual brightness-zero RGB mapping keeps the fox and an intermediate fade");
        Check(board.SettingsForPersistenceTest().ReadValue("BRIGHTNESS", 8U) == 0U,
              "the startup display preserves the saved near-dark preference");
        std::printf("[BootFoxZeroAndResumeV33] level=0 rotate=%u Hall=partial accepted=1 "
                    "peak_nonblack=%u eye_r=%u midpoint_energy=%lu peak_energy=%lu next=live-board "
                    "bypass=reset/SDK/optical-visibility\n", rotate, nonblack, peak.pixels[eye].r,
                    static_cast<unsigned long>(midpointEnergy), static_cast<unsigned long>(peakEnergy));
    }
    constexpr const char* fen = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1";
    for (const unsigned scenario : {0U, 1U, 2U, 3U}) {
        const bool moved = (scenario & 1U) != 0U;
        const bool unfinished = (scenario & 2U) != 0U;
        virtual_litch_config config{};
        virtual_litch_default_config(&config);
        config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
        config.strong_power_source = 1U;
        config.brightness_scale_256 = 128U;
        test_support::BoundedHintSession fixture(config), baselineFixture(config);
        auto& board = fixture.session;
        auto& normal = baselineFixture.session;
        struct ResumeCase { uint64_t expected; bool unfinished; } resume{0ULL, unfinished};
        for (auto* session : {&board, &normal}) {
            if (!unfinished)
                Check(session->LoadExternalFen(fen), "seed an actual saved ongoing position through its production owner");
            session->RunControlIngress([](void* context, modules::SettingsStore& settings,
                    modules::ControlMutationService&, modules::GamePersistenceCoordinator& game) {
                auto& seed = *static_cast<ResumeCase*>(context);
                if (seed.unfinished) {
                    settings.Doc()["VS_CPU_LEVEL"] = 1U;
                    settings.Doc()["VS_CPU_PLAYER"] = 1U;
                    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
                    modules::GamePersistenceState saved;
                    Check(saved.fen.Assign(modules::INITIAL_FEN) &&
                              saved.undoBaseFen.Assign(modules::INITIAL_FEN),
                          "opening CPU fixture retains its bounded FEN");
                    saved.cpuMoveValid = true;
                    saved.cpuMoveFrom = 52U;
                    saved.cpuMoveTo = 36U;
                    Check(modules::WriteGamePersistenceState(settings.Doc(), saved),
                          "seed a durable opening CPU prompt before any accepted move");
                }
                Check(game.LoadOrInitialize(), "restore the saved game document through the production restore owner");
                Check(game.TryGetBootResumeOccupancy(seed.expected),
                      "restored game supplies authoritative progress and physical projection");
            }, &resume);
            if (unfinished) {
                session->ObserveBootHallMask(resume.expected);
                Check(session->ObserveHallMask(resume.expected & ~(UINT64_C(1) << 52U)) == VIRTUAL_LITCH_OK,
                      "raw Hall lift starts the restored CPU physical move");
                session->RunControlIngress([](void* context, modules::SettingsStore&,
                        modules::ControlMutationService&, modules::GamePersistenceCoordinator& game) {
                    Check(game.PrepareSettingsSnapshotForLifecycle(),
                          "lifecycle preparation retains the real unfinished physical plan");
                    Check(game.LoadOrInitialize(), "restore that unfinished plan before its boot scan");
                    Check(game.TryGetBootResumeOccupancy(static_cast<ResumeCase*>(context)->expected),
                          "unfinished opening remains a game in progress");
                }, &resume);
                Check((resume.expected & (UINT64_C(1) << 52U)) == 0ULL,
                      "resume projection retains the lifted pawn rather than raw FEN occupancy");
            }
        }
        const uint64_t observed = moved ? resume.expected ^ (UINT64_C(1) << 28U) : resume.expected;
        board.BeginBootDisplay();
        board.ObserveBootHallMask(observed);
        normal.ObserveBootHallMask(observed);
        Check(board.Advance(350U) == VIRTUAL_LITCH_OK && normal.Advance(350U) == VIRTUAL_LITCH_OK,
              "fresh boot observations reach the selected presentation through ordinary rendering");
        Check((board.PhysicalFrame().frame_hash == normal.PhysicalFrame().frame_hash) == !moved,
              "only an exactly matching ongoing game skips the fox and immediately retains live output");
        std::printf("[BootFoxZeroAndResumeV33] restored=ongoing unfinished=%u Hall=%016llx moved=%u "
                    "startup=%s next=%s bypass=physical-reset/flash-persistence\n",
                    unfinished, static_cast<unsigned long long>(observed), moved, moved ? "fox" : "skipped",
                    moved ? "correct-pieces-after-display" : "resume-game");
    }
}

void BootFoxFadeKeepsRampUnderPowerLimitV33() {
    for (const uint8_t strong : {uint8_t(0U), uint8_t(1U)}) {
        virtual_litch_config config{};
        virtual_litch_default_config(&config);
        config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
        config.strong_power_source = strong;
        config.recovery_power_limit = strong ? 0U : 1U;
        config.brightness_scale_256 = 128U;
        test_support::BoundedHintSession fixture(config);
        auto& board = fixture.session;
        board.SettingsForPersistenceTest().Doc()["MINIMAL_ANIMATIONS"] = true;
        board.BeginBootDisplay();
        Check(board.ObserveHallMask(0ULL) == VIRTUAL_LITCH_OK,
              "empty Hall admits the power-limited Fox journey");
        constexpr uint32_t deadlines[] = {0U, 50U, 100U, 125U, 150U, 200U, 225U, 275U, 350U};
        std::array<uint32_t, std::size(deadlines)> energy{};
        uint32_t previous = 0U;
        for (size_t sample = 0U; sample < energy.size(); ++sample) {
            const uint32_t elapsed = deadlines[sample];
            Check(board.Advance(elapsed - previous) == VIRTUAL_LITCH_OK,
                  "normal rendering reaches each fade-in deadline");
            const auto& output = board.PhysicalFrame();
            Check(output.applied_led_current_ma <= (strong ? 1000U : 216U),
                  "every accepted fade step retains its total LED current cap");
            if (!strong && elapsed == 350U)
                Check(output.power_limited != 0U &&
                          output.applied_led_current_ma < output.estimated_led_current_ma,
                      "full-demand Fox peak exercises actual power limiting");
            for (uint16_t index = 0U; index < 256U; ++index) {
                const auto& pixel = output.pixels[index];
                energy[sample] += pixel.r + pixel.g + pixel.b;
                Check(pixel.w == 0U, "Fox fade retains actual V3.3 RGB output");
            }
            std::printf("[BootFoxFadeKeepsRampUnderPowerLimitV33] strong=%u t=%lu "
                        "board_energy=%lu eye_r=%u limited=%u applied_ma=%u next=hold-Fox\n",
                strong, static_cast<unsigned long>(elapsed),
                static_cast<unsigned long>(energy[sample]), output.pixels[102U].r,
                output.power_limited, output.applied_led_current_ma);
            previous = elapsed;
        }
        Check(energy[0U] == 0U && energy[1U] == 0U && energy[2U] == 0U && energy.back() != 0U,
              "accepted Fox output fades from actual black to visible peak");
        for (size_t sample = 3U; sample < energy.size(); ++sample)
            Check(energy[sample] > energy[sample - 1U],
                  "produced Fox brightness keeps rising throughout all 250ms under either power envelope");
        Check(energy[6U] * 3U > energy.back() && energy[6U] * 3U < energy.back() * 2U,
              "125ms into the fade, produced-board output is near half of its own power-limited peak");
    }
}

void BootFoxCancellationV33(const uint8_t power, const uint8_t brightness) {
    virtual_litch_config config{};virtual_litch_default_config(&config);
    config.hardware=VIRTUAL_LITCH_HARDWARE_V3_3;config.strong_power_source=power;
    config.recovery_power_limit=power==0U ? 1U : 0U; // Current conservative-envelope selector.
    config.brightness_scale_256=brightness;
    for(const uint8_t owner:{uint8_t{VIRTUAL_LITCH_OWNER_P1},uint8_t{VIRTUAL_LITCH_OWNER_P2}}) {
        test_support::BoundedHintSession fixture(config), baseline(config);
        auto& b=fixture.session;auto& n=baseline.session;
        b.BeginBootDisplay();Check(b.ObserveHallMask(0ULL)==VIRTUAL_LITCH_OK,"empty Hall admits cancellable fox");
        Check(n.ObserveHallMask(0ULL)==VIRTUAL_LITCH_OK,"baseline sees same empty Hall");
        Check(b.Advance(150U)==VIRTUAL_LITCH_OK && n.Advance(150U)==VIRTUAL_LITCH_OK,"mapped logo enters normal power pipeline");
        auto logo=b.PhysicalFrame();
        Check(logo.applied_led_current_ma<=logo.estimated_led_current_ma,"current-limited logo never requests uncapped delivery");
        if(power==0U && brightness==128U) Check(logo.power_limited!=0U,"cancellation during fade retains the already current-limited baseline");
        modules::ControlMutationOutcome outcome{};
        Check(!b.TryGetLastBoardControlOutcome(outcome),"fresh boot has no previous control acknowledgement");
        Check(b.SetButtonDown(owner,runtime::VirtualLitchPhysicalButton::X,true)==VIRTUAL_LITCH_OK && b.Advance(30U)==VIRTUAL_LITCH_OK,"raw X passes production debounce");
        Check(b.SetButtonDown(owner,runtime::VirtualLitchPhysicalButton::X,false)==VIRTUAL_LITCH_OK && b.Advance(30U)==VIRTUAL_LITCH_OK,"raw cancel release passes debounce");
        Check(!b.TryGetLastBoardControlOutcome(outcome),"cancel X is consumed without dispatching Undo/reset/edit");
        Check(n.Advance(60U)==VIRTUAL_LITCH_OK && b.PhysicalFrame().frame_hash==n.PhysicalFrame().frame_hash,"X reveals current baseline without stale fox crossfade");
        std::printf("[EmptyBootFoxToLiveBoardV33] cancel=X owner=%u scale=%u power=%u limited=%u current_ma=%u ack=consumed next=restored-mode\n",owner,brightness,power,logo.power_limited,logo.applied_led_current_ma);
    }
    test_support::BoundedHintSession fixture(config), baseline(config);
    auto& b=fixture.session;auto& n=baseline.session;
    b.BeginBootDisplay();b.ObserveHallMask(0ULL);n.ObserveHallMask(0ULL);
    b.Advance(150U);n.Advance(150U);
    Check(b.ObserveHallMask(1ULL)==VIRTUAL_LITCH_OK && n.ObserveHallMask(1ULL)==VIRTUAL_LITCH_OK,"piece placement remains authoritative during fade");
    Check(b.PhysicalFrame().frame_hash==n.PhysicalFrame().frame_hash,"piece placement immediately reveals required correction output");
    b.ObserveHallMask(0ULL);n.ObserveHallMask(0ULL);b.Advance(100U);n.Advance(100U);
    Check(b.PhysicalFrame().frame_hash==n.PhysicalFrame().frame_hash,"removal after cancellation never rearms");
    b.BeginBootDisplay();b.ObserveHallMask(0ULL);n.ObserveHallMask(0ULL);b.Advance(100U);
    virtual_litch_runtime_health_fact fact{};fact.struct_size=sizeof(fact);fact.abi_version=VIRTUAL_LITCH_RUNTIME_HEALTH_ABI_VERSION;
    fact.kind=VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BOARD_SCAN;fact.board_scan=VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN;
    Check(b.DispatchRuntimeHealthFact(fact)==VIRTUAL_LITCH_OK,"production health uncertainty preempts boot art");
    Check(n.Advance(100U)==VIRTUAL_LITCH_OK && n.DispatchRuntimeHealthFact(fact)==VIRTUAL_LITCH_OK,
          "baseline receives same authoritative health fact");
    Check(b.BoardHealthConnectionState().runtime[3U]==VIRTUAL_LITCH_RUNTIME_HEALTH_HEALTHY,
          "first uncertain scan uses the existing 100ms grace period");
    Check(b.Advance(runtime::RUNTIME_HEALTH_BOARD_UNCERTAINTY_ENTER_MS)==VIRTUAL_LITCH_OK &&
          n.Advance(runtime::RUNTIME_HEALTH_BOARD_UNCERTAINTY_ENTER_MS)==VIRTUAL_LITCH_OK,
          "ordinary loop advances the actual health-entry interval");
    Check(b.DispatchRuntimeHealthFact(fact)==VIRTUAL_LITCH_OK && n.DispatchRuntimeHealthFact(fact)==VIRTUAL_LITCH_OK,
          "confirmed uncertainty reaches the same production health reducer");
    Check(b.BoardHealthConnectionState().runtime[3U]==VIRTUAL_LITCH_RUNTIME_HEALTH_INPUT_PAUSED,
          "canonical paused health owns the output now");
    Check(b.PhysicalFrame().frame_hash==n.PhysicalFrame().frame_hash,
          "health uncertainty immediately reveals existing recovery output");
    std::printf("[EmptyBootFoxToLiveBoardV33] cancel=health state=%u confirmed_after=100ms mapped_output=baseline next=recovery\n",b.BoardHealthConnectionState().runtime[3U]);
}

void BootFoxTypedControlV33() {
    virtual_litch_config config{};virtual_litch_default_config(&config);
    config.hardware=VIRTUAL_LITCH_HARDWARE_V3_3;config.strong_power_source=1U;
    test_support::BoundedHintSession fixture(config);
    auto& board=fixture.session;
    board.BeginBootDisplay();board.ObserveHallMask(0ULL);board.Advance(150U);
    modules::ControlMutationOutcome outcome{};
    board.RunControlIngress([](void* context,modules::SettingsStore&,
            modules::ControlMutationService& controls,modules::GamePersistenceCoordinator&) {
        StaticJsonDocument<32> doc;doc["value"]=true;
        *static_cast<modules::ControlMutationOutcome*>(context)=
            controls.MutateSetting("MINIMAL_ANIMATIONS",doc["value"],41U);
    },&outcome);
    Check(outcome.status==modules::ControlMutationStatus::Applied,
          "same typed setting service acknowledges a real configuration action");
    Check(board.Advance(1U)==VIRTUAL_LITCH_OK,"setting callback cancellation reaches actual render");
    // No rearm after a later empty snapshot: current mode owns output.
    const auto before=board.PhysicalFrame();board.ObserveHallMask(0ULL);
    Check(board.PhysicalFrame().frame_hash==before.frame_hash,"typed control cancellation does not restart logo");
    std::printf("[EmptyBootFoxToLiveBoardV33] control=typed-setting ack=Applied cancellation=permanent next=normal-mode\n");
}

void AccentPolicyJourney() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    modules::StartupPieceAccent accent;
    accent.Begin();
    Check(accent.Update(false, 0ULL, true, true) == 256U,
          "services alone never invent occupied pieces");
    Check(accent.Update(true, 1ULL, false, true) == 256U &&
              accent.PieceScale256(0U) == 0U,
          "ordinary pieces await the first ambient sample without dimming other layers");
    fxe::ManualMillis::SetNowMs(1000U);
    Check(accent.Update(true, 1ULL, false, true) == 256U &&
              accent.PieceScale256(0U) == 0U,
          "missing sensor starts the bounded sequential fade from zero");
    fxe::ManualMillis::SetNowMs(1400U);
    Check(accent.Update(true, 1ULL, false, true) == 256U &&
              accent.PieceScale256(0U) == 256U && !accent.IsArmed(),
          "piece reaches its normal level and the introduction ends without an over-bright pulse");
    accent.Begin();
    Check(accent.Update(true, 1ULL, true, true) == 256U &&
              accent.PieceScale256(0U) == 0U,
          "fresh usable sample admits the sequential fade without delay");
    Check(accent.Update(true, 0ULL, true, true) == 256U && !accent.IsArmed(), "piece handling cancels immediately");
    accent.Begin();
    Check(accent.Update(true, 1ULL, true, false) == 256U && !accent.IsArmed(), "required recovery cues cancel instead of dimming");
}

void OccupiedAccentJourney(const uint16_t brightnessScale, const bool cancelPickup,
                           const uint8_t hardwareProfile = VIRTUAL_LITCH_HARDWARE_S3) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardwareProfile;
    const auto revision = hardwareProfile == VIRTUAL_LITCH_HARDWARE_V3
        ? hardware::BoardRevision::V3S3 : hardware::BoardRevision::V2S3;
    config.strong_power_source = 1U;
    config.brightness_scale_256 = brightnessScale;
    test_support::BoundedHintSession fixture(config);
    test_support::BoundedHintSession baselineFixture(config);
    auto& baseline = baselineFixture.session;
    auto& board = fixture.session;
    board.SettingsForPersistenceTest().Doc()["P1_END_PIECE_WHEEL"] = 0U;
    board.SettingsForPersistenceTest().Doc()["P2_END_PIECE_WHEEL"] = 170U;
    board.SettingsForPersistenceTest().Doc()["BRIGHTNESS"] = brightnessScale == 8U ? 0U : 5U;
    board.SettingsForPersistenceTest().Doc()["BOARD_SETUP_ASSISTANCE"] = false;
    baseline.SettingsForPersistenceTest().Doc()["P1_END_PIECE_WHEEL"] = 0U;
    baseline.SettingsForPersistenceTest().Doc()["P2_END_PIECE_WHEEL"] = 170U;
    baseline.SettingsForPersistenceTest().Doc()["BRIGHTNESS"] = brightnessScale == 8U ? 0U : 5U;
    baseline.SettingsForPersistenceTest().Doc()["BOARD_SETUP_ASSISTANCE"] = false;
    // This marks the same service handoff as FirmwareSystem, before Hall input.
    board.BeginStartupPieceAccent();
    constexpr uint64_t opening = UINT64_C(0xFFFF00000000FFFF);
    Check(board.ObserveHallMask(opening) == VIRTUAL_LITCH_OK, "first real Hall frame enters startup accent owner");
    Check(baseline.ObserveHallMask(opening) == VIRTUAL_LITCH_OK, "baseline observes same real Hall frame");
    const uint16_t pawn = hardware::MapSemanticSquareLedTargets(
        revision, 52U, hardware::SquareLedSlot::Piece, false).indices[0];
    uint16_t previous = 0U;
    uint16_t firstLevel = 0U;
    uint16_t peakLevel = 0U;
    for (uint32_t step = 0U; step <= 21U; ++step) {
        if (step != 0U) {
            Check(board.Advance(50U) == VIRTUAL_LITCH_OK, "timed production render accepted");
            Check(baseline.Advance(50U) == VIRTUAL_LITCH_OK, "baseline advances identically");
        }
        if (cancelPickup && step == 2U) {
            Check(board.ObserveHallMask(opening & ~(1ULL << 52U)) == VIRTUAL_LITCH_OK,
                  "real pickup cancels ordinary startup layer");
            Check(baseline.ObserveHallMask(opening & ~(1ULL << 52U)) == VIRTUAL_LITCH_OK,
                  "baseline observes identical pickup");
        }
        const auto& physical = board.PhysicalFrame();
        if (cancelPickup && step >= 2U) {
            const auto& normal = baseline.PhysicalFrame();
            for (size_t index = 0U; index < physical.led_count; ++index) {
                const auto& a = physical.pixels[index];
                const auto& b = normal.pixels[index];
                Check(a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w,
                      "pickup restores actual RGBW output equal to no-accent production baseline");
            }
            const auto& cue = physical.pixels[hardware::MapSemanticSquareLedTargets(
                revision, 36U, hardware::SquareLedSlot::Square0, false).indices[0]];
            Check(cue.r != 0U || cue.g != 0U || cue.b != 0U || cue.w != 0U,
                  "manual-zero pickup retains actual palette-selected e4 destination light");
        }
        const auto& p = physical.pixels[pawn];
        const uint16_t level = static_cast<uint16_t>(p.r + p.g + p.b + p.w);
        if (!cancelPickup && brightnessScale != 8U && step == 2U) {
            Check(PieceLevel(physical, revision, 0U) >
                      PieceLevel(physical, revision, 15U),
                  "piece entrance visibly staggers two same-side pieces");
        }
        if (!cancelPickup && brightnessScale != 8U && step <= 13U) {
            Check(level >= previous, "occupied-piece rise is monotonic");
        }
        previous = level;
        if (step == 0U) { firstLevel = level; }
        if (step == 13U) { peakLevel = level; }
        Check(physical.applied_led_current_ma <= 1000U, "accent keeps actual emitter current inside strong-source cap");
        std::printf("[STARTUP_PIECES] t=%lu scale=%u pickup=%u pawn=%u current=%u phase=%u next=%s\n",
                    static_cast<unsigned long>(physical.now_ms), brightnessScale, cancelPickup ? 1U : 0U,
                    level, physical.applied_led_current_ma, board.State().interaction_phase,
                    cancelPickup && step >= 2U ? "place-pawn-on-e4" : "wait-or-play");
    }
    if (!cancelPickup && brightnessScale != 8U) {
        Check(firstLevel == 0U && peakLevel == previous,
              "actual saturated piece fades from zero and never exceeds its normal output");
    }
    if (cancelPickup) {
        Check(board.State().valid_destination_mask != 0ULL, "pickup retains required legal destination guidance");
    }
}

void SleepInterruptAccentJourney(const uint8_t hardwareProfile = VIRTUAL_LITCH_HARDWARE_S3) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = hardwareProfile;
    config.strong_power_source = 1U;
    config.brightness_scale_256 = 64U;
    test_support::BoundedHintEngine accentEngine;
    test_support::BoundedHintEngine normalEngine;
    using runtime::virtual_litch_internal::Session;
    using runtime::virtual_litch_internal::SessionMode;
    Session accented(config, SessionMode::EngineBackedJourney, nullptr, &accentEngine);
    Session normal(config, SessionMode::EngineBackedJourney, nullptr, &normalEngine);
    constexpr uint64_t opening = UINT64_C(0xFFFF00000000FFFF);
    for (Session* board : {&accented, &normal}) {
        board->SettingsForPersistenceTest().Doc()["BOARD_SETUP_ASSISTANCE"] = false;
    }
    accented.BeginStartupPieceAccent();
    for (Session* board : {&accented, &normal}) {
        Check(board->DispatchBoardObservation(UINT64_MAX, opening) == VIRTUAL_LITCH_OK,
              "sleep journey receives complete real Hall observation");
        Check(board->Advance(100U) == VIRTUAL_LITCH_OK, "sleep interrupts during rise");
    }
    Check(accented.PhysicalFrame().frame_hash != normal.PhysicalFrame().frame_hash,
          "sleep interrupts a visible occupied-piece accent");
    for (Session* board : {&accented, &normal}) {
        Check(board->RequestSleep(VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE) == VIRTUAL_LITCH_OK,
              "existing production sleep owner accepts countdown");
        Check(board->SleepLifecycle().state == VIRTUAL_LITCH_SLEEP_COUNTDOWN,
              "sleep cancellation comparison exercises active countdown");
    }
    for (uint32_t step = 0U; step < 4U; ++step) {
        const auto& a = accented.PhysicalFrame();
        const auto& b = normal.PhysicalFrame();
        Check(a.led_count == b.led_count, "sleep frame geometry matches baseline");
        for (uint16_t i = 0U; i < a.led_count; ++i) {
            Check(a.pixels[i].r == b.pixels[i].r && a.pixels[i].g == b.pixels[i].g &&
                      a.pixels[i].b == b.pixels[i].b && a.pixels[i].w == b.pixels[i].w,
                  "sleep cancellation restores identical actual baseline output");
        }
        Check(accented.Advance(50U) == VIRTUAL_LITCH_OK && normal.Advance(50U) == VIRTUAL_LITCH_OK,
              "sleep countdown continues with accent permanently cancelled");
    }
}

void DisabledGridJourney() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    firmware::StartupLedSequence startup;
    startup.Begin(false);
    uint32_t services = 0U;
    uint32_t attempts = 0U;
    bool outputAccepted = false;
    const auto render = [&](const uint16_t scale) {
        ++attempts;
        Check(scale == 256U, "disabled grid skips the invisible ramp");
        return outputAccepted;
    };
    const auto initialize = [&]() { ++services; };
    startup.Update(render, initialize);
    Check(services == 0U, "disabled grid still requires accepted LED output");
    fxe::ManualMillis::SetNowMs(125U);
    outputAccepted = true;
    startup.Update(render, initialize);
    Check(startup.IsReady() && services == 1U && attempts == 2U,
          "first accepted dark frame admits services without a 500ms wait");
    startup.Update(render, initialize);
    Check(services == 1U, "disabled grid admits services exactly once");
}

void V3_3StartupOutputJourney(const bool rotate) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.board_rotate = rotate ? 1U : 0U;
    config.square_rotate_for_output = rotate ? 1U : 0U;
    config.strong_power_source = 1U;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    board.SettingsForPersistenceTest().Doc()["BRIGHTNESS"] = 8U;
    board.SettingsForPersistenceTest().Doc()["AUTO_BRIGHTNESS"] = false;
    board.SettingsForPersistenceTest().Doc()["WHITE_SQUARES_ON"] = true;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    firmware::StartupLedSequence startup;
    startup.Begin(firmware::StartupLedSequence::UsesPreServiceGrid(hardware::BoardRevision::V3_3S3));
    unsigned services=0U;
    startup.Update([&](uint16_t progress) {
        board.CaptureStartupFrame(fxe::Millis(),progress);
        const auto& physical=board.PhysicalFrame();
        Check(physical.led_count==300U && physical.channels_per_led==3U,
              "V3.3 accepted black uses the real board/status RGB topology");
        Check(Sum(physical)==0U && physical.applied_led_current_ma==0U,
              "V3.3 pre-service frame is entirely black including controls");
        return true;
    },[&](){++services;});
    Check(startup.IsReady() && startup.AcceptedFrames()==1U && services==1U,
          "one accepted black transfer opens V3.3 services without 500ms pre-Hall fade");
    Check(firmware::StartupLedSequence::UsesPreServiceGrid(hardware::BoardRevision::V2S3) &&
          firmware::StartupLedSequence::UsesPreServiceGrid(hardware::BoardRevision::V3S3),
          "existing V2/V3 grid sequence is preserved");
    std::printf("[V3_3_STARTUP] rotate=%u accepted=1 RGB_outputs=300 current=0 services=ready occupancy=unknown next=first-valid-Hall\n",rotate);

}

void HealthyJourney(const bool squaresOn, const uint8_t brightness,
                    const bool minimalAnimations = false) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    // A fake strong source must not bypass the production startup cap.
    config.strong_power_source = 1U;
    // Fileless gameplay capture uses this explicit scale after startup.
    config.brightness_scale_256 = modules::brightness_scale_policy::ComputeEffectiveScale256(
        brightness, false, 0.0f, 256U);
    test_support::BoundedHintSession boardFixture(config);
    auto& board = boardFixture.session;
    board.SettingsForPersistenceTest().Doc()["WHITE_SQUARES_ON"] = squaresOn;
    board.SettingsForPersistenceTest().Doc()["BRIGHTNESS"] = brightness;
    board.SettingsForPersistenceTest().Doc()["MINIMAL_ANIMATIONS"] = minimalAnimations;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    firmware::StartupLedSequence startup;
    const bool gridEnabled = squaresOn && brightness != 0U;
    startup.Begin(gridEnabled);
    const uint32_t fadeEndMs = gridEnabled ? 600U : 100U;
    uint32_t services = 0U;
    uint32_t previousSum = 0U;
    uint32_t firstLitSum = 0U;
    uint32_t finalSum = 0U;
    uint32_t serviceStartedMs = 0U;
    uint32_t serviceCompletedMs = 0U;
    uint64_t plateauHash = 0U;
    const bool oldOrderFault = std::getenv("LITCH_STARTUP_REPRO_OLD") != nullptr;
    const auto render = [&](uint16_t scale) {
        const uint32_t now = fxe::Millis();
        board.CaptureStartupFrame(now, scale);
        const auto& semantic = board.SemanticFrame();
        const auto& physical = board.PhysicalFrame();
        Check(physical.applied_led_current_ma <= 216U, "startup keeps conservative LED headroom");
        for (const auto& pixel : semantic.status) {
            Check(IsBlack(pixel), "startup cannot claim turn, ready, recovery or BLE");
        }
        for (uint8_t square = 0U; square < 64U; ++square) {
            Check(IsBlack(semantic.board[square][1]), "unscanned pieces have no invented color");
        }
        if (gridEnabled && brightness != 0U && scale == 128U) {
            uint8_t visibleLightSquares = 0U;
            uint16_t minimumVisibleLevel = UINT16_MAX;
            uint16_t maximumVisibleLevel = 0U;
            for (uint8_t square = 0U; square < 64U; ++square) {
                const uint8_t rank = static_cast<uint8_t>(square / 8U);
                const uint8_t file = static_cast<uint8_t>(square % 8U);
                if (((rank + file) & 1U) != 0U) { continue; }
                const auto& left = semantic.board[square][0];
                const auto& right = semantic.board[square][2];
                const uint16_t level = static_cast<uint16_t>(
                    left.r + left.g + left.b + left.w +
                    right.r + right.g + right.b + right.w);
                if (level == 0U) { continue; }
                ++visibleLightSquares;
                if (level < minimumVisibleLevel) { minimumVisibleLevel = level; }
                if (level > maximumVisibleLevel) { maximumVisibleLevel = level; }
            }
            Check(visibleLightSquares > 0U && visibleLightSquares < 32U,
                  "mid-ramp startup reveals only part of the light-square sequence");
            Check(minimumVisibleLevel < maximumVisibleLevel,
                  "mid-ramp startup contains simultaneous fade levels");
        }
        const uint32_t sum = Sum(physical);
        if (brightness == 0U) {
            Check(sum >= previousSum, "near-dark fade remains monotonic after quantization");
        }
        previousSum = sum;
        if (firstLitSum == 0U && sum > 0U) { firstLitSum = sum; }
        if (scale == 256U) { finalSum = sum; }
        if (!gridEnabled) { Check(sum == 0U, "disabled grid remains intentionally dark"); }
        if (scale == 0U || scale == 128U || scale == 256U) {
            std::printf("[STARTUP] minimal=%u t=%lu raw_fade=%u light_sum=%lu current_ma=%u input=not_admitted text=none next=wait\n",
                minimalAnimations ? 1U : 0U, static_cast<unsigned long>(now), scale, static_cast<unsigned long>(sum), physical.applied_led_current_ma);
        }
        // Controlled production-equivalent transfer duration, NOT optical proof.
        fxe::ManualMillis::SetNowMs(now + 11U);
        if (oldOrderFault && services == 0U) {
            ++services; // Old boot began services after its first returned frame.
        }
        return true;
    };
    const auto initialize = [&]() {
        ++services;
        serviceStartedMs = fxe::Millis();
        Check(serviceStartedMs == fadeEndMs + 11U, "services start only after final enabled-grid or dark transfer settles");
        plateauHash = board.PhysicalFrame().frame_hash;
        // Model blocking service cost while the completed static grid is latched.
        fxe::ManualMillis::SetNowMs(serviceStartedMs + 250U);
        serviceCompletedMs = fxe::Millis();
        // Recapture the held output at the handoff time, without advancing any
        // gameplay owner during services or inventing another accepted transfer.
        board.CaptureStartupFrame(serviceCompletedMs, 256U);
        Check(board.PhysicalFrame().frame_hash == plateauHash, "services preserve static startup surface");
        Check(!startup.IsReady(), "readiness waits for service callback completion");
    };
    for (uint32_t i = 0U; i < 1000U && !startup.IsReady(); ++i) {
        startup.Update(render, initialize);
        if (fxe::Millis() < fadeEndMs) { Check(services == 0U, "no service before completed fade"); }
        if (!startup.IsReady()) {
            fxe::ManualMillis::SetNowMs(fxe::Millis() + 1U);
        }
    }
    Check(startup.IsReady(), "healthy startup reaches service-ready boundary");
    Check(services == 1U, "services initialize exactly once");
    Check(startup.AcceptedFrames() == (gridEnabled ? 21U : 1U), "enabled grid presents21 samples; disabled grid presents one dark sample");
    if (squaresOn && brightness == 8U) {
        Check(firstLitSum > 0U && firstLitSum < finalSum,
              "normal and Minimal gameplay settings retain an actual intermediate startup fade");
    }
    Check(startup.MaxFrameGapMs() == (gridEnabled ? 25U : 0U), "fade frame start cadence remains 25ms");
    Check(startup.RejectedFrames() == 0U, "healthy output has no rejected transfers");
    startup.Update(render, initialize);
    Check(services == 1U, "later loop cannot restart initialization");
    Check(serviceCompletedMs == fadeEndMs + 261U && fxe::Millis() == serviceCompletedMs &&
              board.PhysicalFrame().now_ms == serviceCompletedMs,
          "startup output and playback clock reach the actual service completion together");
    // Existing production-equivalent Hall ingress establishes the actual board
    // after services: no direct FEN/reducer injection supplies the ready state.
    constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
    Check(board.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK,
          "authoritative opening scan accepted at handoff");
    Check(board.State().now_ms == serviceCompletedMs &&
              fxe::Millis() == serviceCompletedMs,
          "first authoritative Hall input cannot run at the pre-service fade timestamp");
    Check(board.Advance(200U) == VIRTUAL_LITCH_OK,
          "opening reaches the production setup stability boundary");
    const auto& readyState = board.State();
    const auto readyHome = board.HomeStateLine();
    const bool playReady =
        readyState.interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
        readyState.required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::None) &&
        readyState.physical_occupied_mask == OPENING &&
        readyState.expected_occupied_mask == OPENING &&
        readyState.correction_mask == 0ULL && readyState.human_illegal_mask == 0ULL &&
        readyState.cpu_illegal_mask == 0ULL && readyState.pending_action_token == 0U &&
        readyState.side_to_move_black == 0U &&
        std::strstr(readyHome.CStr(), "\"a\":\"idle\"") != nullptr &&
        std::strstr(readyHome.CStr(), "\"pm\":0") != nullptr &&
        std::strstr(readyHome.CStr(), "\"out\":\"n\"") != nullptr;
    Check(playReady, "authoritative Home and board action agree that White can play");
    Check(readyState.now_ms == serviceCompletedMs + 200U &&
              board.PhysicalFrame().now_ms == readyState.now_ms &&
              fxe::Millis() == readyState.now_ms,
          "post-service state, output and production timers share the same time");
    std::printf("[STARTUP] services_at=%lu completed_at=%lu ready_at=%lu ready=%u occupancy=%016llx text=%.*s next=%s home=%s\n",
        static_cast<unsigned long>(serviceStartedMs),
        static_cast<unsigned long>(serviceCompletedMs),
        static_cast<unsigned long>(readyState.now_ms), playReady ? 1U : 0U,
        static_cast<unsigned long long>(readyState.physical_occupied_mask),
        static_cast<int>(board.DisplayText().size()), board.DisplayText().data(),
        playReady ? "lift-White-piece" : "inspect-handoff", readyHome.CStr());

    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;
    const uint64_t lifted = OPENING & ~(UINT64_C(1) << E2);
    const bool liftAccepted = board.ObserveHallMask(lifted) == VIRTUAL_LITCH_OK;
    const auto pickup = board.State();
    const auto pickupHome = board.HomeStateLine();
    Check(pickup.now_ms == serviceCompletedMs + 200U &&
              board.PhysicalFrame().now_ms == pickup.now_ms && fxe::Millis() == pickup.now_ms,
          "legal pickup is published immediately on the post-service clock");
    const auto isBlue = [](const auto& pixel) {
        return pixel.b > pixel.r && pixel.b > pixel.g && pixel.w == 0U;
    };
    const auto leftIndex = hardware::LedIndexForSquare(E4, hardware::SquareLedSlot::Square0);
    const auto rightIndex = hardware::LedIndexForSquare(E4, hardware::SquareLedSlot::Square2);
    const auto& liftFrame = board.PhysicalFrame();
    const bool immediateDestination = isBlue(liftFrame.pixels[leftIndex]) && isBlue(liftFrame.pixels[rightIndex]);
    Check(immediateDestination == (minimalAnimations || brightness == 0U),
          "Minimal and minimum brightness show e4 immediately while the ordinary reveal travels from e2");
    constexpr uint32_t e4RevealMs = 2U * runtime::LEGAL_DESTINATION_STEP_MS + runtime::LEGAL_DESTINATION_FADE_MS;
    Check(board.Advance(e4RevealMs) == VIRTUAL_LITCH_OK &&
              board.PhysicalFrame().now_ms == pickup.now_ms + e4RevealMs,
          "the e4 cue arrives after its two-square reveal and fade on the post-service clock");
    const auto& e4Left = board.PhysicalFrame().pixels[leftIndex];
    const auto& e4Right = board.PhysicalFrame().pixels[rightIndex];
    const bool destinationBlue = isBlue(e4Left) && isBlue(e4Right);
    const bool pickupActionable = liftAccepted &&
        pickup.interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
        // A lifted selection has legal destinations but no committed relocation plan.
        pickup.required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::None) &&
        pickup.selected_square == E2 && pickup.physical_occupied_mask == lifted &&
        pickup.expected_occupied_mask == OPENING &&
        (pickup.valid_destination_mask & (UINT64_C(1) << E4)) != 0ULL &&
        pickup.correction_mask == 0ULL && pickup.human_illegal_mask == 0ULL &&
        pickup.cpu_illegal_mask == 0ULL && destinationBlue &&
        std::strstr(pickupHome.CStr(), "\"a\":\"lift\"") != nullptr &&
        std::strstr(pickupHome.CStr(), "\"pm\":0") != nullptr;
    Check(pickupActionable,
          "physical e2 selection publishes Home lift with blue legal e4 placement guidance");
    std::printf("[STARTUP_PICKUP] t=%lu cue_at=%lu accepted=%u phase=%s action=%s legal=%016llx e4=%u/%u/%u/%u|%u/%u/%u/%u next=%s home=%s\n",
        static_cast<unsigned long>(pickup.now_ms), static_cast<unsigned long>(board.PhysicalFrame().now_ms), liftAccepted ? 1U : 0U,
        runtime::ChessInteractionPhaseToString(static_cast<runtime::ChessInteractionPhase>(pickup.interaction_phase)),
        runtime::RequiredPhysicalActionToString(static_cast<runtime::RequiredPhysicalAction>(pickup.required_action)),
        static_cast<unsigned long long>(pickup.valid_destination_mask),
        e4Left.r, e4Left.g, e4Left.b, e4Left.w,
        e4Right.r, e4Right.g, e4Right.b, e4Right.w,
        pickupActionable ? "place-pawn-on-e4" : "inspect-pickup", pickupHome.CStr());
}
void RejectedOutputJourney() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 249U);
    firmware::StartupLedSequence startup;
    startup.Begin();
    uint32_t services = 0U;
    uint32_t attempts = 0U;
    bool allow = false;
    auto render = [&](uint16_t) { ++attempts; return allow; };
    auto initialize = [&]() { ++services; };
    for (uint32_t ms = 0U; ms <= 1000U; ++ms) {
        startup.Update(render, initialize);
        fxe::ManualMillis::SetNowMs(fxe::Millis() + 1U);
    }
    Check(services == 0U && !startup.IsReady(), "timer wrap and permanent denied output cannot admit services");
    Check(attempts == 41U, "failed outputs retry at bounded cadence");
    Check(startup.NeedsOutputRecovery(), "initial failure cannot postpone USB diagnostic deadline");
    Check(firmware::IsStartupConsoleLineAllowed("support"), "blocked output keeps support diagnostics");
    Check(firmware::IsStartupConsoleLineAllowed(" reboot nosave \r"), "explicit safe reboot remains available");
    for (const char* command : {"reboot", "settings erase", "5ven on", "ledpower clear", "engine reset", "reboot nosave;settings erase"}) {
        Check(!firmware::IsStartupConsoleLineAllowed(command), "partial startup rejects mutations and unsafe command composition");
    }
    allow = true;
    fxe::ManualMillis::SetNowMs(fxe::Millis() + 25U);
    startup.Update(render, initialize);
    Check(services == 0U && !startup.IsReady(), "first recovered output still must complete its fade");
    Check(!startup.NeedsOutputRecovery(), "recovered fade defers diagnostic work again");
    for (uint32_t ms = 0U; ms < 500U; ++ms) {
        fxe::ManualMillis::SetNowMs(fxe::Millis() + 1U);
        startup.Update(render, initialize);
    }
    Check(services == 1U && startup.IsReady(), "fresh accepted fade recovers without reboot");
    Check(!startup.NeedsOutputRecovery(), "recovery restriction ends only after initialization");
    std::puts("[STARTUP] rejected=41 services=0 diagnostic=support/reboot-nosave recovery=fresh-fade ready=1");
}

enum class StartupLatchState { Fresh, Persisted, UnrelatedPending };

void BrownoutConsoleJourney(const StartupLatchState latchState) {
    // Only the platform reset-cause observation and irreversible reboot effect
    // are supplied here. SettingsStore, input parsing, dispatch interception and
    // the output-recovery sequence are production owners.
    constexpr const char* LATCH_KEY = "LED_BROWNOUT_LIMIT_LATCH";
    char directory[] = "/tmp/litch-startup-console-XXXXXX";
    if (mkdtemp(directory) == nullptr) {
        Check(false, "create isolated startup settings fixture");
        return;
    }
    const std::string path = std::string(directory) + "/settings.json";
    fxe::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path.c_str();
    settingsConfig.allowSynchronousDeferredSaves = false;
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    if (latchState == StartupLatchState::Persisted) {
        fxe::SettingsStore fixture(settingsConfig);
        Check(fixture.Load(), "load persisted-protection fixture");
        fixture.Doc()[LATCH_KEY] = true;
        Check(fixture.SaveNowBlocking(), "persist protection before the tested boot");
    }
    fxe::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "boot loads actual settings file");
    if (latchState != StartupLatchState::Persisted) {
        if (latchState == StartupLatchState::Fresh) {
            // Same mutation made by SetLedBrownoutLimitLatched after observing
            // ESP_RST_BROWNOUT; Arduino wiring is covered by the source guard.
            settings.Doc()[LATCH_KEY] = true;
        } else {
            settings.Doc()["BRIGHTNESS"] = 3U;
        }
        settings.ArmPersistenceGuard();
        settings.MarkMustSaveDirty();
    }
    const uint32_t savesBefore = settings.GetSaveCount();
    firmware::StartupLedSequence startup;
    startup.Begin();
    uint32_t services = 0U;
    uint32_t restartDispatches = 0U;
    uint32_t supportDispatches = 0U;
    bool outputAccepted = false;
    auto render = [&](uint16_t) { return outputAccepted; };
    auto initialize = [&]() { ++services; };
    fxe::MemoryConsoleTransport transport;
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);
    fxe::Console::Config consoleConfig;
    consoleConfig.installBuiltins = false;
    fxe::Console console(transport, consoleConfig);
    console.SetSettingsStore(&settings);
    console.SetEnteredLineCallback([&](const char* line) {
        return !startup.IsReady() && firmware::HandleStartupConsoleLine(
            console, line,
            settings.ReadValue(LATCH_KEY, false) &&
                settings.IsPersistenceGuardArmed());
    });
    Check(console.RegisterCommand("reboot", "capture restart dispatch",
        [&](const fxe::Console::Args& args) {
            Check(args.argc == 2U && args.Equals(1U, "nosave"),
                  "only the exact diagnostic restart reaches its handler");
            ++restartDispatches;
            return true;
        }), "register observable reboot effect");
    Check(console.RegisterCommand("support", "capture diagnostics",
        [&](const fxe::Console::Args&) {
            ++supportDispatches;
            return true;
        }), "register support diagnostics");
    for (const uint32_t nowMs : {1500U, 61000U}) {
        fxe::ManualMillis::SetNowMs(nowMs);
        startup.Update(render, initialize);
        Check(startup.NeedsOutputRecovery(), "blocked output admits diagnostic input");
        output.Clear();
        fxe::test_support::RunCommand(console, transport, " reboot nosave ");
        if (latchState == StartupLatchState::Fresh) {
            Check(restartDispatches == 0U,
                  "fresh unsaved brownout rejects actual restart dispatch even after time");
            Check(output.Str().find("restart blocked to preserve unsaved power protection") != std::string::npos,
                  "rejection explains the protection being preserved");
            Check(settings.IsPersistenceGuardArmed(), "rejection preserves the pending guard");
        } else {
            Check(restartDispatches == (nowMs == 1500U ? 1U : 2U),
                  "persisted protection or unrelated pending work permits restart");
        }
        fxe::test_support::RunCommand(console, transport, "support");
    }
    Check(supportDispatches == 2U, "unsaved-protection guard preserves diagnostic dispatch");
    Check(services == 0U, "console input never admits startup services on failed output");
    Check(settings.GetSaveCount() == savesBefore, "console updates never persist the fresh latch");
    fxe::SettingsStore durable(settingsConfig);
    Check(durable.Load(), "independently read the durable fixture");
    Check(durable.ReadValue(LATCH_KEY, false) == (latchState == StartupLatchState::Persisted),
          "fresh protection is demonstrably absent from storage while persisted protection survives");

    outputAccepted = true;
    fxe::ManualMillis::SetNowMs(61025U);
    startup.Update(render, initialize);
    Check(services == 0U, "diagnostic rejection preserves the recovered fade gate");
    for (uint32_t elapsedMs = 25U; elapsedMs <= 500U; elapsedMs += 25U) {
        fxe::ManualMillis::SetNowMs(61025U + elapsedMs);
        startup.Update(render, initialize);
    }
    Check(startup.IsReady() && services == 1U,
          "live output recovery completes startup without clearing protection or rebooting");
    const uint32_t restartsBeforeReadyCommand = restartDispatches;
    fxe::test_support::RunCommand(console, transport, "reboot nosave");
    Check(restartDispatches == restartsBeforeReadyCommand + 1U,
          "this pre-worker restriction does not become a general durability policy");
    std::printf("[STARTUP_CONSOLE] latch=%s before_ready_restarts=%lu diagnostics=%lu services=1 recovery=fresh-fade\n",
        latchState == StartupLatchState::Fresh ? "fresh" :
            latchState == StartupLatchState::Persisted ? "persisted" : "unrelated-pending",
        static_cast<unsigned long>(restartsBeforeReadyCommand),
        static_cast<unsigned long>(supportDispatches));
    std::filesystem::remove_all(directory);
}
} // namespace

int main() {
    for (const bool duck : {false, true}) EmptyBootFoxToLiveBoardV33(duck);
    BootFoxZeroAndResumeV33();
    BootFoxFadeKeepsRampUnderPowerLimitV33();
    BootFoxTypedControlV33();
    BootFoxCancellationV33(1U,128U);
    BootFoxCancellationV33(0U,8U);
    BootFoxCancellationV33(0U,128U);
    AccentPolicyJourney();
    OccupiedAccentJourney(64U, false);
    OccupiedAccentJourney(8U, true);
    SleepInterruptAccentJourney();
    OccupiedAccentJourney(64U, false, VIRTUAL_LITCH_HARDWARE_V3);
    OccupiedAccentJourney(8U, true, VIRTUAL_LITCH_HARDWARE_V3);
    SleepInterruptAccentJourney(VIRTUAL_LITCH_HARDWARE_V3);
    DisabledGridJourney();
    V3_3StartupOutputJourney(false);
    V3_3StartupOutputJourney(true);
    HealthyJourney(true, 0U);
    HealthyJourney(true, 8U);
    HealthyJourney(true, 8U, true);
    HealthyJourney(false, 0U);
    HealthyJourney(false, 8U);
    RejectedOutputJourney();
    BrownoutConsoleJourney(StartupLatchState::Fresh);
    BrownoutConsoleJourney(StartupLatchState::Persisted);
    BrownoutConsoleJourney(StartupLatchState::UnrelatedPending);
    std::printf("[STARTUP_LED_JOURNEY] %s\n", failures == 0 ? "pass" : "FAIL");
    return failures == 0 ? 0 : 1;
}
