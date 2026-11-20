#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <string>

#include "logger.hpp"

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[LEGALITY_MASK_INTEGRATION] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_legality_mask_integration.json";
    cfg.deferredSaveDelayMs = 999999;

    (void)std::remove(cfg.path);
    (void)std::remove("build/test_legality_mask_integration.json.bak");
    (void)std::remove("build/test_legality_mask_integration.json.tmp");

    runtime::ManualClock clock(1000);
    fxe::ManualMillis::SetNowMs(clock.NowMs());

    const auto advanceTime = [&](const uint32_t deltaMs) {
        clock.AdvanceMs(deltaMs);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
    };

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::SettingsStore settings(cfg);
    Check(settings.Load(), "settings load");

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    settings.Doc()["DEBUG_PICKUP"] = true;
    std::string emitted;
    logger::SetMinLevel(logger::Level::INFO);
    logger::SetOutputLineCallback([&emitted](const char* line) {
        emitted += line;
        emitted += '\n';
    });

    // An own piece blocked by its neighbors keeps source guidance, with no
    // legal destinations. Only trying to place it elsewhere needs correction.
    sm.HandleSensorChange(56, false);
    Check(sm.IsHumanPieceInHand(), "a1 rook lift enters PickedUp");
    coord.UpdateLegalDestinationsMask();
    Check(emitted.find("pickup:") == std::string::npos,
          "Trace-off suppresses production pickup chatter before the transport sink");
    logger::Warn("sensor read failed; retrying");
    logger::Info("sensor read recovered");
    Check(emitted.find("sensor read failed") != std::string::npos &&
              emitted.find("sensor read recovered") != std::string::npos,
          "fault and meaningful recovery still reach the same sink at INFO");
    emitted.clear();
    logger::SetMinLevel(logger::Level::TRACE);
    Check(sm.IsHumanPieceInHand(), "blocked a1 rook stays picked up");
    Check(!sm.HasHumanIllegal() && sm.GetValidDestMask() == 0ULL,
          "blocked rook has empty destinations without illegal feedback");
    uint8_t illegalKingSquare = 255;
    uint8_t illegalAttackerSquare = 255;
    Check(!sm.TryGetKingSafetyPrompt(illegalKingSquare, illegalAttackerSquare),
          "blocked rook does not imply self-check prompt");

    sm.HandleSensorChange(40, true); // a3 is not reachable by the blocked rook.
    Check(sm.IsHumanPieceInHand() && !sm.IsMoveSettlingActive() &&
              sm.GetHumanIllegalMask() == (1ULL << 40),
          "placing blocked rook away from source still requires local correction");
    sm.HandleSensorChange(40, false);
    Check(sm.IsHumanPieceInHand() && !sm.HasHumanIllegal(),
          "reversing rejected placement restores held-source guidance");

    // Return the piece and tick once; illegal should clear when occupancy matches expected.
    sm.HandleSensorChange(56, true);
    advanceTime(1);
    sm.Tick();
    Check(!sm.HasHumanIllegal(), "illegal clears after returning piece");

    const char* pinnedKnightFen = "4r3/8/8/8/8/8/4N3/4K3 w - - 0 1";
    Check(coord.LoadExternalFen(pinnedKnightFen), "LoadExternalFen pinned knight");
    sm.ApplyPhysicalOccupancySnapshot((1ULL << 4) | (1ULL << 52) | (1ULL << 60));
    sm.ExitPlacementMode();

    sm.HandleSensorChange(52, false);
    Check(sm.IsHumanPieceInHand(), "pinned knight lift enters PickedUp");
    coord.UpdateLegalDestinationsMask();
    Check(emitted.find("[TRACE] pickup:") != std::string::npos,
          "Trace-on retains explicitly enabled pickup diagnostics");
    logger::SetOutputLineCallback(nullptr);
    logger::SetMinLevel(logger::Level::INFO);
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "pinned knight pickup is rejected to Idle");
    Check(sm.HasHumanIllegal(), "pinned knight pickup latches illegal");
    Check(sm.TryGetKingSafetyPrompt(illegalKingSquare, illegalAttackerSquare),
          "pinned knight pickup exposes illegal self-check prompt");
    Check(illegalKingSquare == 60, "pinned knight self-check prompt king square is e1");
    Check(illegalAttackerSquare == 4, "pinned knight self-check prompt attacker square is e8");

    sm.HandleSensorChange(52, true);
    advanceTime(1);
    sm.Tick();
    Check(!sm.TryGetKingSafetyPrompt(illegalKingSquare, illegalAttackerSquare),
          "self-check prompt clears after returning pinned knight");

    struct PinCase {
        const char* fen;
        uint64_t occupancy;
        uint8_t source;
        uint8_t king;
        uint8_t pinner;
    };
    const PinCase pinCases[] = {
        {"k7/8/8/8/7b/8/5B2/4K3 w - - 0 1",
         (1ULL << 0) | (1ULL << 39) | (1ULL << 53) | (1ULL << 60), 53, 60, 39},
        {"7k/8/8/8/q2R3K/8/8/8 w - - 0 1",
         (1ULL << 7) | (1ULL << 32) | (1ULL << 35) | (1ULL << 39), 35, 39, 32},
        // The checking bishop already attacks e1. Lifting e2 newly exposes
        // the Rook; explain that actual pin, rather than the unrelated check.
        {"k3r3/8/8/8/1b6/8/4N3/4K3 w - - 0 1",
         (1ULL << 0) | (1ULL << 4) | (1ULL << 33) | (1ULL << 52) | (1ULL << 60), 52, 60, 4},
        // A second blocker and a non-sliding attacker cannot pin the Rook.
        {"k3r3/8/8/8/8/4P3/4R3/4K3 w - - 0 1",
         (1ULL << 0) | (1ULL << 4) | (1ULL << 44) | (1ULL << 52) | (1ULL << 60), 52, 60, 255},
        {"k7/8/8/8/4n3/8/4R3/4K3 w - - 0 1",
         (1ULL << 0) | (1ULL << 36) | (1ULL << 52) | (1ULL << 60), 52, 60, 255},
    };
    for (const auto& position : pinCases) {
        Check(coord.LoadExternalFen(position.fen), "load focused pin geometry");
        sm.ApplyPhysicalOccupancySnapshot(position.occupancy);
        sm.ExitPlacementMode();
        uint64_t legal = 0ULL;
        Check(engine.GetLegalDestinationsMask(position.source, legal), "reference real engine legality before pickup");
        sm.HandleSensorChange(position.source, false);
        coord.UpdateLegalDestinationsMask();
        uint8_t king = 255U, pinner = 255U;
        const bool warning = sm.TryGetKingSafetyPrompt(king, pinner);
        Check(warning == (position.pinner < 64U), "only a true absolute pin produces exposure feedback");
        Check(!warning || (king == position.king && pinner == position.pinner),
              "diagonal and horizontal pins identify the newly exposed slider");
        Check(legal == 0ULL || (sm.IsHumanPieceInHand() && sm.GetValidDestMask() == legal && !sm.HasHumanIllegal()),
              "exposure feedback preserves every legal move supplied by the production rules");
        sm.HandleSensorChange(position.source, true);
        coord.UpdateLegalDestinationsMask();
        Check(!sm.TryGetKingSafetyPrompt(king, pinner), "each restored source retires its exposure feedback");
    }

    const char* epOnlyFen = "4k3/8/4n3/3pP3/8/8/8/4K3 w - d6 0 1";
    Check(coord.LoadExternalFen(epOnlyFen), "LoadExternalFen ep-only");
    sm.ApplyPhysicalOccupancySnapshot((1ULL << 4) | (1ULL << 20) | (1ULL << 27) | (1ULL << 28) | (1ULL << 60));
    sm.ExitPlacementMode();

    uint64_t epDestMask64 = 0;
    Check(engine.GetLegalDestinationsMask(28, epDestMask64), "engine legal mask for e5 returns true");
    Check(epDestMask64 == (1ULL << 19), "engine legal mask for e5 is d6 only");

    Check(engine.SetFen("4k3/8/4n3/3pP3/8/8/8/4K3 w - - 0 1"), "engine desync removes ep square");

    sm.HandleSensorChange(28, false);
    Check(sm.IsHumanPieceInHand(), "ep-only pawn lift enters PickedUp");
    coord.UpdateLegalDestinationsMask();
    Check(sm.IsHumanPieceInHand(), "ep-only pawn pickup stays PickedUp");
    Check(!sm.HasHumanIllegal(), "ep-only pawn pickup does not latch illegal");
    Check(sm.GetValidDestMask() == (1ULL << 19), "ep-only pawn pickup restores d6 legal mask");

    sm.HandleSensorChange(28, true);
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "ep-only pawn return cancels pickup");

    const char* normalCaptureFen = "4k3/8/3nn3/4P3/8/8/8/4K3 w - - 0 1";
    Check(coord.LoadExternalFen(normalCaptureFen), "LoadExternalFen normal capture");
    sm.ApplyPhysicalOccupancySnapshot((1ULL << 4) | (1ULL << 19) | (1ULL << 20) | (1ULL << 28) | (1ULL << 60));
    sm.ExitPlacementMode();

    uint64_t captureDestMask64 = 0;
    Check(engine.GetLegalDestinationsMask(28, captureDestMask64), "engine legal mask for e5 capture returns true");
    Check(captureDestMask64 == (1ULL << 19), "engine legal mask for e5 capture is d6 only");

    Check(engine.SetFen("4k3/8/3nn3/8/4P3/8/8/4K3 w - - 0 1"), "engine desync leaves pawn on e4");

    sm.HandleSensorChange(28, false);
    Check(sm.IsHumanPieceInHand(), "normal capture pawn lift enters PickedUp");
    coord.UpdateLegalDestinationsMask();
    Check(sm.IsHumanPieceInHand(), "normal capture pawn pickup stays PickedUp");
    Check(!sm.HasHumanIllegal(), "normal capture pawn pickup does not latch illegal");
    Check(sm.GetValidDestMask() == (1ULL << 19), "normal capture pawn pickup restores d6 legal mask");

    sm.HandleSensorChange(28, true);
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "normal capture pawn return cancels pickup");

    // Start position: pick up the pawn from e2 (square 52).
    Check(coord.LoadExternalFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
          "LoadExternalFen initial");
    sm.ApplyPhysicalOccupancySnapshot(0xFFFF00000000FFFFULL);
    sm.ExitPlacementMode();
    sm.HandleSensorChange(52, false);
    Check(sm.IsHumanPieceInHand(), "Idle->PickedUp");

    // Force legality mask update (this mirrors the firmware sensor cadence).
    coord.UpdateLegalDestinationsMask();

    // Once this selected source has engine-derived legal guidance, a later
    // sequential placement outside it gets immediate local feedback.
    sm.HandleSensorChange(28, true); // e5 (illegal)
    Check(sm.IsHumanPieceInHand(),
          "sequential illegal destination keeps the selected piece in hand");
    Check(!sm.IsMoveSettlingActive(),
          "sequential illegal destination never starts settle");
    Check(sm.HasHumanIllegal(),
          "sequential illegal destination immediately latches feedback");
    Check((sm.GetHumanIllegalMask() & (1ULL << 28)) != 0ULL,
          "sequential illegal feedback marks only its target square");

    // Remove the rejected placement, then finish the still-selected move on a
    // legal destination.
    sm.HandleSensorChange(28, false);
    Check(!sm.HasHumanIllegal(),
          "sequential illegal target clears when it is removed");
    Check(sm.IsHumanPieceInHand(),
          "removing sequential illegal target preserves placement flow");
    sm.HandleSensorChange(36, true); // e4
    Check(sm.IsMoveSettlingActive(), "legal destination settles");

    advanceTime(10U);
    sm.Tick();
    coord.Update();
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "settle completes");

    // A source removal and placement first seen in one atomic scan has no
    // prior source-specific legality mask. Keep that case transactional and
    // let the authoritative engine reject it.
    Check(coord.LoadExternalFen(
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
          "reload initial position for atomic invalid placement");
    constexpr uint64_t INITIAL_OCC = 0xFFFF00000000FFFFULL;
    sm.ApplyPhysicalOccupancySnapshot(INITIAL_OCC);
    sm.ExitPlacementMode();
    const uint64_t atomicInvalidOcc =
        (INITIAL_OCC & ~(1ULL << 52)) | (1ULL << 28);
    sm.HandleBoardObservation((1ULL << 52) | (1ULL << 28),
                              atomicInvalidOcc,
                              clock.NowMs(),
                              sm.GetLastBoardObservationSequence() + 1U);
    Check(sm.IsMoveSettlingActive(),
          "same-scan invalid placement reaches transactional settle");
    advanceTime(10U);
    sm.Tick();
    coord.Update();
    Check(sm.HasHumanIllegal(),
          "same-scan invalid placement is rejected by the engine result");

    if (g_failures == 0) {
        std::puts("[LEGALITY_MASK_INTEGRATION] ok");
        return 0;
    }

    std::printf("[LEGALITY_MASK_INTEGRATION] failures=%d\n", g_failures);
    return 1;
}
