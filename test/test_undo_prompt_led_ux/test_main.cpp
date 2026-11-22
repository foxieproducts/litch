#include <array>
#include <cstdint>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/led_mapping.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/persisted_side.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "modules/turn_anim_style_setting.hpp"
#include "runtime/app_event.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/square_mapping.hpp"
#include "../support/led_frame_composer_snapshot_adapter.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void AdvanceAndTick(runtime::ManualClock& clock, runtime::GameStateMachine& sm, const uint32_t deltaMs) {
    clock.AdvanceMs(deltaMs);
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeTick(clock.NowMs()));
}

void Sensor(runtime::ManualClock& clock, runtime::GameStateMachine& sm, const uint8_t square, const bool occ) {
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeSensorDelta(clock.NowMs(), square, occ));
}

void PressP1(runtime::ManualClock& clock, runtime::GameStateMachine& sm) {
    (void)clock;
    if (sm.IsUndoPromptActive()) {
        sm.CancelUndoPrompt(true);
        return;
    }
    Check(sm.BeginUndoPrompt(12U, 28U,
                             modules::PersistedSide::Black,
                             'p', '\0', 0U),
          "explicit undo prompt accepted");
}

std::array<bool, 64> Occ64ToOccupiedPhysical(const uint64_t occ64) {
    std::array<bool, 64> out{};
    for (uint8_t sq = 0; sq < 64; ++sq) {
        out[sq] = ((occ64 >> sq) & 1ULL) != 0ULL;
    }
    return out;
}

modules::Rgb PieceLedAtSquare(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
                              const uint8_t sq) {
    return ledFrame[hardware::LedIndexForSquare(sq, hardware::SquareLedSlot::Piece)];
}

modules::Rgb SquareLedAtSquare(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
                               const uint8_t sq,
                               const hardware::SquareLedSlot slot) {
    return ledFrame[hardware::LedIndexForSquare(sq, slot)];
}

uint16_t Intensity(const modules::Rgb c) {
    return static_cast<uint16_t>(c.r) + static_cast<uint16_t>(c.g) + static_cast<uint16_t>(c.b);
}

uint8_t MaxChannel(const modules::Rgb c) {
    return (c.r > c.g) ? ((c.r > c.b) ? c.r : c.b) : ((c.g > c.b) ? c.g : c.b);
}

uint32_t ColorDistanceSq(const modules::Rgb a, const modules::Rgb b) {
    const int32_t dr = static_cast<int32_t>(a.r) - static_cast<int32_t>(b.r);
    const int32_t dg = static_cast<int32_t>(a.g) - static_cast<int32_t>(b.g);
    const int32_t db = static_cast<int32_t>(a.b) - static_cast<int32_t>(b.b);
    return static_cast<uint32_t>(dr * dr + dg * dg + db * db);
}

uint32_t NormalizedColorDistanceSq(const modules::Rgb a,
                                   const modules::Rgb b) {
    const uint8_t aMax = MaxChannel(a);
    const uint8_t bMax = MaxChannel(b);
    if (aMax == 0U || bMax == 0U) return UINT32_MAX;
    const auto normalize = [](const uint8_t channel, const uint8_t maximum) {
        return static_cast<int32_t>(
            (static_cast<uint32_t>(channel) * 255U + maximum / 2U) /
            maximum);
    };
    const int32_t dr = normalize(a.r, aMax) - normalize(b.r, bMax);
    const int32_t dg = normalize(a.g, aMax) - normalize(b.g, bMax);
    const int32_t db = normalize(a.b, aMax) - normalize(b.b, bMax);
    return static_cast<uint32_t>(dr * dr + dg * dg + db * db);
}

uint32_t EdgeRangeIntensity(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edgeFrame,
                            const uint8_t start,
                            const uint8_t count) {
    uint32_t total = 0;
    const uint8_t limit = static_cast<uint8_t>(start + count);
    for (uint8_t index = start; index < limit && index < edgeFrame.size(); ++index) {
        total += Intensity(edgeFrame[index]);
    }
    return total;
}

