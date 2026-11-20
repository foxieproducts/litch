#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/chess_interaction_persistence.hpp"

namespace {

int gFailures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++gFailures;
    }
}

void Observe(runtime::ChessInteractionModel& model,
             const uint32_t sequence,
             const uint64_t changed,
             const uint64_t occupied) {
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            sequence, sequence, changed, occupied));
}

bool MakeDurablePromotion(const uint64_t before,
                          const uint64_t mover,
                          const uint64_t opponent,
                          const uint8_t from,
                          const uint8_t to,
                          char (&encoded)[128]) {
    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            before, 0ULL, 0ULL, mover, opponent, mover));
    Observe(model, 1U, 0ULL, before);
    uint64_t occupied = before & ~(1ULL << from);
    Observe(model, 2U, 1ULL << from, occupied);
    uint32_t sequence = 3U;
    if ((opponent & (1ULL << to)) != 0ULL) {
        occupied &= ~(1ULL << to);
        Observe(model, sequence++, 1ULL << to, occupied);
    }
    occupied |= 1ULL << to;
    Observe(model, sequence, 1ULL << to, occupied);
    runtime::ReduceChessInteraction(
        model, runtime::ChessInteractionEvent::MakeSettleConfirmed());
    return model.phase.tag == runtime::ChessInteractionPhase::AwaitingAction &&
           runtime::EncodeChessInteractionDurableState(
               model, encoded, sizeof(encoded));
}

void RunCase(const char* path,
             const char* fen,
             const uint64_t before,
             const uint64_t mover,
             const uint64_t opponent,
             const uint8_t from,
             const uint8_t to,
             const runtime::ChessInteractionPhase divergentPhase,
             const bool resetAfter = false, const bool replaceAfter = false) {
    char encoded[128] = {};
    Check(MakeDurablePromotion(
              before, mover, opponent, from, to, encoded),
          "build durable pending promotion");

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(path);
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "promotion boot settings load");
    settings.Doc()[modules::KEY_GAME_FEN] = fen;
    settings.Doc()[modules::KEY_GAME_INTERACTION_V1] = encoded;

    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 1U;
    config.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(config);
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(),
          "promotion boot coordinator restore");
    Check(!stateMachine.IsPromotionActive(),
          "restored promotion waits for the physical boot scan");

    coordinator.SetReplacementAdmissionCallback(
        [](void*, const modules::GameHistoryPersistenceState&,
           modules::GamePersistenceCoordinator::ReplacementKind) {
            return modules::ControlCoordinatorResult::Busy;
        }, nullptr);
    Check(!coordinator.ResetEngine() && !coordinator.LoadExternalFen(fen),
          "rejected reset and replacement retain the restored pending promotion");
    coordinator.SetReplacementAdmissionCallback(nullptr, nullptr);

    const uint64_t incomplete =
        before & ~(1ULL << from) & ~(1ULL << to);
    stateMachine.ApplyPhysicalOccupancySnapshot(incomplete);
    if (!stateMachine.IsPhysicalOccupancyConsistentWithInteraction()) {
        (void)stateMachine.ForceEnterPlacementMode();
    }
    Check(stateMachine.GetChessInteractionModel().phase.tag == divergentPhase,
          "divergent promotion scan reopens physical placement");
    Check(!stateMachine.IsPromotionActive() &&
              !stateMachine.HasCommittedMovePending(),
          "divergent promotion cannot expose a modal or engine action");
    coordinator.Update();
    Check(!stateMachine.IsPromotionActive(),
          "coordinator cannot restart promotion before exact occupancy");

    stateMachine.HandleSensorChange(to, true);
    fxe::ManualMillis::AdvanceMs(1U);
    stateMachine.Tick();
    coordinator.Update();
    Check(stateMachine.IsPromotionActive(),
          "exact physical completion restarts the promotion modal");
    if (replaceAfter) {
        Check(coordinator.LoadExternalFen(fen) && !stateMachine.IsPromotionActive(),
              "accepted external replacement cancels the old restored selector");
        stateMachine.ApplyPhysicalOccupancySnapshot(before);
        fxe::ManualMillis::AdvanceMs(2100U); stateMachine.Tick(); coordinator.Update();
        stateMachine.HandleSensorChange(from, false);
        coordinator.Update();
        if ((opponent & (UINT64_C(1) << to)) != 0U) stateMachine.HandleSensorChange(to, false);
        stateMachine.HandleSensorChange(to, true);
        fxe::ManualMillis::AdvanceMs(2100U);
        stateMachine.Tick();
        coordinator.Update();
        Check(!stateMachine.IsPromotionActive() && coordinator.GetUndoHistoryMoveCount() == 1U,
              "first new promotion after accepted replacement uses default Queen");
    }
    if (resetAfter) {
        Check(coordinator.ResetEngine() && !stateMachine.IsPromotionActive() &&
                  coordinator.GetUndoHistoryMoveCount() == 0U,
              "accepted new game cancels the restored selector and its history");
        stateMachine.ApplyPhysicalOccupancySnapshot(UINT64_C(0xFFFF00000000FFFF));
        fxe::ManualMillis::AdvanceMs(2100U); stateMachine.Tick(); coordinator.Update();
        const uint8_t moves[][3] = {{48,32,0},{1,18,0},{32,24,0},{15,23,0},
            {24,16,0},{23,31,0},{16,9,1},{31,39,0},{9,1,0}};
        for (const auto& move : moves) {
            stateMachine.HandleSensorChange(move[0], false);
            coordinator.Update();
            if (move[2]) { stateMachine.HandleSensorChange(move[1], false); coordinator.Update(); }
            stateMachine.HandleSensorChange(move[1], true);
            fxe::ManualMillis::AdvanceMs(2100U); stateMachine.Tick(); coordinator.Update();
        }
        char result[160] = {};
        Check(engine.GetFen(result, sizeof(result)) && result[1] == 'Q' &&
                  !stateMachine.IsPromotionActive() && coordinator.GetUndoHistoryMoveCount() == 9U,
              "raw new-game promotion uses default Queen without inherited restored choice");
    }

}

} // namespace

