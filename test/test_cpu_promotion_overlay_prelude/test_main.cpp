#include <array>
#include <cstdint>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

#include "hardware/led_mapping.hpp"
#include "hardware/led_layout.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_text_renderer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/orientation.hpp"
#include "modules/promotion_piece_cue.hpp"
#include "modules/settings_store.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "modules/ui_color_keys.hpp"
#include "modules/ui_color_presets.hpp"
#include "runtime/game_state_machine.hpp"
#include "../support/led_frame_composer_snapshot_adapter.hpp"

namespace {

uint16_t Intensity(const modules::Rgb c) {
    return static_cast<uint16_t>(c.r) + static_cast<uint16_t>(c.g) + static_cast<uint16_t>(c.b);
}

uint64_t RotateMask180(const uint64_t mask) {
    uint64_t rotated = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((mask & (UINT64_C(1) << square)) != 0ULL) {
            rotated |= UINT64_C(1) << (63U - square);
        }
    }
    return rotated;
}

uint64_t IconAreaMask() {
    uint64_t mask = 0ULL;
    for (uint8_t row = 0U;
         row < modules::promotion_piece_cue::ICON_HEIGHT; ++row) {
        for (uint8_t column = 0U;
             column < modules::promotion_piece_cue::ICON_WIDTH; ++column) {
            const uint8_t square = static_cast<uint8_t>(
                (modules::promotion_piece_cue::ICON_ORIGIN_Y + row) * 8U +
                modules::promotion_piece_cue::ICON_ORIGIN_X + column);
            mask |= UINT64_C(1) << square;
        }
    }
    return mask;
}

uint64_t LitGridMask(const std::array<modules::Rgb, 64>& frame,
                     const uint16_t threshold = 1U) {
    uint64_t mask = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if (Intensity(frame[square]) >= threshold) {
            mask |= UINT64_C(1) << square;
        }
    }
    return mask;
}

uint32_t LitEdgeMask(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& frame) {
    uint32_t mask = 0U;
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        if (Intensity(frame[index]) != 0U) {
            mask |= UINT32_C(1) << index;
        }
    }
    return mask;
}

uint32_t RotateEdgeMask180(const uint32_t mask) {
    uint32_t rotated = 0U;
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        if ((mask & (UINT32_C(1) << index)) != 0U) {
            const uint8_t mapped = static_cast<uint8_t>(
                (index + modules::EDGE_LED_COUNT / 2U) %
                modules::EDGE_LED_COUNT);
            rotated |= UINT32_C(1) << mapped;
        }
    }
    return rotated;
}

bool SameFrame(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& left,
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& right) {
    for (size_t index = 0U; index < left.size(); ++index) {
        if (left[index].r != right[index].r ||
            left[index].g != right[index].g ||
            left[index].b != right[index].b) {
            return false;
        }
    }
    return true;
}

bool SameGrid(const std::array<modules::Rgb, 64>& left,
              const std::array<modules::Rgb, 64>& right) {
    for (size_t index = 0U; index < left.size(); ++index) {
        if (left[index].r != right[index].r ||
            left[index].g != right[index].g ||
            left[index].b != right[index].b) {
            return false;
        }
    }
    return true;
}

bool SameBoard(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& left,
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& right) {
    for (size_t index = 0U; index < left.size(); ++index) {
        if (left[index].r != right[index].r ||
            left[index].g != right[index].g ||
            left[index].b != right[index].b) {
            return false;
        }
    }
    return true;
}

bool IsFullRedOnly(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& frame) {
    for (const modules::Rgb color : frame) {
        if (color.r == 0U || color.g != 0U || color.b != 0U) {
            return false;
        }
    }
    return true;
}

runtime::InteractionModelSnapshot PromotionSnapshot(
    const bool cpu,
    const modules::PromotionPiece piece,
    const modules::SideToMove moverSide,
    const uint32_t elapsedMs = 800U) {
    runtime::InteractionModelSnapshot snapshot;
    snapshot.system.nowMs = elapsedMs;
    snapshot.game.sideToMove = moverSide;
    snapshot.game.setupLifecycle = runtime::SetupLifecycle::Gameplay;
    snapshot.interaction.taggedState.tag =
        cpu ? runtime::ChessInteractionPhase::ExecutingPlan
            : runtime::ChessInteractionPhase::AwaitingAction;
    auto& plan = snapshot.interaction.taggedState.plan;
    plan.kind = runtime::PhysicalTransitionKind::Promotion;
    plan.source = cpu ? runtime::PhysicalTransitionSource::Cpu
                      : runtime::PhysicalTransitionSource::Human;
    plan.moveFrom = 0U;
    plan.moveTo = 63U;
    plan.relocations[0] = runtime::PieceRelocation{0U, 63U};
    plan.relocationCount = 1U;
    plan.promotionRequired = true;
    snapshot.interaction.promotionSelection = piece;
    snapshot.interaction.cpuMovePromptElapsedMs = elapsedMs;
    if (cpu) {
        static constexpr char SUFFIXES[] = {'q', 'r', 'b', 'n'};
        snapshot.interaction.cpuPromotionValid = true;
        snapshot.interaction.cpuPromotionSuffixLower =
            SUFFIXES[static_cast<uint8_t>(piece)];
    }
    return snapshot;
}