modules::Rgb BrightestEdgeLed(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edgeFrame,
                              const uint8_t start,
                              const uint8_t count) {
    modules::Rgb best = modules::RGB_BLACK;
    uint16_t bestIntensity = 0;
    const uint8_t limit = static_cast<uint8_t>(start + count);
    for (uint8_t index = start; index < limit && index < edgeFrame.size(); ++index) {
        const uint16_t currentIntensity = Intensity(edgeFrame[index]);
        if (currentIntensity > bestIntensity) {
            bestIntensity = currentIntensity;
            best = edgeFrame[index];
        }
    }
    return best;
}

bool AnyEdgeLit(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edgeFrame) {
    for (const modules::Rgb rgb : edgeFrame) {
        if (rgb.r != 0U || rgb.g != 0U || rgb.b != 0U) {
            return true;
        }
    }

    return false;
}

bool AllEdgesShowRedFault(const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edgeFrame) {
    if (edgeFrame.empty()) {
        return false;
    }

    for (const modules::Rgb rgb : edgeFrame) {
        if (rgb.r == 0U || rgb.r <= rgb.g || rgb.r <= rgb.b) {
            return false;
        }
    }

    return true;
}

} // namespace

int main() {
    std::puts("[UNDO_PROMPT_LED_UX] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_undo_prompt_led_ux.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    settings.Doc()["P1_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_PIECE_WHEEL"] = 128;
    settings.Doc()["P1_END_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_END_PIECE_WHEEL"] = 128;
    settings.Doc()[modules::turn_anim_style_setting::KEY_TURN_ANIM_STYLE] =
        modules::turn_anim_style_setting::VALUE_ACTIVE_END_PINGPONG_SIDES;

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 20;
    smCfg.settleFlashMs = 10;
    smCfg.illegalFlashMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    runtime::ManualClock clock;

    // Seed a simple expected/physical occupancy with a black pawn-like move path
    // and one unrelated occupied square used to validate local undo correction cues.
    sm.SetExpectedOccupancyMask((1ULL << 7) | (1ULL << 12));
    sm.SetExpectedSideToMove(modules::PersistedSide::White);
    sm.DebugSetPhysicalOccupancyMask((1ULL << 7) | (1ULL << 12));

    // Move 12 -> 28 and finalize.
    Sensor(clock, sm, 12, false);
    sm.SetValidDestMask(1ULL << 28);
    Sensor(clock, sm, 28, true);
    AdvanceAndTick(clock, sm, 25);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "forward settle exposes its action");

    // Mirror the real coordinator catch-up after a finalized move so the edge
    // renderer is validating the turn animation rather than the generic error overlay.
    sm.SetExpectedOccupancyMask((1ULL << 7) | (1ULL << 28));
    sm.SetExpectedSideToMove(modules::PersistedSide::White);
    AdvanceAndTick(clock, sm, 1);
    Check(!sm.HasHumanIllegal(), "forward position clears human-illegal edge overlay");
    Check(!sm.HasCpuIllegal(), "forward position clears cpu-illegal edge overlay");

    // Request undo (prompt should indicate return-to square 12).
    PressP1(clock, sm);
    Check(sm.IsUndoPromptActive(), "undo prompt active");

    modules::PersistedSide undoMoverSide = modules::PersistedSide::Unknown;
    Check(sm.TryGetUndoPromptMoverSide(undoMoverSide), "undo prompt mover side inferred immediately");
    Check(undoMoverSide == modules::PersistedSide::Black, "undo prompt mover side is black");
    modules::PersistedSide undoEdgeSide = modules::PersistedSide::Unknown;
    Check(sm.TryGetStatusEdgeActiveSideOverride(undoEdgeSide), "undo prompt sets status-edge owner immediately");
    Check(undoEdgeSide == modules::PersistedSide::Black, "undo prompt status-edge owner is black");

    uint8_t undoFromSquare = 0;
    uint8_t undoToSquare = 0;
    Check(sm.TryGetUndoPromptMove(undoFromSquare, undoToSquare), "undo prompt move available");
    Check(undoFromSquare == 12, "undo from is 12");
    Check(undoToSquare == 28, "undo to is 28");

    modules::LitchMaxEngine engine;
    engine.Reset();
    Check(engine.SetFen("4k2r/8/8/4p3/8/8/8/4K3 w - - 0 1"), "engine set forward fen");

    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};

    const uint64_t physOcc64 = sm.GetPhysicalOccupancyMask();
    const std::array<bool, 64> occupiedPhysical = Occ64ToOccupiedPhysical(physOcc64);

    modules::led_frame_composer::ComposeInputs in;
    in.settings = &settings;
    in.firstPlayerIsP2 = false;

    fxe::ManualMillis::SetNowMs(0);

    modules::led_frame_composer::internal::ComposeContext composeCtx;
    runtime::InteractionModelSnapshot contextSnapshot0;
    Check(test_support::BuildComposeContextWithSnapshot(
              in, sm, derived, &engine, composeCtx, contextSnapshot0),
          "build compose context");
    Check(composeCtx.activeOwner == modules::UiOwner::P2,
          "undo prompt keeps active owner on the undone side before coordinator update");
    Check(composeCtx.edgeActiveOwner == modules::UiOwner::P2,
          "undo prompt keeps edge animation owner on the undo mover immediately");
    Check(composeCtx.undoMoverPieceFenKnown, "undo mover piece is inferred immediately from engine position");
    Check(composeCtx.undoMoverPieceFen == 'p', "undo mover identity is black pawn immediately");
    Check(composeCtx.undoMoverPieceRgb.b > composeCtx.undoMoverPieceRgb.r,
          "undo mover color uses the correct player palette immediately");

    {
        modules::BoardLedFrame overlayOnlyLeds{};
        std::array<modules::Rgb, 64> overlayOnlyGrid{};
        modules::led_frame_composer::internal::ApplyCenterTextOverlay(
            composeCtx, overlayOnlyLeds, overlayOnlyGrid);
        bool hasBrightNeutralGlyphPixel = false;
        for (const modules::Rgb pixel : overlayOnlyGrid) {
            if (pixel.r >= 200U && pixel.g >= 200U && pixel.b >= 200U) {
                hasBrightNeutralGlyphPixel = true;
                break;
            }
        }
        Check(!hasBrightNeutralGlyphPixel,
              "undo uses spatial guidance without a board-letter glyph");
    }

    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);

    const auto rotatedPhysicalFrame = hardware::MapSemanticLedFrame(
        {hardware::BoardRevision::V2S3, true, true}, ledFrame, edgeFrame);

    const modules::Rgb returnSquare = gridFrame[12];
    const modules::Rgb currentSquare = gridFrame[28];
    const modules::Rgb currentPieceLed = PieceLedAtSquare(ledFrame, 28);

    // The return-to square should be the dominant cue and should use a
    // saturated prompt-color pulse (not neutral near-white).
    const uint8_t returnMax =
        (returnSquare.r > returnSquare.g) ? ((returnSquare.r > returnSquare.b) ? returnSquare.r : returnSquare.b)
                                          : ((returnSquare.g > returnSquare.b) ? returnSquare.g : returnSquare.b);
    const uint8_t returnMin =
        (returnSquare.r < returnSquare.g) ? ((returnSquare.r < returnSquare.b) ? returnSquare.r : returnSquare.b)
                                          : ((returnSquare.g < returnSquare.b) ? returnSquare.g : returnSquare.b);
    Check(static_cast<uint8_t>(returnMax - returnMin) >= 24U,
          "return-to square uses saturated prompt color");
    Check(MaxChannel(returnSquare) >= static_cast<uint8_t>(MaxChannel(currentSquare) + 16U),
          "return-to square remains the stronger single-channel cue");
    Check(Intensity(currentPieceLed) >= 120U,
          "current undo piece stays visible under the undo overlay");

    // Piece LEDs remain off when squares are unoccupied.
    const modules::Rgb returnPieceLed = PieceLedAtSquare(ledFrame, 12);
    Check(Intensity(returnPieceLed) == 0U, "return-to piece LED stays off when square is empty");

    const uint32_t undoP2HomeEdgeIntensity = EdgeRangeIntensity(edgeFrame, 0, 8);
    const uint32_t undoP1HomeEdgeIntensity = EdgeRangeIntensity(edgeFrame, 16, 8);
    Check(undoP2HomeEdgeIntensity > undoP1HomeEdgeIntensity,
          "undo prompt puts the active-turn animation on the undo mover side immediately");

    const modules::Rgb undoHomeLed = BrightestEdgeLed(edgeFrame, 0, 8);
    Check(ColorDistanceSq(undoHomeLed, composeCtx.p2PieceRgb) <
              ColorDistanceSq(undoHomeLed, composeCtx.p1PieceRgb),
          "undo prompt active-turn animation uses the undo mover palette immediately");
    Check(NormalizedColorDistanceSq(undoHomeLed, composeCtx.p2PieceRgb) <= 9U,
          "undo active-side animation uses the direct player color without a hue offset");

    {
        runtime::GameStateMachine smMissing(smCfg);
        smMissing.Reset();

        runtime::ManualClock clockMissing;
        static constexpr uint8_t SAME_TYPE_CANDIDATE_SQUARE = 8U;
        smMissing.SetExpectedOccupancyMask(
            (1ULL << 7) | (1ULL << SAME_TYPE_CANDIDATE_SQUARE) |
            (1ULL << 12));
        smMissing.SetExpectedSideToMove(modules::PersistedSide::White);
        smMissing.DebugSetPhysicalOccupancyMask(
            (1ULL << 7) | (1ULL << SAME_TYPE_CANDIDATE_SQUARE) |
            (1ULL << 12));

        Sensor(clockMissing, smMissing, 12, false);
        smMissing.SetValidDestMask(1ULL << 28);
        Sensor(clockMissing, smMissing, 28, true);
        AdvanceAndTick(clockMissing, smMissing, 25);
        Check(smMissing.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction,
              "missing-piece setup settle exposes its action");

        smMissing.SetExpectedOccupancyMask(
            (1ULL << 7) | (1ULL << SAME_TYPE_CANDIDATE_SQUARE) |
            (1ULL << 28));
        smMissing.SetExpectedSideToMove(modules::PersistedSide::White);
        AdvanceAndTick(clockMissing, smMissing, 1);

        PressP1(clockMissing, smMissing);
        Check(smMissing.IsUndoPromptActive(), "missing-piece setup undo prompt active");

        modules::LitchMaxEngine engineMissing;
        engineMissing.Reset();
        Check(engineMissing.SetFen("4k2r/p7/8/4p3/8/8/8/4K3 w - - 0 1"),
              "missing-piece engine set forward fen");

        modules::EngineDerivedState derivedMissing;
        derivedMissing.RefreshFromEngine(engineMissing);

        std::array<bool, 64> occupiedMissing = Occ64ToOccupiedPhysical(smMissing.GetPhysicalOccupancyMask());

        modules::led_frame_composer::ComposeInputs inMissing;
        inMissing.settings = &settings;
        inMissing.firstPlayerIsP2 = false;

        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledMissing{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeMissing{};
        std::array<modules::Rgb, 64> gridMissing{};
        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledMissingLater{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeMissingLater{};
        std::array<modules::Rgb, 64> gridMissingLater{};

        Sensor(clockMissing, smMissing, 28, false);
        occupiedMissing = Occ64ToOccupiedPhysical(smMissing.GetPhysicalOccupancyMask());

        fxe::ManualMillis::SetNowMs(0);
        modules::led_frame_composer::internal::ComposeContext missingComposeCtx;
        runtime::InteractionModelSnapshot contextSnapshot1;
        Check(test_support::BuildComposeContextWithSnapshot(
                  inMissing, smMissing, derivedMissing, &engineMissing, missingComposeCtx, contextSnapshot1),
              "build compose context after undo piece pickup");
        const modules::led_frame_composer::internal::InvalidBoardState missingInvalidBoard =
            modules::led_frame_composer::internal::ComputeInvalidBoardState(missingComposeCtx);
        Check(!missingInvalidBoard.IsActive(), "undo piece in hand stays on the normal undo baseline");
        Check(missingComposeCtx.humanIllegalMask == 0ULL,
              "undo piece in hand does not latch the human-illegal mask");
        const auto missingRuntime =
            modules::led_frame_composer::internal::BuildBoardComposeRuntime(
                missingComposeCtx);
        Check((missingRuntime.undoCandidateBlinkMask &
               (1ULL << SAME_TYPE_CANDIDATE_SQUARE)) != 0ULL,
              "ordinary mover-in-hand guidance identifies a matching occupied piece");
        Check((missingRuntime.undoMoverInitialMask & (1ULL << 9U)) != 0ULL,
              "ordinary mover-in-hand guidance identifies conventional starting squares");
        test_support::ComposeWithSnapshot(
            inMissing,
            smMissing,
            derivedMissing,
            &engineMissing,
            ledMissing,
            edgeMissing,
            gridMissing);

        const modules::Rgb missingSquare0 =
            SquareLedAtSquare(ledMissing, 12, hardware::SquareLedSlot::Square0);
        const modules::Rgb missingPiece = PieceLedAtSquare(ledMissing, 12);
        const modules::Rgb missingSquare2 =
            SquareLedAtSquare(ledMissing, 12, hardware::SquareLedSlot::Square2);
        const modules::Rgb missingCurrentSquare0 =
            SquareLedAtSquare(ledMissing, 28, hardware::SquareLedSlot::Square0);
        const modules::Rgb missingCurrentPiece = PieceLedAtSquare(ledMissing, 28);
        const modules::Rgb missingCurrentSquare2 =
            SquareLedAtSquare(ledMissing, 28, hardware::SquareLedSlot::Square2);
        const modules::Rgb missingReturnGrid = gridMissing[12];
        const modules::Rgb missingCurrentGrid = gridMissing[28];
        Check(Intensity(missingSquare0) > 0U, "undo piece in hand keeps the return square cued immediately");
        Check(Intensity(missingPiece) == 0U, "undo piece in hand does not mark the return square as invalid");
        Check(Intensity(missingSquare2) > 0U, "undo piece in hand keeps the return square frame lit");
        Check(Intensity(missingCurrentPiece) == 0U,
              "undo piece in hand leaves the pulled-from piece slot dark");
        Check(MaxChannel(missingReturnGrid) >= static_cast<uint8_t>(MaxChannel(missingCurrentGrid) + 16U),
              "undo piece in hand keeps the return square as the stronger prompt cue");
        Check(ColorDistanceSq(missingSquare0, missingSquare2) < 4096U,
              "undo piece in hand keeps a single prompt family on the return square");
        const uint32_t missingP2HomeEdgeIntensity = EdgeRangeIntensity(edgeMissing, 0, 8);
        const uint32_t missingP1HomeEdgeIntensity = EdgeRangeIntensity(edgeMissing, 16, 8);
        Check(missingP2HomeEdgeIntensity > missingP1HomeEdgeIntensity,
              "undo piece in hand keeps the normal undo active-side edge animation");

        fxe::ManualMillis::SetNowMs(240);
        test_support::ComposeWithSnapshot(
            inMissing,
            smMissing,
            derivedMissing,
            &engineMissing,
            ledMissingLater,
            edgeMissingLater,
            gridMissingLater);
        Check(MaxChannel(gridMissingLater[12]) >= static_cast<uint8_t>(MaxChannel(gridMissingLater[28]) + 16U),
              "undo piece in hand keeps the return square as the stronger cue over time");
        Check(Intensity(PieceLedAtSquare(ledMissingLater, 28)) == 0U,
              "undo piece in hand keeps the pulled-from piece slot dark over time");

        fxe::ManualMillis::SetNowMs(10);
        test_support::ComposeWithSnapshot(
            inMissing,
            smMissing,
            derivedMissing,
            &engineMissing,
            ledMissingLater,
            edgeMissingLater,
            gridMissingLater);
        Check(EdgeRangeIntensity(edgeMissingLater, 0, 8) > EdgeRangeIntensity(edgeMissingLater, 16, 8),
              "undo piece in hand keeps the normal undo edge owner during later frames");

        uint16_t candidateMin = UINT16_MAX;
        uint16_t candidateMax = 0U;
        bool startReferenceVisible = false;
        for (uint32_t sampleMs = 0U; sampleMs <= 1600U; sampleMs += 100U) {
            fxe::ManualMillis::SetNowMs(sampleMs);
            test_support::ComposeWithSnapshot(
                inMissing,
                smMissing,
                derivedMissing,
                &engineMissing,
                ledMissingLater,
                edgeMissingLater,
                gridMissingLater);
            const uint16_t candidateIntensity = Intensity(
                PieceLedAtSquare(ledMissingLater,
                                 SAME_TYPE_CANDIDATE_SQUARE));
            if (candidateIntensity < candidateMin) {
                candidateMin = candidateIntensity;
            }
            if (candidateIntensity > candidateMax) {
                candidateMax = candidateIntensity;
            }
            // Squares 9 and 18 share board parity and are empty; only 9 is a
            // conventional black-pawn starting square.
            startReferenceVisible =
                startReferenceVisible ||
                ColorDistanceSq(gridMissingLater[9U],
                                gridMissingLater[18U]) != 0U;
        }
        Check(candidateMax > candidateMin,
              "ordinary mover-in-hand matching piece visibly pulses");
        Check(startReferenceVisible,
              "ordinary mover-in-hand starting-square reference is visually distinct");

        fxe::ManualMillis::SetNowMs(0);
    }

    // Validate the focused composer's direct logical-to-physical mapping.
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);

    const uint8_t returnPhysical = runtime::MapSquareIndex(12, true);
    const uint8_t currentPhysical = runtime::MapSquareIndex(28, true);
    Check(returnPhysical < 64U, "returnPhysical valid");
    Check(currentPhysical < 64U, "currentPhysical valid");

    const modules::Rgb returnSquareRot = rotatedPhysicalFrame[
        hardware::LedIndexForSquare(returnPhysical,
                                    hardware::SquareLedSlot::Square0)];
    const modules::Rgb currentSquareRot = rotatedPhysicalFrame[
        hardware::LedIndexForSquare(currentPhysical,
                                    hardware::SquareLedSlot::Square0)];

    const uint8_t returnRotMax = (returnSquareRot.r > returnSquareRot.g)
                                     ? ((returnSquareRot.r > returnSquareRot.b) ? returnSquareRot.r : returnSquareRot.b)
                                     : ((returnSquareRot.g > returnSquareRot.b) ? returnSquareRot.g : returnSquareRot.b);
    const uint8_t returnRotMin = (returnSquareRot.r < returnSquareRot.g)
                                     ? ((returnSquareRot.r < returnSquareRot.b) ? returnSquareRot.r : returnSquareRot.b)
                                     : ((returnSquareRot.g < returnSquareRot.b) ? returnSquareRot.g : returnSquareRot.b);
    Check(static_cast<uint8_t>(returnRotMax - returnRotMin) >= 24U,
          "rotate: return-to square uses saturated prompt color");
    Check(MaxChannel(returnSquareRot) >= static_cast<uint8_t>(MaxChannel(currentSquareRot) + 16U),
          "rotate: return-to square remains the stronger single-channel cue");

    // Production keeps semantic composition unflipped and applies square
    // rotation at the hardware-output boundary. Exercise the overlay flag
    // independently so rotation cannot revive a text glyph.
    runtime::InteractionModelSnapshot contextSnapshot2;
    Check(test_support::BuildComposeContextWithSnapshot(
              in, sm, derived, &engine, composeCtx, contextSnapshot2),
          "build rotated-output Undo compose context");
    {
        modules::BoardLedFrame overlayOnlyLeds{};
        std::array<modules::Rgb, 64> overlayOnlyGrid{};
        modules::led_frame_composer::internal::ApplyCenterTextOverlay(
            composeCtx, overlayOnlyLeds, overlayOnlyGrid);
        bool hasBrightNeutralGlyphPixel = false;
        for (const modules::Rgb pixel : overlayOnlyGrid) {
            if (pixel.r >= 200U && pixel.g >= 200U && pixel.b >= 200U) {
                hasBrightNeutralGlyphPixel = true;
                break;
            }
        }
        Check(!hasBrightNeutralGlyphPixel,
              "rotated-output Undo remains spatial and does not revive a board-letter glyph");
    }

    // GAME_FIRST_PLAYER swaps logical player placement while the final output
    // mapping keeps each palette beside its fixed printed control bank.
    in.firstPlayerIsP2 = true;
    settings.Doc()["GAME_FIRST_PLAYER"] = 2U;
    runtime::InteractionModelSnapshot contextSnapshot3;
    Check(test_support::BuildComposeContextWithSnapshot(
              in, sm, derived, &engine, composeCtx, contextSnapshot3),
          "build swapped-player-end Undo compose context");
    Check(composeCtx.activeOwner == modules::UiOwner::P2 &&
              composeCtx.edgeActiveOwner == modules::UiOwner::P2,
          "GAME_FIRST_PLAYER preserves logical black/P2 Undo ownership");
    Check(composeCtx.undoMoverPieceRgb.r > composeCtx.undoMoverPieceRgb.b,
          "logical P2 Undo uses its newly assigned near-end palette");
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> physicalFrame{};
    for (uint16_t index = 0U; index < hardware::BOARD_LED_COUNT; ++index) {
        physicalFrame[hardware::MapLedIndexForSquareAndEdgeRotate(
            index, true, false)] = ledFrame[index];
    }
    const uint8_t swappedCurrentPhysical =
        runtime::MapSquareIndex(28U, true);
    const uint8_t swappedReturnPhysical =
        runtime::MapSquareIndex(12U, true);
    Check(PieceLedAtSquare(physicalFrame, swappedCurrentPhysical).r >
              PieceLedAtSquare(physicalFrame, swappedCurrentPhysical).b,
          "GAME_FIRST_PLAYER keeps the current Undo piece in the palette fixed to its physical end");
    Check(Intensity(SquareLedAtSquare(
              physicalFrame,
              swappedReturnPhysical,
              hardware::SquareLedSlot::Square0)) > 0U,
          "GAME_FIRST_PLAYER maps the Undo return prompt to its physical output square");
    Check(EdgeRangeIntensity(edgeFrame, 0, 8) >
              EdgeRangeIntensity(edgeFrame, 16, 8),
          "Undo owner animation remains canonical before physical mapping");
    in.firstPlayerIsP2 = false;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;

    Sensor(clock, sm, 7, false);
    Check(sm.HasHumanIllegal(), "undo prompt unrelated lift latches human-illegal");

    fxe::ManualMillis::SetNowMs(0);
    runtime::InteractionModelSnapshot contextSnapshot4;
    Check(test_support::BuildComposeContextWithSnapshot(
              in, sm, derived, &engine, composeCtx, contextSnapshot4),
          "build compose context after undo prompt invalid lift");
    {
        auto rawMismatchCtx = composeCtx;
        rawMismatchCtx.humanIllegalMask = 0ULL;
        rawMismatchCtx.cpuIllegalMask = 0ULL;
        const modules::led_frame_composer::internal::InvalidBoardState rawMismatchInvalidBoard =
            modules::led_frame_composer::internal::ComputeInvalidBoardState(rawMismatchCtx);
        Check(!rawMismatchInvalidBoard.IsActive(),
              "undo prompt does not promote physical differences into mismatch-rainbow feedback");

        auto rawMismatchSnapshot = composeCtx.Snapshot();
        rawMismatchSnapshot.interaction.placementModeActive = true;
        rawMismatchSnapshot.interaction.placementMissingMask = (1ULL << 7);
        rawMismatchCtx.snapshot = &rawMismatchSnapshot;
        Check(!modules::led_frame_composer::internal::ComputeInvalidBoardState(rawMismatchCtx).IsActive(),
              "undo feedback ownership wins over a stale compatibility placement flag");
    }
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);

    const modules::Rgb unrelatedSquare0 =
        SquareLedAtSquare(ledFrame, 7, hardware::SquareLedSlot::Square0);
    const modules::Rgb unrelatedPiece = PieceLedAtSquare(ledFrame, 7);
    const modules::Rgb unrelatedSquare2 =
        SquareLedAtSquare(ledFrame, 7, hardware::SquareLedSlot::Square2);
    Check(Intensity(unrelatedSquare0) > 0U, "undo prompt invalid lift keeps a local correction cue");
    Check(Intensity(unrelatedPiece) == 0U, "undo prompt invalid lift uses a local red square border");
    Check(Intensity(unrelatedSquare2) > 0U, "undo prompt invalid lift keeps the local square frame lit");
    Check(ColorDistanceSq(unrelatedSquare0, unrelatedSquare2) == 0U &&
              unrelatedSquare0.r > unrelatedSquare0.g &&
              unrelatedSquare0.r > unrelatedSquare0.b,
          "undo prompt invalid lift uses one flat local red cue rather than a rainbow");

    Check(!AllEdgesShowRedFault(edgeFrame),
          "undo prompt invalid lift does not escalate to a whole-board edge fault");

    fxe::ManualMillis::SetNowMs(10);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    Check(!AllEdgesShowRedFault(edgeFrame),
          "undo prompt invalid lift keeps the whole-board edge fault suppressed over time");

    fxe::ManualMillis::SetNowMs(0);

    Sensor(clock, sm, 7, true);
    Check(!sm.HasHumanIllegal(), "restoring unrelated piece clears undo prompt illegal cue");

    // Simulate the coordinator having consumed/applied the undo before the user
    // backs out with cancel-undo. The engine is now at the undone position,
    // while the physical board still has the piece at the forward square.
    const uint32_t undoToken =
        sm.GetChessInteractionModel().phase.pendingActionToken;
    Check(undoToken != 0U, "undo exposes action token for simulated coordinator");
    sm.HandleInteractionActionResult(undoToken, true);
    sm.SetExpectedOccupancyMask((1ULL << 7) | (1ULL << 12));
    sm.SetExpectedSideToMove(modules::PersistedSide::Black);
    Check(engine.SetFen("4k2r/4p3/8/8/8/8/8/4K3 b - - 0 1"), "engine set undone fen");
    derived.RefreshFromEngine(engine);

    PressP1(clock, sm);
    Check(!sm.IsUndoPromptActive(), "cancel-undo clears undo prompt immediately");

    modules::PersistedSide visualSide = modules::PersistedSide::Unknown;
    Check(sm.TryGetVisualActiveSideOverride(visualSide), "cancel-undo sets visual active-side override");
    Check(visualSide == modules::PersistedSide::White, "cancel-undo visual side matches forward position");
    modules::PersistedSide cancelEdgeSide = modules::PersistedSide::Unknown;
    Check(sm.TryGetStatusEdgeActiveSideOverride(cancelEdgeSide), "cancel-undo sets status-edge owner immediately");
    Check(cancelEdgeSide == modules::PersistedSide::White, "cancel-undo status-edge owner matches forward position");

    runtime::InteractionModelSnapshot contextSnapshot5;
    Check(test_support::BuildComposeContextWithSnapshot(
              in, sm, derived, &engine, composeCtx, contextSnapshot5),
          "build compose context after cancel-undo");
    Check(composeCtx.activeOwner == modules::UiOwner::P1,
          "cancel-undo keeps active owner on the forward side before coordinator update");
    Check(composeCtx.edgeActiveOwner == modules::UiOwner::P1,
          "cancel-undo keeps edge animation owner on the forward side before coordinator update");
    Check(runtime::IsCancelUndoTransitionActive(
              composeCtx.Snapshot()),
          "cancel-undo stays visible through explicit render transition state");
    Check(composeCtx.Snapshot().render.moveTransition.fromSquare == 12U &&
              composeCtx.Snapshot().render.moveTransition.toSquare == 28U,
          "cancel-undo exposes one render-only move identity");
    Check(!modules::led_frame_composer::internal::ComputeInvalidBoardState(composeCtx).IsActive(),
          "cancel-undo suppresses raw mismatch fault cues during the in-flight transition");

    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    const modules::Rgb cancelPieceLed = PieceLedAtSquare(ledFrame, 28);
    Check(ColorDistanceSq(cancelPieceLed, composeCtx.p2PieceRgb) < ColorDistanceSq(cancelPieceLed, composeCtx.p1PieceRgb),
          "cancel-undo keeps the moved piece closer to the correct player palette before coordinator update");

    const uint32_t p2HomeEdgeIntensity = EdgeRangeIntensity(edgeFrame, 0, 8);
    const uint32_t p1HomeEdgeIntensity = EdgeRangeIntensity(edgeFrame, 16, 8);
    Check(p1HomeEdgeIntensity > p2HomeEdgeIntensity,
          "cancel-undo returns the active-turn animation to the forward side immediately");

    const modules::Rgb cancelHomeLed = BrightestEdgeLed(edgeFrame, 16, 8);
    Check(ColorDistanceSq(cancelHomeLed, composeCtx.p1PieceRgb) <
              ColorDistanceSq(cancelHomeLed, composeCtx.p2PieceRgb),
          "cancel-undo edge animation returns to the forward palette immediately");
    Check(NormalizedColorDistanceSq(cancelHomeLed, composeCtx.p1PieceRgb) <= 9U,
          "cancel-undo active-side animation restores the direct player color");

    if (g_failures == 0) {
        std::puts("[UNDO_PROMPT_LED_UX] ok");
        return 0;
    }

    std::printf("[UNDO_PROMPT_LED_UX] failures=%d\n", g_failures);
    return 1;
}
