#include "trace_replay_frames.hpp"

#include <algorithm>
#include <array>

#include "hardware/led_layout.hpp"
#include "modules/edge_led_renderer.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/frame_hash.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/led_frame_composer.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#include "runtime/square_mapping.hpp"

namespace trace::replay_internal {

uint64_t MapOccupancyMask(const uint64_t rawMask, const bool squareRotate) {
    uint64_t logicalMask = 0ULL;
    for (uint8_t rawSquare = 0U; rawSquare < 64U; ++rawSquare) {
        const uint64_t mask = 1ULL << rawSquare;
        if ((rawMask & mask) != 0ULL) {
            logicalMask |=
                1ULL << runtime::MapSquareIndex(rawSquare, squareRotate);
        }
    }
    return logicalMask;
}

FrameHashes ComputeComposedFrameHashes(
    const runtime::GameStateMachine& stateMachine,
    const modules::EngineDerivedState* derived,
    const modules::GamePersistenceCoordinator* gamePersistence,
    const bool boardRotate,
    const bool firstPlayerIsP2,
    const bool squareRotateForOutput) {
    runtime::InteractionModelSnapshotBuildInputs snapshotInputs;
    snapshotInputs.stateMachine = &stateMachine;
    snapshotInputs.derived = derived;
    snapshotInputs.gameClock =
        gamePersistence ? &gamePersistence->GetGameClock() : nullptr;
    if (gamePersistence != nullptr) {
        snapshotInputs.authoritativeFen = gamePersistence->GetPresentationFen();
        snapshotInputs.clockWinner = gamePersistence->GetClockTerminalWinner();
        snapshotInputs.clockTerminalStartMs =
            gamePersistence->GetClockTerminalStartMs();
        snapshotInputs.variant = gamePersistence->GetGameVariant();
        snapshotInputs.duckSquare = gamePersistence->GetDuckSquare();
        snapshotInputs.initialDuckCandidateSquare =
            gamePersistence->GetInitialDuckCandidateSquare();
        snapshotInputs.initialVariantValidationActive =
            gamePersistence->IsInitialVariantValidationPending();
        snapshotInputs.setupLifecycle = runtime::ClassifySetupLifecycle(
            gamePersistence->GetGameVariant(),
            gamePersistence->GetDuckTurnPhase(),
            stateMachine.GetMovedSquaresMask());
        snapshotInputs.duckTurnPhase = gamePersistence->GetDuckTurnPhase();
        snapshotInputs.variantOutcome = gamePersistence->GetVariantOutcome();
    }
    const runtime::InteractionModelSnapshot snapshot =
        runtime::BuildInteractionModelSnapshot(snapshotInputs);

    modules::led_frame_composer::ComposeInputs composeIn;
    composeIn.settings = gamePersistence
                             ? &gamePersistence->GetSettings()
                             : nullptr;
    composeIn.interactionSnapshot = &snapshot;
    composeIn.firstPlayerIsP2 = firstPlayerIsP2;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame = {};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame = {};
    std::array<modules::Rgb, 64> gridFrame = {};
    modules::led_frame_composer::Compose(
        composeIn, ledFrame, edgeFrame, gridFrame);

    std::array<modules::Rgb, hardware::BOARD_LED_COUNT> boardOnly = {};
    std::copy_n(ledFrame.begin(), hardware::BOARD_LED_COUNT,
                boardOnly.begin());
    FrameHashes out;
    out.board = fxe::HashRgbFrame(boardOnly.data(), boardOnly.size());
    out.edge = fxe::HashRgbFrame(edgeFrame.data(), edgeFrame.size());
    out.all = fxe::HashCombineU64(out.board, out.edge);
    return out;
}

} // namespace trace::replay_internal