void MapToPhysical(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& semanticBoard,
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& semanticEdge,
    const bool squareRotate,
    const bool boardRotate,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& physical) {
    physical.fill(modules::RGB_BLACK);
    for (uint16_t index = 0U; index < hardware::BOARD_LED_COUNT; ++index) {
        const uint16_t mapped =
            hardware::MapLedIndexForSquareAndEdgeRotate(
                index, squareRotate, boardRotate);
        physical[mapped] = semanticBoard[index];
    }
    modules::status_led_frame_adapter::ApplyToDedicatedChain(
        semanticEdge, boardRotate, physical);
}

uint64_t LitPhysicalSurfaceMask(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& frame) {
    uint64_t mask = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        const modules::Rgb square0 = frame[hardware::LedIndexForSquare(
            square, hardware::SquareLedSlot::Square0)];
        const modules::Rgb square2 = frame[hardware::LedIndexForSquare(
            square, hardware::SquareLedSlot::Square2)];
        if (Intensity(square0) != 0U || Intensity(square2) != 0U) {
            mask |= UINT64_C(1) << square;
        }
    }
    return mask;
}

uint32_t LitPhysicalEdgeMask(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& frame) {
    uint32_t mask = 0U;
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        const uint16_t physical = static_cast<uint16_t>(
            hardware::FirstEdgeLedIndex() + index);
        if (Intensity(frame[physical]) != 0U) {
            mask |= UINT32_C(1) << index;
        }
    }
    return mask;
}

bool IconUsesOnlyColor(
    const std::array<modules::Rgb, 64>& grid,
    const uint64_t iconMask,
    const modules::Rgb expectedColor) {
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((iconMask & (UINT64_C(1) << square)) == 0ULL) {
            continue;
        }
        const modules::Rgb color = grid[square];
        if (color.r != expectedColor.r || color.g != expectedColor.g ||
            color.b != expectedColor.b) {
            return false;
        }
    }
    return true;
}

std::array<bool, 64> Occ64ToOccupiedPhysical(const uint64_t occ64) {
    std::array<bool, 64> out{};
    for (uint8_t sq = 0; sq < 64; ++sq) {
        out[sq] = ((occ64 >> sq) & 1ULL) != 0ULL;
    }
    return out;
}

} // namespace

