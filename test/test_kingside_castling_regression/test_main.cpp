#include "bounded_hint_engine.hpp"
#include <array>
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "hardware/chessboard_hardware.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/chess_strings.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/orientation.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "runtime/square_mapping.hpp"
#include "../support/led_frame_composer_snapshot_adapter.hpp"

namespace {

constexpr char USER_PRE_CASTLE_FEN[] =
    "r1bqk1nr/pp1n2bp/2p1p3/3pPpp1/2B2B2/2NP1N2/PPPQ1PPP/"
    "R3K2R w KQkq - 0 1";

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

uint64_t OccupancyForFen(const char* fen) {
    uint64_t occupied = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        char piece = '\0';
        if (modules::TryGetFenPieceAtSquare(fen, square, piece)) {
            occupied |= 1ULL << square;
        }
    }
    return occupied;
}

modules::Rgb PieceLed(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& frame,
    const uint8_t square) {
    return frame[hardware::LedIndexForSquare(
        square, hardware::SquareLedSlot::Piece)];
}

modules::Rgb SquareLed(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& frame,
    const uint8_t square) {
    return frame[hardware::LedIndexForSquare(
        square, hardware::SquareLedSlot::Square0)];
}

bool IsLit(const modules::Rgb color) {
    return color.r != 0U || color.g != 0U || color.b != 0U;
}

uint64_t MapMask(const uint64_t mask, const bool rotate) {
    uint64_t mapped = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((mask & (1ULL << square)) != 0ULL) {
            mapped |= 1ULL << runtime::MapSquareIndex(square, rotate);
        }
    }
    return mapped;
}

class AtomicBoardInput final : public modules::IBoardInput {
  public:
    explicit AtomicBoardInput(const uint64_t initialOccupancy)
        : m_occupancy(initialOccupancy) {}

    bool TryReadSnapshot(uint64_t& outOccupancy) override {
        outOccupancy = m_occupancy;
        return true;
    }

    bool TryReadDelta(modules::SquareDelta&) override { return false; }

    bool TryReadObservation(modules::BoardObservation& outObservation) override {
        if (!m_pending) {
            return false;
        }
        outObservation = m_observation;
        m_occupancy = m_observation.occupiedMask;
        m_pending = false;
        return true;
    }

    bool SupportsAtomicObservations() const override { return true; }
    void Resync() override {}

    void Push(const uint64_t changedMask, const uint64_t occupiedMask) {
        m_observation = {changedMask, occupiedMask};
        m_pending = true;
    }

  private:
    uint64_t m_occupancy = 0ULL;
    modules::BoardObservation m_observation{};
    bool m_pending = false;
};

} // namespace

