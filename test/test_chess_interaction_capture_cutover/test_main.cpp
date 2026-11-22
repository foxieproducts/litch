#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

uint64_t FenOccupancy(const char* fen) {
    uint64_t occupied = 0;
    uint8_t rank = 0;
    uint8_t file = 0;
    for (const char* p = fen; p != nullptr && *p != '\0' && *p != ' '; ++p) {
        if (*p == '/') {
            ++rank;
            file = 0;
        } else if (*p >= '1' && *p <= '8') {
            file = static_cast<uint8_t>(file + static_cast<uint8_t>(*p - '0'));
        } else {
            occupied |= 1ULL << static_cast<uint8_t>(rank * 8U + file);
            ++file;
        }
    }
    return occupied;
}

uint64_t EngineOccupancy(modules::IEngine& engine) {
    char fen[128] = {};
    Check(engine.GetFen(fen, sizeof(fen)), "read engine FEN");
    return FenOccupancy(fen);
}

void Observe(runtime::GameStateMachine& stateMachine,
             const uint32_t sequence,
             const uint64_t changed,
             const uint64_t occupied) {
    stateMachine.HandleBoardObservation(changed,
                                        occupied,
                                        fxe::Millis(),
                                        sequence);
}

void AdvanceSettle(runtime::ManualClock& clock,
                   runtime::GameStateMachine& stateMachine,
                   modules::GamePersistenceCoordinator& coordinator) {
    clock.AdvanceMs(20U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    stateMachine.Tick();
    coordinator.Update();
}

void TestNormalCapture(const bool targetFirst) {
    constexpr uint8_t FROM = 36U;  // e4
    constexpr uint8_t TO = 27U;    // d5
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t TO_MASK = 1ULL << TO;
    const char* fen = "7k/8/8/3p4/4P3/8/8/7K w - - 0 1";

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = targetFirst
                              ? "build/test_capture_cutover_target_first.json"
                              : "build/test_capture_cutover_mover_first.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "capture settings load");

    runtime::ManualClock clock(1000U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(config);
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);

    stateMachine.DebugSetPhysicalOccupancyMask(FenOccupancy(fen));
    Check(coordinator.LoadExternalFen(fen), "load capture FEN");
    uint64_t physical = FenOccupancy(fen);
    uint32_t sequence = 1U;
    Observe(stateMachine, sequence++, 0, physical);

    if (targetFirst) {
        physical &= ~TO_MASK;
        Observe(stateMachine, sequence++, TO_MASK, physical);
        Check(stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::CaptureStaged,
              "production target-first removal is explicitly staged");
        physical &= ~FROM_MASK;
        Observe(stateMachine, sequence++, FROM_MASK, physical);
    } else {
        physical &= ~FROM_MASK;
        Observe(stateMachine, sequence++, FROM_MASK, physical);
        physical &= ~TO_MASK;
        Observe(stateMachine, sequence++, TO_MASK, physical);
    }

    physical |= TO_MASK;
    Observe(stateMachine, sequence++, TO_MASK, physical);
    Check(stateMachine.GetChessInteractionModel().phase.plan.kind ==
              runtime::PhysicalTransitionKind::Capture,
          "production normal capture uses reducer plan");
    AdvanceSettle(clock, stateMachine, coordinator);

    const uint64_t expected = (FenOccupancy(fen) & ~FROM_MASK) | TO_MASK;
    Check(EngineOccupancy(engine) == expected,
          targetFirst ? "target-first capture applies once"
                      : "mover-first capture applies once");
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "capture acknowledgement adds one undo move");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle,
          "capture acknowledgement completes reducer plan");
}

