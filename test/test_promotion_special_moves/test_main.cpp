#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/chess_interaction_persistence.hpp"
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
    uint64_t occupied = 0ULL;
    uint8_t rank = 0U;
    uint8_t file = 0U;
    for (const char* cursor = fen; cursor != nullptr && *cursor != '\0'; ++cursor) {
        if (*cursor == ' ') {
            break;
        }
        if (*cursor == '/') {
            ++rank;
            file = 0U;
        } else if (*cursor >= '1' && *cursor <= '8') {
            file = static_cast<uint8_t>(file + *cursor - '0');
        } else {
            occupied |= 1ULL << static_cast<uint8_t>(rank * 8U + file);
            ++file;
        }
    }
    return occupied;
}

bool SameFenBoard(const char* lhs, const char* rhs) {
    const char* lhsEnd = std::strchr(lhs, ' ');
    const char* rhsEnd = std::strchr(rhs, ' ');
    if (lhsEnd == nullptr || rhsEnd == nullptr) {
        return false;
    }
    const size_t lhsLength = static_cast<size_t>(lhsEnd - lhs);
    return lhsLength == static_cast<size_t>(rhsEnd - rhs) &&
           std::strncmp(lhs, rhs, lhsLength) == 0;
}

void Observe(runtime::GameStateMachine& stateMachine,
             const uint64_t changed,
             const uint64_t occupied,
             const uint32_t timestamp,
             const uint32_t sequence) {
    stateMachine.HandleBoardObservation(
        changed, occupied, timestamp, sequence);
}

void FinishSettle(runtime::GameStateMachine& stateMachine,
                  modules::GamePersistenceCoordinator& coordinator,
                  const uint32_t nowMs) {
    fxe::ManualMillis::SetNowMs(nowMs);
    stateMachine.Tick();
    coordinator.Update();
}

void TestCapturePromotionRetainsUndoMetadata() {
    constexpr const char* FEN =
        "r6k/1P6/8/8/8/8/8/7K w - - 0 1";
    constexpr uint8_t FROM = 9U;
    constexpr uint8_t TO = 0U;

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_capture_promotion_metadata.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    (void)std::remove("build/test_capture_promotion_metadata.json.bak");
    (void)std::remove("build/test_capture_promotion_metadata.json.tmp");
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "capture promotion settings load");
    settings.Doc()["PROMOTION_MODE"] = "choose";
    settings.Doc()[modules::KEY_GAME_FEN] = FEN;

    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 10U;
    stateConfig.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "capture promotion initializes");

    const uint64_t before = FenOccupancy(FEN);
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    stateMachine.SetValidDestMask(1ULL << TO);
    const uint64_t pawnLifted = before & ~(1ULL << FROM);
    const uint64_t targetLifted = pawnLifted & ~(1ULL << TO);
    const uint64_t placed = targetLifted | (1ULL << TO);
    Observe(stateMachine, 1ULL << FROM, pawnLifted, 1000U, 1U);
    Observe(stateMachine, 1ULL << TO, targetLifted, 1001U, 2U);
    Observe(stateMachine, 1ULL << TO, placed, 1002U, 3U);

    const runtime::PhysicalTransitionPlan& plan =
        stateMachine.GetChessInteractionModel().phase.plan;
    Check(plan.kind == runtime::PhysicalTransitionKind::Promotion,
          "capture promotion has promotion plan identity");
    Check(plan.promotionRequired && plan.removalRequired &&
              plan.removalSquare == TO,
          "capture promotion retains promotion and capture metadata");
    Check(runtime::CanSafelyPersistChessInteractionState(
              stateMachine.GetChessInteractionModel()),
          "pending capture promotion is safely persistable");

    FinishSettle(stateMachine, coordinator, 1020U);
    Check(stateMachine.IsPromotionActive(),
          "capture promotion opens promotion choice");
    stateMachine.HandleButtonEvent(
        modules::UserButton::P1O, fxe::Button::Event::Press);
    coordinator.Update();

    char promotedFen[128] = {};
    Check(engine.GetFen(promotedFen, sizeof(promotedFen)),
          "capture promotion reads promoted FEN");
    Check(std::strncmp(promotedFen, "Q6k/", 4U) == 0,
          "capture promotion applies selected piece");

    Check(coordinator.RequestUndo(1U),
          "capture promotion starts undo");
    coordinator.Update();
    uint8_t restoreSquare = 255U;
    char restorePiece = '\0';
    Check(stateMachine.TryGetUndoRestoreInfo(
              restoreSquare, restorePiece),
          "capture promotion undo requests captured piece restoration");
    Check(restoreSquare == TO && restorePiece == 'r',
          "capture promotion undo restores the rook on the capture square");
}