int main() {
    fxe::test_support::TestResults results("CPU_PROMOTION_OVERLAY_PRELUDE");
    results.Start();

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_cpu_promotion_overlay_prelude.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();
    settings.Doc()["P1_PIECE_WHEEL"] = 170U;
    settings.Doc()["P2_PIECE_WHEEL"] = 0U;
    settings.Doc()[modules::ui_color_keys::P1_END_PIECE_WHEEL] = 170U;
    settings.Doc()[modules::ui_color_keys::P2_END_PIECE_WHEEL] = 0U;
    settings.NotifyDocChanged();

    runtime::GameStateMachineConfig smCfg;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    // Minimal expected/physical occupancy so the CPU move prompt is active.
    sm.SetExpectedOccupancyMask(1ULL << 28);
#ifndef ARDUINO
    sm.DebugSetPhysicalOccupancyMask(1ULL << 28);
#endif

    // Queue a CPU move with a promotion suffix so the center overlay would normally appear.
    results.Check(sm.QueueCpuMove(28, 20, false, 0, 'q'), "queue cpu move with promo suffix");
    results.Check((sm.HasQueuedCpuMove() && !sm.IsMoveSettlingActive()), "entered CpuMoving");

    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};

    const modules::Rgb ordinaryBackground = modules::ScaleRgbSaturatingBy256(
        modules::ui_color_presets::SQUARE_COLOR_PRESETS[0], 320U);

    // Compose at t=0: the promotion overlay should be suppressed (prelude), so
    // the background should NOT be globally dimmed.
    {
        const auto occupiedPhysical = Occ64ToOccupiedPhysical(sm.GetPhysicalOccupancyMask());

        modules::led_frame_composer::ComposeInputs in;
        in.settings = &settings;
        in.firstPlayerIsP2 = false;

        fxe::ManualMillis::SetNowMs(0);
        test_support::ComposeWithSnapshot(
            in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);

        // a8 is a light square in the canonical mapping.
        results.Check(gridFrame[0].r == ordinaryBackground.r &&
                          gridFrame[0].g == ordinaryBackground.g &&
                          gridFrame[0].b == ordinaryBackground.b,
                      "t=0: background not dimmed by promo overlay");
    }

    // Compose after the prelude: the promotion overlay should appear, dimming the background.
    {
        const auto occupiedPhysical = Occ64ToOccupiedPhysical(sm.GetPhysicalOccupancyMask());

        modules::led_frame_composer::ComposeInputs in;
        in.settings = &settings;
        in.firstPlayerIsP2 = false;

        fxe::ManualMillis::SetNowMs(800);
        test_support::ComposeWithSnapshot(
            in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);

        const uint16_t dim0 = Intensity(gridFrame[0]);
        results.Check(dim0 < Intensity(ordinaryBackground),
                      "t=800ms: background dimmed by promo overlay");

        const uint8_t toPhysical = modules::MapSquareToPhysical(20, false);
        const uint16_t dimTo = Intensity(gridFrame[toPhysical]);
        results.Check(dimTo >= static_cast<uint16_t>(dim0 + 25U),
                      "t=800ms: promo destination square remains highlighted");
        results.Check(
            (LitGridMask(gridFrame, 64U) & IconAreaMask()) ==
                modules::promotion_piece_cue::BoardMask(
                    modules::PromotionPiece::Queen),
            "t=800ms: CPU promotion shows a Queen silhouette, not a letter");
        results.Check(LitEdgeMask(edgeFrame) == 0U,
                      "CPU promotion never advertises human selection controls");
    }

    // Remove square-background light for exact silhouette-mask comparisons.
    settings.Doc()["WHITE_SQUARES_ON"] = 0U;
    settings.NotifyDocChanged();

    bool allFlowPieceMasksExact = true;
    bool allMoverColorsExact = true;
    bool allPlacementRotationMappingsExact = true;
    bool allHumanCpuEdgePoliciesExact = true;
    constexpr modules::PromotionPiece PIECES[] = {
        modules::PromotionPiece::Queen,
        modules::PromotionPiece::Rook,
        modules::PromotionPiece::Bishop,
        modules::PromotionPiece::Knight,
    };
    constexpr modules::SideToMove MOVER_SIDES[] = {
        modules::SideToMove::White,
        modules::SideToMove::Black,
    };
    for (const bool cpu : {false, true}) {
        for (const modules::PromotionPiece piece : PIECES) {
            for (const modules::SideToMove moverSide : MOVER_SIDES) {
                for (const bool firstPlayerIsP2 : {false, true}) {
                    for (const bool boardRotate : {false, true}) {
                        runtime::InteractionModelSnapshot snapshot =
                            PromotionSnapshot(cpu, piece, moverSide);
                        modules::led_frame_composer::ComposeInputs in;
                        in.settings = &settings;
                        in.interactionSnapshot = &snapshot;
                        in.firstPlayerIsP2 = firstPlayerIsP2;

                        modules::led_frame_composer::Compose(
                            in, ledFrame, edgeFrame, gridFrame);
                        const uint64_t expectedIcon =
                            modules::promotion_piece_cue::BoardMask(piece);
                        allFlowPieceMasksExact &=
                            (LitGridMask(gridFrame) & IconAreaMask()) ==
                            expectedIcon;
                        // The mover selects its assigned physical end's hue.
                        const bool moverUsesP2ReferencePalette =
                            (moverSide == modules::SideToMove::Black) != firstPlayerIsP2;
                        const modules::Rgb expectedMoverColor =
                            moverUsesP2ReferencePalette ? modules::RGB_RED
                                                       : modules::RGB_BLUE;
                        allMoverColorsExact &= IconUsesOnlyColor(
                            gridFrame, expectedIcon,
                            expectedMoverColor);

                        std::array<modules::Rgb,
                                   hardware::TOTAL_LED_COUNT> physical{};
                        MapToPhysical(
                            ledFrame, edgeFrame,
                            modules::ComputeSquareRotate(
                                boardRotate, firstPlayerIsP2),
                            boardRotate, physical);
                        const uint64_t physicalIconArea =
                            modules::ComputeSquareRotate(
                                boardRotate, firstPlayerIsP2)
                                ? RotateMask180(IconAreaMask())
                                : IconAreaMask();
                        const uint64_t expectedPhysicalIcon =
                            modules::ComputeSquareRotate(
                                boardRotate, firstPlayerIsP2)
                                ? RotateMask180(expectedIcon)
                                : expectedIcon;
                        allPlacementRotationMappingsExact &=
                            (LitPhysicalSurfaceMask(physical) &
                             physicalIconArea) == expectedPhysicalIcon;

                        const uint32_t expectedSemanticEdge =
                            cpu ? 0U
                                : modules::promotion_piece_cue::
                                      HUMAN_LEFT_RIGHT_EDGE_MASK;
                        const uint32_t expectedPhysicalEdge =
                            boardRotate
                                ? RotateEdgeMask180(expectedSemanticEdge)
                                : expectedSemanticEdge;
                        allHumanCpuEdgePoliciesExact &=
                            LitEdgeMask(edgeFrame) == expectedSemanticEdge &&
                            LitPhysicalEdgeMask(physical) ==
                                expectedPhysicalEdge;
                    }
                }
            }
        }
    }
    results.Check(
        allFlowPieceMasksExact,
        "human and CPU flows render all four exact chess-piece silhouettes");
    results.Check(
        allMoverColorsExact,
        "all player/orientation quadrants preserve the mover's placement-selected color through literal rotation, with no neutral letter pixels");
    results.Check(
        allPlacementRotationMappingsExact,
        "all P1/P2 end-placement and BOARD_ROTATE quadrants preserve the rotation-safe silhouette");
    results.Check(
        allHumanCpuEdgePoliciesExact,
        "only human promotion shows restrained Left/Right side-edge guidance in every orientation");

    // Higher-priority health status must remain bit-for-bit identical whether
    // or not the human promotion modal is present.
    runtime::InteractionModelSnapshot humanPromotion = PromotionSnapshot(
        false, modules::PromotionPiece::Bishop,
        modules::SideToMove::White);
    modules::led_frame_composer::ComposeInputs healthIn;
    healthIn.settings = &settings;
    healthIn.interactionSnapshot = &humanPromotion;
    healthIn.runtimeHealth.state =
        runtime::RuntimeHealthState::InputPaused;
    healthIn.runtimeHealth.advice.boardInputAllowed = false;
    healthIn.runtimeHealth.advice.moveAcceptanceAllowed = false;
    healthIn.runtimeHealth.advice.clocksAllowed = false;
    modules::led_frame_composer::Compose(
        healthIn, ledFrame, edgeFrame, gridFrame);
    const auto promotionHealthFrame = edgeFrame;

    runtime::InteractionModelSnapshot idleHealth = humanPromotion;
    idleHealth.interaction = runtime::InteractionStateView{};
    healthIn.interactionSnapshot = &idleHealth;
    modules::led_frame_composer::Compose(
        healthIn, ledFrame, edgeFrame, gridFrame);
    results.Check(
        SameFrame(promotionHealthFrame, edgeFrame),
        "promotion guidance cannot replace higher-priority paused-health status");

    // A foreground menu owns the board and suppresses both the silhouette and
    // the promotion edge affordance.
    modules::led_frame_composer::ComposeInputs menuIn;
    menuIn.settings = &settings;
    menuIn.interactionSnapshot = &humanPromotion;
    menuIn.menuActive = true;
    menuIn.menuVisualAlpha255 = 255U;
    modules::led_frame_composer::Compose(
        menuIn, ledFrame, edgeFrame, gridFrame);
    results.Check(
        (LitGridMask(gridFrame) & IconAreaMask()) == 0ULL,
        "foreground menu suppresses the promotion silhouette");
    results.Check(
        (LitEdgeMask(edgeFrame) &
         modules::promotion_piece_cue::HUMAN_LEFT_RIGHT_EDGE_MASK) == 0U,
        "foreground menu suppresses promotion side-edge guidance");

    // Fatal presentation suppresses the complete promotion layer. Compare it
    // with the same fatal snapshot after removing the promotion plan so a
    // coincidental celebration pixel cannot masquerade as an icon pixel.
    runtime::InteractionModelSnapshot fatalPromotion = humanPromotion;
    fatalPromotion.game.inCheckmate = true;
    fatalPromotion.game.checkmateStartValid = true;
    fatalPromotion.game.checkmateStartMs = 0U;
    modules::led_frame_composer::ComposeInputs fatalIn;
    fatalIn.settings = &settings;
    fatalIn.interactionSnapshot = &fatalPromotion;
    modules::led_frame_composer::Compose(
        fatalIn, ledFrame, edgeFrame, gridFrame);
    const auto fatalPromotionEdge = edgeFrame;
    const auto fatalPromotionGrid = gridFrame;

    runtime::InteractionModelSnapshot fatalIdle = fatalPromotion;
    fatalIdle.interaction = runtime::InteractionStateView{};
    fatalIn.interactionSnapshot = &fatalIdle;
    modules::led_frame_composer::Compose(
        fatalIn, ledFrame, edgeFrame, gridFrame);
    results.Check(
        SameFrame(fatalPromotionEdge, edgeFrame) &&
            SameGrid(fatalPromotionGrid, gridFrame),
        "fatal presentation suppresses the silhouette and side-edge guidance");

    // A contradictory board observation during the modal must give the whole
    // 8x8 and edge surface to correction. Use C2 because it overlaps every
    // broad promotion silhouette body and would expose icon overwrites.
    runtime::InteractionModelSnapshot correctionPromotion = humanPromotion;
    correctionPromotion.board.expectedOccupiedMask = UINT64_C(1) << 50U;
    correctionPromotion.interaction.taggedState.plan.beforeOccupiedMask =
        correctionPromotion.board.expectedOccupiedMask;
    correctionPromotion.interaction.taggedState.plan.afterOccupiedMask =
        correctionPromotion.board.expectedOccupiedMask;
    correctionPromotion.board.physicalOccupiedMask = 0ULL;
    correctionPromotion.interaction.lastInteractionElapsedMs = 1500U;
    modules::led_frame_composer::ComposeInputs correctionIn;
    correctionIn.settings = &settings;
    correctionIn.interactionSnapshot = &correctionPromotion;
    modules::led_frame_composer::Compose(
        correctionIn, ledFrame, edgeFrame, gridFrame);
    const auto promotionCorrectionBoard = ledFrame;
    const auto promotionCorrectionEdge = edgeFrame;
    const auto promotionCorrectionGrid = gridFrame;

    runtime::InteractionModelSnapshot correctionOnly = correctionPromotion;
    correctionOnly.interaction = runtime::InteractionStateView{};
    correctionOnly.interaction.lastInteractionElapsedMs = 1500U;
    correctionIn.interactionSnapshot = &correctionOnly;
    modules::led_frame_composer::Compose(
        correctionIn, ledFrame, edgeFrame, gridFrame);
    results.Check(
        SameBoard(promotionCorrectionBoard, ledFrame) &&
            SameFrame(promotionCorrectionEdge, edgeFrame) &&
            SameGrid(promotionCorrectionGrid, gridFrame) &&
            IsFullRedOnly(edgeFrame),
        "hard correction fully suppresses the promotion icon and guidance");

    correctionPromotion.board.physicalOccupiedMask =
        correctionPromotion.board.expectedOccupiedMask;
    correctionIn.interactionSnapshot = &correctionPromotion;
    modules::led_frame_composer::Compose(
        correctionIn, ledFrame, edgeFrame, gridFrame);
    results.Check(
        (LitGridMask(gridFrame) & IconAreaMask()) ==
                modules::promotion_piece_cue::BoardMask(
                    modules::PromotionPiece::Bishop) &&
            LitEdgeMask(edgeFrame) ==
                modules::promotion_piece_cue::HUMAN_LEFT_RIGHT_EDGE_MASK,
        "restoring the board resumes the same pending promotion selection and controls");
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay controls;
    modules::led_frame_composer::Compose(correctionIn, ledFrame, edgeFrame, gridFrame,
        nullptr, nullptr, nullptr, nullptr, &controls);
    results.Check(controls.controlMask == 0x555U && controls.emulatedControlMask == 0xFFU,
                  "restored Choose owns only Left/Right/O on both accepted control banks");
    correctionPromotion.interaction.taggedState.plan.afterOccupiedMask |= UINT64_C(1) << 63U;
    controls = {};
    modules::led_frame_composer::Compose(correctionIn, ledFrame, edgeFrame, gridFrame,
        nullptr, nullptr, nullptr, nullptr, &controls);
    results.Check(controls.controlMask == 0U && controls.emulatedControlMask == 0U &&
                      (LitEdgeMask(edgeFrame) & modules::promotion_piece_cue::HUMAN_LEFT_RIGHT_EDGE_MASK) == 0U,
                  "physical mismatch with the completed promotion plan cannot advertise Choose controls");


    return results.Finish();
}