void TestMoverFirstEnPassant() {
    constexpr uint8_t FROM = 28U;       // e5
    constexpr uint8_t TARGET = 19U;     // d6
    constexpr uint8_t CAPTURE = 27U;    // d5
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t TARGET_MASK = 1ULL << TARGET;
    constexpr uint64_t CAPTURE_MASK = 1ULL << CAPTURE;
    const char* fen = "7k/8/8/3pP3/8/8/8/7K w - d6 0 1";

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_capture_cutover_en_passant.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "en-passant settings load");

    runtime::ManualClock clock(2000U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(config);
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);
    stateMachine.DebugSetPhysicalOccupancyMask(FenOccupancy(fen));
    Check(coordinator.LoadExternalFen(fen), "load en-passant FEN");
    const runtime::ChessPositionState& acknowledged =
        stateMachine.GetChessInteractionModel().acknowledged;
    Check(acknowledged.enPassantTargetSquare == TARGET,
          "acknowledged position includes en-passant target");
    Check(acknowledged.enPassantCaptureSquare == CAPTURE,
          "acknowledged position includes en-passant capture square");

    const uint64_t before = FenOccupancy(fen);
    Observe(stateMachine, 1U, 0, before);
    const uint64_t placedBeforeRemoval =
        (before & ~FROM_MASK) | TARGET_MASK;
    Observe(stateMachine,
            2U,
            FROM_MASK | TARGET_MASK,
            placedBeforeRemoval);
    Check(stateMachine.GetChessInteractionModel().phase.plan.kind ==
              runtime::PhysicalTransitionKind::EnPassant,
          "production derives en-passant plan from acknowledged metadata");
    Check(stateMachine.IsMoveSettlingActive(),
          "en-passant mover placement enters compatibility settle");

    AdvanceSettle(clock, stateMachine, coordinator);
    Check(EngineOccupancy(engine) == before,
          "settle expiry cannot commit while captured pawn remains");
    Check(!stateMachine.HasCommittedMovePending(),
          "blocked en-passant has no committed engine action");

    const uint64_t after = placedBeforeRemoval & ~CAPTURE_MASK;
    Observe(stateMachine, 3U, CAPTURE_MASK, after);
    coordinator.Update();
    Check(EngineOccupancy(engine) == after,
          "en-passant applies after off-destination pawn removal");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle,
          "en-passant acknowledgement completes reducer plan");
    Check((stateMachine.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle),
          "en-passant capture removal is consumed by the active plan");
}

void TestIncompleteEnPassantUnrelatedPickupRequiresCorrection() {
    constexpr uint8_t FROM = 28U;       // e5
    constexpr uint8_t TARGET = 19U;     // d6
    constexpr uint8_t CAPTURE = 27U;    // d5
    constexpr uint8_t UNRELATED = 7U;   // h8
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t TARGET_MASK = 1ULL << TARGET;
    constexpr uint64_t CAPTURE_MASK = 1ULL << CAPTURE;
    constexpr uint64_t UNRELATED_MASK = 1ULL << UNRELATED;
    const char* fen = "7k/8/8/3pP3/8/8/8/7K w - d6 0 1";

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path =
        "build/test_capture_cutover_en_passant_interruption.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "interrupted en-passant settings load");

    runtime::ManualClock clock(2500U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(config);
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(settings, engine,
                                                    stateMachine);

    const uint64_t before = FenOccupancy(fen);
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    Check(coordinator.LoadExternalFen(fen),
          "load interrupted en-passant FEN");
    Observe(stateMachine, 1U, 0ULL, before);

    const uint64_t incomplete = (before & ~FROM_MASK) | TARGET_MASK;
    Observe(stateMachine, 2U, FROM_MASK | TARGET_MASK, incomplete);
    Check(stateMachine.IsMoveSettlingActive(),
          "incomplete en-passant enters compatibility settle");
    Check(stateMachine.GetChessInteractionModel().phase.requiredAction ==
              runtime::RequiredPhysicalAction::RemoveCapture,
          "incomplete en-passant still requires captured pawn removal");

    const uint64_t interrupted = incomplete & ~UNRELATED_MASK;
    Observe(stateMachine, 3U, UNRELATED_MASK, interrupted);
    coordinator.Update();

    const uint64_t expectedCorrection = before ^ interrupted;
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "unrelated pickup aborts incomplete en-passant into correction");
    Check(stateMachine.GetChessInteractionModel().phase.correctionMask ==
              expectedCorrection,
          "interrupted en-passant correction covers the full observed mismatch");
    Check(stateMachine.GetHumanIllegalMask() == expectedCorrection,
          "feedback exposes the canonical correction mask");
    Check(!stateMachine.IsHumanPieceInHand(),
          "interrupted en-passant correction has no piece in hand");
    Check(EngineOccupancy(engine) == before,
          "interrupted en-passant never commits the logical move");
    Check(!stateMachine.HasCommittedMovePending(),
          "interrupted en-passant exposes no engine action");

    stateMachine.Reset();
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    Check(coordinator.LoadExternalFen(fen),
          "reload en-passant FEN for destination re-lift");
    Observe(stateMachine, 4U, 0ULL, before);
    Observe(stateMachine, 5U, FROM_MASK | TARGET_MASK, incomplete);
    Observe(stateMachine, 6U, TARGET_MASK, incomplete & ~TARGET_MASK);

    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::PieceLifted,
          "destination-only re-lift cancels incomplete en-passant");
    Check(stateMachine.GetChessInteractionModel().phase.selectedSquare == FROM,
          "en-passant re-lift restores the original mover source");
    Check(stateMachine.IsHumanPieceInHand(),
          "piece-in-hand query agrees with canceled en-passant placement");
    Check(!stateMachine.HasHumanIllegal(),
          "destination-only en-passant re-lift is not correction");
    Check(EngineOccupancy(engine) == before,
          "canceled en-passant remains logically uncommitted");

    stateMachine.Reset();
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    Check(coordinator.LoadExternalFen(fen),
          "reload en-passant FEN for unrelated placement");
    Observe(stateMachine, 7U, 0ULL, before);
    Observe(stateMachine, 8U, FROM_MASK | TARGET_MASK, incomplete);
    constexpr uint64_t EXTRA_MASK = 1ULL << 0U; // a8 is empty
    const uint64_t extraPlaced = incomplete | EXTRA_MASK;
    Observe(stateMachine, 9U, EXTRA_MASK, extraPlaced);

    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "unrelated placement aborts incomplete en-passant into correction");
    Check(stateMachine.GetHumanIllegalMask() == (before ^ extraPlaced),
          "unrelated placement projects the full canonical correction");
    Check(EngineOccupancy(engine) == before,
          "unrelated placement cannot commit incomplete en-passant");

    stateMachine.Reset();
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    Check(coordinator.LoadExternalFen(fen),
          "reload en-passant FEN for ambiguous capture removal");
    Observe(stateMachine, 10U, 0ULL, before);
    Observe(stateMachine, 11U, FROM_MASK | TARGET_MASK, incomplete);
    const uint64_t captureAndOtherRemoved =
        incomplete & ~CAPTURE_MASK & ~UNRELATED_MASK;
    Observe(stateMachine,
            12U,
            CAPTURE_MASK | UNRELATED_MASK,
            captureAndOtherRemoved);

    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "capture removal plus unrelated pickup aborts en-passant");
    Check(stateMachine.GetHumanIllegalMask() ==
              (before ^ captureAndOtherRemoved),
          "ambiguous capture removal corrects the complete mismatch");
    Check(!stateMachine.HasCommittedMovePending(),
          "ambiguous capture removal exposes no en-passant action");
    Check(EngineOccupancy(engine) == before,
          "ambiguous capture removal remains logically uncommitted");
}