void TestCancelPromotionDoesNotUndoPriorMove() {
    constexpr const char* FEN =
        "r6k/1P6/8/8/8/8/8/7K b - - 0 1";
    constexpr uint8_t KING_FROM = 7U;
    constexpr uint8_t KING_TO = 6U;
    constexpr uint8_t PAWN_FROM = 9U;
    constexpr uint8_t PROMOTION_TO = 0U;

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_cancel_capture_promotion.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    (void)std::remove("build/test_cancel_capture_promotion.json.bak");
    (void)std::remove("build/test_cancel_capture_promotion.json.tmp");
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "cancel promotion settings load");
    settings.Doc()["PROMOTION_MODE"] = "choose";
    settings.Doc()[modules::KEY_GAME_FEN] = FEN;

    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 10U;
    stateConfig.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "cancel promotion initializes");

    const uint64_t initial = FenOccupancy(FEN);
    stateMachine.DebugSetPhysicalOccupancyMask(initial);
    stateMachine.SetValidDestMask(1ULL << KING_TO);
    const uint64_t kingLifted = initial & ~(1ULL << KING_FROM);
    const uint64_t afterPriorMove = kingLifted | (1ULL << KING_TO);
    Observe(stateMachine, 1ULL << KING_FROM, kingLifted, 2000U, 1U);
    Observe(stateMachine, 1ULL << KING_TO, afterPriorMove, 2001U, 2U);
    FinishSettle(stateMachine, coordinator, 2020U);

    char fenAfterPriorMove[128] = {};
    Check(engine.GetFen(fenAfterPriorMove, sizeof(fenAfterPriorMove)),
          "cancel promotion reads prior-move FEN");
    Check(!SameFenBoard(fenAfterPriorMove, FEN),
          "prior move is committed before promotion attempt");

    stateMachine.SetValidDestMask(1ULL << PROMOTION_TO);
    const uint64_t pawnLifted = afterPriorMove & ~(1ULL << PAWN_FROM);
    const uint64_t targetLifted = pawnLifted & ~(1ULL << PROMOTION_TO);
    const uint64_t promotedPhysical = targetLifted | (1ULL << PROMOTION_TO);
    Observe(stateMachine, 1ULL << PAWN_FROM, pawnLifted, 2030U, 3U);
    Observe(stateMachine, 1ULL << PROMOTION_TO, targetLifted, 2031U, 4U);
    Observe(stateMachine, 1ULL << PROMOTION_TO, promotedPhysical, 2032U, 5U);
    FinishSettle(stateMachine, coordinator, 2050U);
    Check(stateMachine.IsPromotionActive(),
          "cancel scenario opens promotion choice");

    Check(coordinator.RequestUndo(1U),
          "cancel scenario requests promotion cancellation");
    coordinator.Update();

    char fenAfterCancel[128] = {};
    Check(engine.GetFen(fenAfterCancel, sizeof(fenAfterCancel)),
          "cancel scenario reads engine after cancellation");
    Check(SameFenBoard(fenAfterCancel, fenAfterPriorMove),
          "canceling pending promotion does not undo the prior move");
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "canceling pending promotion removes only provisional move history");
    uint8_t restoreSquare = 255U;
    char restorePiece = '\0';
    Check(stateMachine.TryGetUndoRestoreInfo(restoreSquare, restorePiece) &&
              restoreSquare == PROMOTION_TO && restorePiece == 'r',
          "canceling capture promotion prompts rook restoration");

    const uint64_t promotionRemoved = promotedPhysical &
                                      ~(1ULL << PROMOTION_TO);
    const uint64_t pawnReturned = promotionRemoved | (1ULL << PAWN_FROM);
    Observe(stateMachine, 1ULL << PROMOTION_TO, promotionRemoved, 2060U, 6U);
    Observe(stateMachine, 1ULL << PAWN_FROM, pawnReturned, 2061U, 7U);
    Observe(stateMachine, 1ULL << PROMOTION_TO, afterPriorMove, 2062U, 8U);
    coordinator.Update();

    char fenAfterCorrection[128] = {};
    Check(engine.GetFen(fenAfterCorrection, sizeof(fenAfterCorrection)),
          "cancel scenario reads engine after physical correction");
    Check(SameFenBoard(fenAfterCorrection, fenAfterPriorMove),
          "physical promotion correction does not mutate engine history");
    Check(!stateMachine.IsUndoPromptActive(),
          "promotion cancellation correction prompt completes");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle,
          "promotion cancellation returns authoritative interaction to idle");
}

} // namespace

int main() {
    std::puts("[PROMOTION_SPECIAL_MOVES] start");
    fxe::ScopedMillisProviderOverride millisGuard(
        &fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    TestCapturePromotionRetainsUndoMetadata();
    TestCancelPromotionDoesNotUndoPriorMove();

    if (g_failures == 0) {
        std::puts("[PROMOTION_SPECIAL_MOVES] ok");
        return 0;
    }
    std::printf("[PROMOTION_SPECIAL_MOVES] failures=%d\n", g_failures);
    return 1;
}