int main() {
    fxe::ScopedMillisProviderOverride millisGuard(
        &fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);

    static constexpr char SETTINGS_PATH[] =
        "build/test_kingside_castling_regression.json";
    (void)std::remove(SETTINGS_PATH);
    (void)std::remove(
        "build/test_kingside_castling_regression.json.bak");
    (void)std::remove(
        "build/test_kingside_castling_regression.json.tmp");

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = SETTINGS_PATH;
    settingsConfig.deferredSaveDelayMs = 100000U;
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "settings load");
    settings.Doc()["VS_CPU_LEVEL"] = 0U;
    settings.Doc()["P1_PIECE_WHEEL"] = 0U;
    settings.Doc()["P2_PIECE_WHEEL"] = 128U;

    runtime::GameStateMachineConfig stateMachineConfig;
    stateMachineConfig.settleDurationMs = 10U;
    stateMachineConfig.cpuIdleDelayMs = 999999U;
    stateMachineConfig.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(stateMachineConfig);
    stateMachine.Reset();

    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "coordinator load");

    uint64_t physical = OccupancyForFen(USER_PRE_CASTLE_FEN);
    stateMachine.DebugSetPhysicalOccupancyMask(physical);
    Check(coordinator.LoadExternalFen(USER_PRE_CASTLE_FEN),
          "load user's pre-castle FEN");

    physical &= ~(1ULL << 60U);
    stateMachine.HandleBoardObservation(
        1ULL << 60U, physical, 1000U, 1U);

    // Kingside is a short reach. Reproduce the fastest physical path by
    // placing the king before the coordinator gets another update tick to
    // install engine-derived destination guidance.
    physical |= 1ULL << 62U;
    stateMachine.HandleBoardObservation(
        1ULL << 62U, physical, 1001U, 2U);

    const runtime::TaggedInteractionState& phase =
        stateMachine.GetChessInteractionModel().phase;
    Check(phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
              phase.plan.kind ==
                  runtime::PhysicalTransitionKind::Castling,
          "king placement retains the castling plan");
    Check(phase.requiredAction ==
              runtime::RequiredPhysicalAction::LiftSecondary,
          "king placement advances to rook pickup");
    Check(!stateMachine.HasHumanIllegal(),
          "kingside king placement is not marked illegal");

    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);
    runtime::InteractionModelSnapshot snapshot =
        test_support::BuildInteractionSnapshot(
            stateMachine, derived, &engine);
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &settings;
    inputs.interactionSnapshot = &snapshot;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};
    modules::led_frame_composer::Compose(
        inputs, ledFrame, edgeFrame, gridFrame);

    Check(IsLit(PieceLed(ledFrame, 62U)),
          "king piece LED remains lit on g1");
    Check(IsLit(SquareLed(ledFrame, 63U)),
          "rook source h1 prompts for pickup");
    Check(IsLit(SquareLed(ledFrame, 61U)),
          "rook destination f1 prompts for placement");

    // Field regression: P1 occupies the opposite physical end, so sensor
    // squares are rotated. Two
    // adjacent scans observe rook h1 first and king e1 second. Production
    // ingress must switch from ordinary rook guidance to castling immediately.
    {
        static constexpr char ROTATED_SETTINGS_PATH[] =
            "build/test_kingside_castling_regression_p2.json";
        (void)std::remove(ROTATED_SETTINGS_PATH);
        (void)std::remove(
            "build/test_kingside_castling_regression_p2.json.bak");
        (void)std::remove(
            "build/test_kingside_castling_regression_p2.json.tmp");

        modules::SettingsStoreConfig rotatedSettingsConfig;
        rotatedSettingsConfig.path = ROTATED_SETTINGS_PATH;
        rotatedSettingsConfig.deferredSaveDelayMs = 100000U;
        modules::SettingsStore rotatedSettings(rotatedSettingsConfig);
        Check(rotatedSettings.Load(), "rotated P2 settings load");
        rotatedSettings.Doc()["VS_CPU_LEVEL"] = 0U;
        rotatedSettings.Doc()["BOARD_ROTATE"] = true;
        rotatedSettings.Doc()["GAME_FIRST_PLAYER"] = 2U;
        rotatedSettings.Doc()["P1_PIECE_WHEEL"] = 0U;
        rotatedSettings.Doc()["P2_PIECE_WHEEL"] = 128U;

        runtime::GameStateMachine rotatedStateMachine(stateMachineConfig);
        rotatedStateMachine.Reset();
        test_support::BoundedHintEngine rotatedEngine;
        rotatedEngine.Reset();
        modules::GamePersistenceCoordinator rotatedCoordinator(
            rotatedSettings, rotatedEngine, rotatedStateMachine);
        Check(rotatedCoordinator.LoadOrInitialize(),
              "rotated P2 coordinator load");

        static constexpr char ROTATED_SPARSE_CASTLE_FEN[] =
            "4k3/8/8/8/8/8/8/4K2R w K - 0 1";
        const bool squareRotate = modules::ComputeSquareRotate(true, true);
        Check(!squareRotate, "physical rotation and P1 far-end placement cancel in raw mapping");
        Check(runtime::MapSquareIndex(4U, squareRotate) == 4U,
              "rotated opposite-end P1 keeps raw e8");
        Check(runtime::MapSquareIndex(60U, squareRotate) == 60U,
              "rotated opposite-end P1 keeps raw e1");
        Check(runtime::MapSquareIndex(63U, squareRotate) == 63U,
              "rotated opposite-end P1 keeps raw h1");
        Check(runtime::MapSquareIndex(62U, squareRotate) == 62U,
              "rotated opposite-end P1 keeps raw g1");
        Check(runtime::MapSquareIndex(61U, squareRotate) == 61U,
              "rotated opposite-end P1 keeps raw f1");

        const uint64_t logicalInitial =
            OccupancyForFen(ROTATED_SPARSE_CASTLE_FEN);
        const uint64_t rawInitial = MapMask(logicalInitial, squareRotate);
        AtomicBoardInput boardInput(rawInitial);
        hardware::ChessboardHardware board(boardInput);
        board.SetFlipSides(squareRotate);
        board.Resync();
        rotatedStateMachine.DebugSetPhysicalOccupancyMask(logicalInitial);
        Check(rotatedCoordinator.LoadExternalFen(ROTATED_SPARSE_CASTLE_FEN),
              "rotated P2 pre-castle FEN");

        runtime::AppEventQueue eventQueue;
        runtime::InteractionDispatcher dispatcher(
            rotatedStateMachine, rotatedCoordinator, eventQueue);

        const uint8_t rawRook = runtime::MapSquareIndex(63U, squareRotate);
        const uint8_t rawKing = runtime::MapSquareIndex(60U, squareRotate);
        uint64_t rawOccupied = rawInitial & ~(1ULL << rawRook);
        boardInput.Push(1ULL << rawRook, rawOccupied);
        board.PollAndDispatch(dispatcher);
        rotatedCoordinator.UpdateLegalDestinationsMask();
        Check(rotatedStateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::PieceLifted,
              "rotated P2 rook scan begins ordinary rook intent");
        Check(rotatedStateMachine.GetChessInteractionModel().phase.selectedSquare ==
                  63U,
              "rotated raw rook maps to logical h1");
        Check((rotatedStateMachine.GetValidDestMask() & (1ULL << 61U)) != 0ULL,
              "rotated P2 rook scan shows ordinary f1 rook move");

        rawOccupied &= ~(1ULL << rawKing);
        boardInput.Push(1ULL << rawKing, rawOccupied);
        board.PollAndDispatch(dispatcher);
        rotatedCoordinator.UpdateLegalDestinationsMask();
        const runtime::TaggedInteractionState& rotatedPhase =
            rotatedStateMachine.GetChessInteractionModel().phase;
        Check(rotatedPhase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
                  rotatedPhase.plan.kind ==
                      runtime::PhysicalTransitionKind::Castling,
              "rotated P2 split rook-king scans enter castling");
        Check(rotatedPhase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary,
              "rotated P2 castle next prompts king destination");
        Check(rotatedStateMachine.GetValidDestMask() == (1ULL << 62U),
              "castling replaces ordinary rook mask with logical g1");
        Check(!rotatedStateMachine.HasHumanIllegal() &&
                  !rotatedStateMachine.HasCaptureReturn(),
              "rotated P2 castle stays out of correction feedback");

        // Exact field regression: after returning the two pieces home, lift
        // the P1-owned White king and rook at the P2-labeled end in one raw
        // hardware observation.
        // The physical-square mapper and reducer must agree that this is the
        // same king-side castle, regardless of changed-bit iteration order.
        const uint64_t rawCastleSources =
            (1ULL << rawKing) | (1ULL << rawRook);
        rawOccupied = rawInitial;
        boardInput.Push(rawCastleSources, rawOccupied);
        board.PollAndDispatch(dispatcher);
        rotatedCoordinator.UpdateLegalDestinationsMask();
        Check(rotatedStateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle,
              "rotated P2 returning both castle sources aborts cleanly");

        rawOccupied = rawInitial & ~rawCastleSources;
        boardInput.Push(rawCastleSources, rawOccupied);
        board.PollAndDispatch(dispatcher);
        rotatedCoordinator.UpdateLegalDestinationsMask();
        const runtime::TaggedInteractionState& coalescedRotatedPhase =
            rotatedStateMachine.GetChessInteractionModel().phase;
        Check(coalescedRotatedPhase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan &&
                  coalescedRotatedPhase.plan.kind ==
                      runtime::PhysicalTransitionKind::Castling,
              "rotated P2 same-scan king-rook pickup enters castling");
        Check(coalescedRotatedPhase.plan.relocations[0].from == 60U &&
                  coalescedRotatedPhase.plan.relocations[1].from == 63U,
              "rotated P2 same-scan pickup keeps king and rook identities");
        Check(rotatedStateMachine.GetValidDestMask() == (1ULL << 62U),
              "rotated P2 same-scan castle prompts logical g1");
        Check(!rotatedStateMachine.HasHumanIllegal() &&
                  !rotatedStateMachine.HasCaptureReturn() &&
                  !rotatedStateMachine.GetInteractionInvariantStatus().IsActive(),
              "rotated P2 same-scan castle remains valid and consistent");

        modules::EngineDerivedState rotatedDerived;
        rotatedDerived.RefreshFromEngine(rotatedEngine);
        runtime::InteractionModelSnapshot rotatedSnapshot =
            test_support::BuildInteractionSnapshot(
                rotatedStateMachine, rotatedDerived, &rotatedEngine);
        modules::led_frame_composer::ComposeInputs rotatedInputs;
        rotatedInputs.settings = &rotatedSettings;
        rotatedInputs.interactionSnapshot = &rotatedSnapshot;
        rotatedInputs.firstPlayerIsP2 = true;
        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> rotatedLeds{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> rotatedEdges{};
        std::array<modules::Rgb, 64> rotatedGrid{};
        modules::led_frame_composer::Compose(
            rotatedInputs, rotatedLeds, rotatedEdges, rotatedGrid);
        const auto rotatedPhysical = hardware::MapSemanticLedFrame(
            {hardware::BoardRevision::V2S3, squareRotate, squareRotate},
            rotatedLeds, rotatedEdges);
        const uint8_t physicalKingTarget =
            runtime::MapSquareIndex(62U, squareRotate);
        Check(IsLit(rotatedPhysical[hardware::LedIndexForSquare(
                  physicalKingTarget, hardware::SquareLedSlot::Square0)]),
              "rotated P2 render immediately lights physical king target");

        // Place the king first. The reducer must retain P1-owned White
        // turn and guide the remaining rook to physical c8.
        rawOccupied |= 1ULL << physicalKingTarget;
        boardInput.Push(1ULL << physicalKingTarget, rawOccupied);
        board.PollAndDispatch(dispatcher);
        rotatedCoordinator.UpdateLegalDestinationsMask();
        const runtime::TaggedInteractionState& kingFirstPhase =
            rotatedStateMachine.GetChessInteractionModel().phase;
        Check(kingFirstPhase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
                  kingFirstPhase.requiredAction ==
                      runtime::RequiredPhysicalAction::PlaceSecondary,
              "rotated P2 king-first placement retains rook guidance");
        Check(!rotatedStateMachine.HasHumanIllegal() &&
                  !rotatedStateMachine.HasCaptureReturn() &&
                  rotatedEngine.GetSideToMove() == modules::SideToMove::White &&
                  modules::led_frame_composer::OwnerForSideToMove(
                      rotatedEngine.GetSideToMove(), true) == modules::UiOwner::P1,
              "opposite-end P1 king-first placement stays White/P1 without fault");

        rotatedDerived.RefreshFromEngine(rotatedEngine);
        rotatedSnapshot = test_support::BuildInteractionSnapshot(
            rotatedStateMachine, rotatedDerived, &rotatedEngine);
        rotatedInputs.interactionSnapshot = &rotatedSnapshot;
        rotatedLeds.fill({});
        rotatedEdges.fill({});
        rotatedGrid.fill({});
        modules::led_frame_composer::Compose(
            rotatedInputs, rotatedLeds, rotatedEdges, rotatedGrid);
        Check(IsLit(SquareLed(
                  rotatedLeds, runtime::MapSquareIndex(61U, squareRotate))),
              "rotated P2 king-first render guides physical c8 rook");

        // Complete the ordinary settle/coordinator path with the rook.
        const uint8_t physicalRookTarget =
            runtime::MapSquareIndex(61U, squareRotate);
        rawOccupied |= 1ULL << physicalRookTarget;
        boardInput.Push(1ULL << physicalRookTarget, rawOccupied);
        board.PollAndDispatch(dispatcher);
        rotatedCoordinator.UpdateLegalDestinationsMask();
        Check(rotatedStateMachine.GetChessInteractionModel().phase.tag ==
                      runtime::ChessInteractionPhase::AwaitingAction &&
                  rotatedStateMachine.ShouldProcessCommittedMoveImmediately() &&
                  rotatedStateMachine.IsMoveSettlingActive(),
              "rotated P2 completed castle enters immediate production acknowledgement path");
        rotatedCoordinator.Update();
        char rotatedPostCastleFen[128] = {};
        Check(rotatedEngine.GetFen(rotatedPostCastleFen,
                                   sizeof(rotatedPostCastleFen)) &&
                  std::strcmp(rotatedPostCastleFen,
                              "4k3/8/8/8/8/8/8/5RK1 b - - 0 1") == 0,
              "rotated P2 castle commits the exact post-castle FEN");
        Check(rotatedCoordinator.GetUndoHistoryMoveCount() == 1U,
              "rotated P2 castle commits exactly one history entry");
        Check(rotatedStateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle &&
                  rotatedEngine.GetSideToMove() == modules::SideToMove::Black &&
                  modules::led_frame_composer::OwnerForSideToMove(
                      rotatedEngine.GetSideToMove(), true) == modules::UiOwner::P2,
              "opposite-end P1 castle completes Idle with Black on P2");
        Check(rotatedStateMachine.IsMoveSettlingActive(),
              "rotated P2 acknowledgement preserves the visual settle");
        fxe::ManualMillis::AdvanceMs(stateMachineConfig.settleDurationMs);
        dispatcher.DispatchTick(fxe::Millis());
        Check(!rotatedStateMachine.IsMoveSettlingActive() &&
                  rotatedStateMachine.GetChessInteractionModel().phase.tag ==
                      runtime::ChessInteractionPhase::Idle,
              "rotated P2 settle expires without disturbing acknowledged Idle");
    }

    if (g_failures == 0) {
        return 0;
    }
    std::printf("[KING_CASTLE_REGRESSION] failures=%d\n", g_failures);
    return 1;
}