void TestPromotionCutover() {
    constexpr uint8_t FROM = 8U;
    constexpr uint8_t TO = 0U;
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t TO_MASK = 1ULL << TO;
    const char* fen = "7k/P7/8/8/8/8/8/7K w - - 0 1";

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_capture_cutover_promotion.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "promotion cutover settings load");
    settings.Doc()["PROMOTION_MODE"] = "choose";

    runtime::ManualClock clock(3000U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(config);
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);
    const uint64_t before = FenOccupancy(fen);
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    Check(coordinator.LoadExternalFen(fen), "load promotion cutover FEN");

    Observe(stateMachine, 1U, 0, before);
    const uint64_t after = (before & ~FROM_MASK) | TO_MASK;
    Observe(stateMachine, 2U, FROM_MASK | TO_MASK, after);
    Check(stateMachine.GetChessInteractionModel().phase.plan.kind ==
              runtime::PhysicalTransitionKind::Promotion,
          "production pawn-to-last-rank uses promotion plan");
    AdvanceSettle(clock, stateMachine, coordinator);
    Check(stateMachine.IsPromotionActive(),
          "promotion plan opens adapter choice before engine mutation");
    Check(EngineOccupancy(engine) == before,
          "promotion engine position remains pre-move until choice");

    stateMachine.HandleButtonEvent(modules::UserButton::P1O,
                                   fxe::Button::Event::Press);
    coordinator.Update();
    char promotedFen[128] = {};
    Check(engine.GetFen(promotedFen, sizeof(promotedFen)),
          "read promoted FEN");
    Check(promotedFen[0] == 'Q',
          "default promotion choice applies as queen");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle,
          "promotion engine acknowledgement completes reducer plan");
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "promotion success appends one acknowledged undo move");
}

} // namespace

int main() {
    std::puts("[CHESS_INTERACTION_CAPTURE_CUTOVER] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    TestNormalCapture(false);
    TestNormalCapture(true);
    TestMoverFirstEnPassant();
    TestIncompleteEnPassantUnrelatedPickupRequiresCorrection();
    TestPromotionCutover();
    if (g_failures == 0) {
        std::puts("[CHESS_INTERACTION_CAPTURE_CUTOVER] ok");
        return 0;
    }
    std::printf("[CHESS_INTERACTION_CAPTURE_CUTOVER] failures=%d\n", g_failures);
    return 1;
}