int main() {
    std::puts("[PROMOTION_BOOT_RECONCILIATION] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);

    constexpr uint64_t WHITE_KING = 1ULL << 63U;
    constexpr uint64_t BLACK_KING = 1ULL << 7U;
    constexpr uint64_t PAWN = 1ULL << 8U;
    constexpr uint64_t ROOK = 1ULL << 1U;
    RunCase("build/promotion_boot_quiet.json",
            "7k/P7/8/8/8/8/8/7K w - - 0 1",
            WHITE_KING | BLACK_KING | PAWN,
            WHITE_KING | PAWN,
            BLACK_KING,
            8U,
            0U,
            runtime::ChessInteractionPhase::PieceLifted);
    RunCase("build/promotion_boot_capture.json",
            "1r5k/P7/8/8/8/8/8/7K w - - 0 1",
            WHITE_KING | BLACK_KING | PAWN | ROOK,
            WHITE_KING | PAWN,
            BLACK_KING | ROOK,
            8U,
            1U,
            runtime::ChessInteractionPhase::ExecutingPlan);

    RunCase("build/promotion_boot_reset.json", "7k/P7/8/8/8/8/8/7K w - - 0 1",
            WHITE_KING | BLACK_KING | PAWN, WHITE_KING | PAWN, BLACK_KING,
            8U, 0U, runtime::ChessInteractionPhase::PieceLifted, true);
    RunCase("build/promotion_boot_replace.json", "7k/P7/8/8/8/8/8/7K w - - 0 1",
            WHITE_KING | BLACK_KING | PAWN, WHITE_KING | PAWN, BLACK_KING,
            8U, 0U, runtime::ChessInteractionPhase::PieceLifted, false, true);

    if (gFailures == 0) {
        std::puts("[PROMOTION_BOOT_RECONCILIATION] ok");
        return 0;
    }
    std::printf("[PROMOTION_BOOT_RECONCILIATION] failures=%d\n", gFailures);
    return 1;
}
