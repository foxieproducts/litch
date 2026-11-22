#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/chess_interaction_persistence.hpp"
#include "runtime/clock.hpp"

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

void TestDurableCodec() {
    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    model.phase.tag = runtime::ChessInteractionPhase::ExecutingPlan;
    model.phase.requiredAction = runtime::RequiredPhysicalAction::RestoreCapture;
    model.phase.plan.kind = runtime::PhysicalTransitionKind::Undo;
    model.phase.plan.source = runtime::PhysicalTransitionSource::Undo;
    model.phase.plan.beforeOccupiedMask = 1ULL << 27U;
    model.phase.plan.afterOccupiedMask = (1ULL << 27U) | (1ULL << 36U);
    model.phase.plan.moveFrom = 36U;
    model.phase.plan.moveTo = 27U;
    model.phase.plan.relocationCount = 1U;
    model.phase.plan.relocations[0] = {27U, 36U};
    model.phase.plan.restorationRequired = true;
    model.phase.plan.restorationSquare = 27U;
    model.phase.plan.restorationPieceFen = 'p';
    model.phase.plan.progress.relocationsLiftedMask = 1U;
    model.phase.plan.progress.relocationsPlacedMask = 1U;
    model.phase.plan.progress.actionAcknowledged = true;
    model.nextActionToken = 19U;

    char encoded[128] = {};
    Check(runtime::EncodeChessInteractionDurableState(
              model, encoded, sizeof(encoded)),
          "encode versioned durable interaction");
    Check(encoded[0] == '0' && encoded[1] == '3',
          "durable encoding begins with version three");

    runtime::ChessInteractionDurableState decoded;
    Check(runtime::DecodeChessInteractionDurableState(encoded, decoded),
          "decode versioned durable interaction");
    runtime::ChessInteractionModel restored;
    runtime::ResetChessInteractionModel(restored);
    Check(runtime::RestoreChessInteractionDurableState(restored, decoded),
          "restore decoded interaction through validated boundary");
    Check(restored.phase.tag == model.phase.tag &&
              restored.phase.plan.kind == model.phase.plan.kind &&
              restored.phase.plan.source == model.phase.plan.source,
          "tag, kind, and source round-trip");
    Check(restored.phase.plan.afterOccupiedMask ==
              model.phase.plan.afterOccupiedMask &&
              restored.phase.plan.restorationSquare == 27U &&
              restored.phase.plan.progress.actionAcknowledged,
          "inverse target and durable progress round-trip");
    Check(restored.nextActionToken == 19U,
          "next action token round-trips");
    Check(restored.board.valid &&
              restored.board.occupiedMask == (1ULL << 36U),
          "durable progress restores canonical physical occupancy");
    runtime::ChessInteractionModel acknowledgedBridge = model;
    acknowledgedBridge.phase.plan.source =
        runtime::PhysicalTransitionSource::Human;
    Check(!runtime::CanSafelyPersistChessInteractionState(
              acknowledgedBridge),
          "action-acknowledged bridge is not a durable boundary");

    char versionTwoEncoded[128] = {};
    std::strcpy(versionTwoEncoded, encoded);
    versionTwoEncoded[0] = '0';
    versionTwoEncoded[1] = '2';
    Check(runtime::DecodeChessInteractionDurableState(
              versionTwoEncoded, decoded),
          "version-two durable state remains decodable");

    // Version one carried a pickup-order byte that was never read by the
    // reducer. Keep accepting that deployed format while normalizing it to the
    // current plan.
    char legacyEncoded[128] = {};
    std::memcpy(legacyEncoded, encoded, 44U);
    legacyEncoded[0] = '0';
    legacyEncoded[1] = '1';
    legacyEncoded[44] = '0';
    legacyEncoded[45] = '2';
    std::strcpy(legacyEncoded + 46U, encoded + 44U);
    Check(runtime::DecodeChessInteractionDurableState(
              legacyEncoded, decoded),
          "version-one durable state ignores retired pickup-order byte");

    runtime::ChessInteractionModel cpuCapture;
    runtime::ResetChessInteractionModel(cpuCapture);
    cpuCapture.phase.tag = runtime::ChessInteractionPhase::ExecutingPlan;
    cpuCapture.phase.requiredAction =
        runtime::RequiredPhysicalAction::LiftPrimary;
    cpuCapture.phase.plan.kind = runtime::PhysicalTransitionKind::Capture;
    cpuCapture.phase.plan.source = runtime::PhysicalTransitionSource::Cpu;
    cpuCapture.phase.plan.beforeOccupiedMask =
        (1ULL << 27U) | (1ULL << 36U);
    cpuCapture.phase.plan.afterOccupiedMask = 1ULL << 36U;
    cpuCapture.phase.plan.moveFrom = 27U;
    cpuCapture.phase.plan.moveTo = 36U;
    cpuCapture.phase.plan.relocationCount = 1U;
    cpuCapture.phase.plan.relocations[0] = {27U, 36U};
    cpuCapture.phase.plan.removalRequired = true;
    cpuCapture.phase.plan.removalSquare = 36U;
    cpuCapture.phase.plan.progress.relocationsPlacedMask = 1U;
    Check(runtime::EncodeChessInteractionDurableState(
              cpuCapture, encoded, sizeof(encoded)),
          "encode CPU destination-capture seed fixture");
    Check(encoded[42] == '0' && encoded[43] == '1',
          "CPU choreography retains deployed numeric source value one");
    char cpuEncoded[128] = {};
    std::strcpy(cpuEncoded, encoded);
    cpuCapture.phase.plan.directedOrigin =
        runtime::DirectedMoveOrigin::Provider;
    char providerOriginEncoded[128] = {};
    Check(runtime::EncodeChessInteractionDurableState(
              cpuCapture, providerOriginEncoded,
              sizeof(providerOriginEncoded)) &&
              std::strcmp(cpuEncoded, providerOriginEncoded) == 0,
          "orthogonal directed origin does not change deployed encoding");
    Check(runtime::IsProviderDirectedTransition(cpuCapture.phase.plan) &&
              !runtime::IsCpuDirectedTransition(cpuCapture.phase.plan),
          "provider and CPU ownership are distinguishable in memory");
    Check(runtime::DecodeChessInteractionDurableState(encoded, decoded) &&
              decoded.phase.plan.source ==
                  runtime::PhysicalTransitionSource::Directed &&
              runtime::IsCpuDirectedTransition(decoded.phase.plan) &&
              decoded.phase.plan.progress.relocationsPlacedMask == 0U,
          "old value-one record restores as CPU with legacy normalization");
    std::memcpy(legacyEncoded, encoded, 44U);
    legacyEncoded[0] = '0';
    legacyEncoded[1] = '1';
    legacyEncoded[44] = '0';
    legacyEncoded[45] = '0';
    std::strcpy(legacyEncoded + 46U, encoded + 44U);
    Check(runtime::DecodeChessInteractionDurableState(
              legacyEncoded, decoded) &&
              decoded.phase.plan.progress.relocationsPlacedMask == 0U,
          "legacy decode clears ambiguous CPU capture placement seed");

    constexpr uint8_t DUCK_FROM = 28U;
    constexpr uint8_t DUCK_TO = 20U;
    runtime::ChessInteractionModel directedDuck;
    runtime::ResetChessInteractionModel(directedDuck);
    directedDuck.acknowledged.occupiedMask = 1ULL << DUCK_FROM;
    Check(runtime::BeginDuckRelocationPlan(
              directedDuck, DUCK_FROM,
              runtime::CHESS_INTERACTION_INVALID_SQUARE,
              runtime::PhysicalTransitionSource::Directed,
              runtime::DirectedMoveOrigin::Cpu),
          "build selectable CPU-origin Directed Duck fixture");
    Check(!runtime::EncodeChessInteractionDurableState(
              directedDuck, encoded, sizeof(encoded)),
          "selectable Directed Duck is not durable for CPU origin");
    directedDuck.phase.plan.directedOrigin =
        runtime::DirectedMoveOrigin::Provider;
    Check(!runtime::EncodeChessInteractionDurableState(
              directedDuck, encoded, sizeof(encoded)),
          "selectable Directed Duck is not durable for provider origin");

    runtime::ResetChessInteractionModel(directedDuck);
    directedDuck.acknowledged.occupiedMask = 1ULL << DUCK_FROM;
    Check(runtime::BeginDuckRelocationPlan(
              directedDuck, DUCK_FROM, DUCK_TO,
              runtime::PhysicalTransitionSource::Directed,
              runtime::DirectedMoveOrigin::Cpu),
          "build fixed-destination CPU-origin Directed Duck fixture");
    char cpuDuckEncoded[128] = {};
    Check(runtime::EncodeChessInteractionDurableState(
              directedDuck, cpuDuckEncoded, sizeof(cpuDuckEncoded)),
          "fixed-destination Directed Duck encodes for CPU origin");
    directedDuck.phase.plan.directedOrigin =
        runtime::DirectedMoveOrigin::Provider;
    char providerDuckEncoded[128] = {};
    Check(runtime::EncodeChessInteractionDurableState(
              directedDuck, providerDuckEncoded,
              sizeof(providerDuckEncoded)) &&
              std::strcmp(cpuDuckEncoded, providerDuckEncoded) == 0,
          "fixed Directed Duck bytes are origin-independent");
    Check(runtime::DecodeChessInteractionDurableState(
              providerDuckEncoded, decoded) &&
              runtime::IsCpuDirectedTransition(decoded.phase.plan),
          "fixed Directed Duck durable value restores with CPU origin");

    encoded[0] = 'f';
    Check(!runtime::DecodeChessInteractionDurableState(encoded, decoded),
          "unknown durable version is rejected");
    Check(runtime::EncodeChessInteractionDurableState(
              model, encoded, sizeof(encoded)),
          "re-encode durable interaction for corruption checks");
    encoded[2] = 'f';
    encoded[3] = 'f';
    Check(!runtime::DecodeChessInteractionDurableState(encoded, decoded),
          "unknown durable phase tag is rejected");
    Check(runtime::EncodeChessInteractionDurableState(
              model, encoded, sizeof(encoded)),
          "re-encode durable interaction for retired-action check");
    encoded[6] = '0';
    encoded[7] = '6';
    Check(!runtime::DecodeChessInteractionDurableState(encoded, decoded),
          "retired promotion-action slot is rejected");
}

void TestPromotionRebootRoundTrip() {
    const char* path = "build/test_interaction_promotion_reboot.json";
    const char* fen = "7k/P7/8/8/8/8/8/7K w - - 0 1";
    constexpr uint64_t FROM = 1ULL << 8U;
    constexpr uint64_t TO = 1ULL << 0U;
    (void)std::remove(path);
    (void)std::remove("build/test_interaction_promotion_reboot.json.bak");
    (void)std::remove("build/test_interaction_promotion_reboot.json.tmp");

    uint32_t pendingToken = 0U;
    {
        modules::SettingsStoreConfig settingsConfig;
        settingsConfig.path = path;
        settingsConfig.deferredSaveDelayMs = 999999U;
        modules::SettingsStore settings(settingsConfig);
        Check(settings.Load(), "promotion reboot settings load A");
        settings.Doc()["PROMOTION_MODE"] = "choose";
        runtime::GameStateMachineConfig config;
        config.settleDurationMs = 10U;
        config.cpuIdleDelayMs = 999999U;
        config.placementStableRequiredMs = 0U;
        runtime::GameStateMachine stateMachine(config);
        test_support::BoundedHintEngine engine;
        modules::GamePersistenceCoordinator coordinator(
            settings, engine, stateMachine);
        const uint64_t before = FenOccupancy(fen);
        stateMachine.DebugSetPhysicalOccupancyMask(before);
        Check(coordinator.LoadExternalFen(fen), "load promotion reboot FEN");
        stateMachine.HandleBoardObservation(0, before, 1000U, 1U);
        stateMachine.HandleBoardObservation(FROM | TO,
                                            (before & ~FROM) | TO,
                                            1000U,
                                            2U);
        fxe::ManualMillis::SetNowMs(1020U);
        stateMachine.Tick();
        coordinator.Update();
        Check(stateMachine.IsPromotionActive(),
              "promotion modal active before reboot");
        pendingToken = stateMachine.GetChessInteractionModel()
                           .phase.pendingActionToken;
        Check(pendingToken != 0U,
              "promotion pending token exists before reboot");
        Check(settings.SaveNow(),
              "promotion snapshot reaches storage after debounce simulation");
    }

    for (const char* malformed : {"null", "true", "1", "[]", "{}"}) {
        modules::SettingsStoreConfig settingsConfig;
        settingsConfig.path = path;
        settingsConfig.deferredSaveDelayMs = 999999U;
        modules::SettingsStore settings(settingsConfig);
        Check(settings.Load(), "load the actual pending promotion before corrupting its JSON type");
        Check(settings.Doc()[modules::KEY_GAME_INTERACTION_V1].is<const char*>(),
              "saved promotion has an encoded durable interaction");
        StaticJsonDocument<64U> value;
        Check(!deserializeJson(value, malformed), "promotion wrong-type fixture parses");
        Check(settings.Doc()[modules::KEY_GAME_INTERACTION_V1].set(value.as<JsonVariantConst>()),
              "wrong-type promotion fixture fits the loaded document");
        runtime::GameStateMachine stateMachine;
        test_support::BoundedHintEngine engine;
        modules::GamePersistenceCoordinator coordinator(settings, engine, stateMachine);
        stateMachine.DebugSetPhysicalOccupancyMask((FenOccupancy(fen) & ~FROM) | TO);
        Check(!coordinator.LoadOrInitialize(),
              "wrong-typed pending promotion fails restore instead of losing its physical plan");
        Check(settings.Doc().containsKey(modules::KEY_GAME_INTERACTION_V1) &&
                  !settings.Doc()[modules::KEY_GAME_INTERACTION_V1].is<const char*>() &&
                  std::strcmp(settings.Doc()[modules::KEY_GAME_FEN] | "", fen) == 0,
              "rejected restoration preserves the input record for boot recovery");
    }

    {
        modules::SettingsStoreConfig settingsConfig;
        settingsConfig.path = path;
        settingsConfig.deferredSaveDelayMs = 999999U;
        modules::SettingsStore settings(settingsConfig);
        Check(settings.Load(), "promotion reboot settings load B");
        runtime::GameStateMachineConfig config;
        config.settleDurationMs = 10U;
        config.cpuIdleDelayMs = 999999U;
        config.placementStableRequiredMs = 0U;
        runtime::GameStateMachine stateMachine(config);
        test_support::BoundedHintEngine engine;
        modules::GamePersistenceCoordinator coordinator(
            settings, engine, stateMachine);
        const uint64_t physicalAfter =
            (FenOccupancy(fen) & ~FROM) | TO;
        stateMachine.DebugSetPhysicalOccupancyMask(physicalAfter);
        Check(coordinator.LoadOrInitialize(),
              "restore promotion interaction after reboot");
        stateMachine.ApplyPhysicalOccupancySnapshot(physicalAfter);
        coordinator.Update();
        Check(stateMachine.IsPromotionActive(),
              "promotion modal restored after reboot");
        Check(stateMachine.GetChessInteractionModel()
                  .phase.pendingActionToken == pendingToken,
              "promotion action token restored exactly");

        stateMachine.HandleButtonEvent(modules::UserButton::P1O,
                                       fxe::Button::Event::Press);
        coordinator.Update();
        char afterFen[128] = {};
        Check(engine.GetFen(afterFen, sizeof(afterFen)),
              "read promoted FEN after reboot");
        Check(afterFen[0] == 'Q',
              "restored promotion commits selected piece exactly once");
        Check(stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle,
              "restored promotion acknowledgement completes plan");
    }
}

void TestPartialCastleRebootRoundTrip() {
    const char* path = "build/test_interaction_castle_reboot.json";
    const char* fen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";
    constexpr uint8_t KING_FROM = 60U;
    constexpr uint8_t KING_TO = 62U;
    constexpr uint8_t ROOK_FROM = 63U;
    constexpr uint8_t ROOK_TO = 61U;
    constexpr uint64_t PICKUP =
        (1ULL << KING_FROM) | (1ULL << ROOK_FROM);
    (void)std::remove(path);
    (void)std::remove("build/test_interaction_castle_reboot.json.bak");
    (void)std::remove("build/test_interaction_castle_reboot.json.tmp");

    uint64_t partialPhysical = 0ULL;
    char acknowledgedFen[128] = {};
    {
        modules::SettingsStoreConfig settingsConfig;
        settingsConfig.path = path;
        settingsConfig.deferredSaveDelayMs = 999999U;
        modules::SettingsStore settings(settingsConfig);
        Check(settings.Load(), "castle reboot settings load A");
        runtime::GameStateMachineConfig config;
        config.settleDurationMs = 10U;
        config.cpuIdleDelayMs = 999999U;
        config.placementStableRequiredMs = 0U;
        runtime::GameStateMachine stateMachine(config);
        test_support::BoundedHintEngine engine;
        modules::GamePersistenceCoordinator coordinator(
            settings, engine, stateMachine);
        const uint64_t before = FenOccupancy(fen);
        stateMachine.DebugSetPhysicalOccupancyMask(before);
        Check(coordinator.LoadExternalFen(fen), "load castle reboot FEN");
        stateMachine.HandleBoardObservation(0, before, 2000U, 1U);
        const uint64_t lifted = before & ~PICKUP;
        stateMachine.HandleBoardObservation(PICKUP, lifted, 2000U, 2U);
        partialPhysical = lifted | (1ULL << KING_TO);
        stateMachine.HandleBoardObservation(1ULL << KING_TO,
                                            partialPhysical,
                                            2000U,
                                            3U);
        fxe::ManualMillis::SetNowMs(2020U);
        stateMachine.Tick();
        coordinator.Update();
        Check(stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan,
              "partial castle retains executing plan before engine action");
        Check(!stateMachine.GetChessInteractionModel()
                   .phase.plan.progress.actionAcknowledged,
              "partial castle persists without premature engine acknowledgement");
        Check(engine.GetFen(acknowledgedFen, sizeof(acknowledgedFen)),
              "read acknowledged castle FEN");
        Check(settings.SaveNow(),
              "partial castle reaches storage after debounce simulation");
    }

    {
        modules::SettingsStoreConfig settingsConfig;
        settingsConfig.path = path;
        settingsConfig.deferredSaveDelayMs = 999999U;
        modules::SettingsStore settings(settingsConfig);
        Check(settings.Load(), "castle reboot settings load B");
        runtime::GameStateMachineConfig config;
        config.settleDurationMs = 10U;
        config.cpuIdleDelayMs = 999999U;
        config.placementStableRequiredMs = 0U;
        runtime::GameStateMachine stateMachine(config);
        test_support::BoundedHintEngine engine;
        modules::GamePersistenceCoordinator coordinator(
            settings, engine, stateMachine);
        Check(coordinator.LoadOrInitialize(),
              "restore partial castle after reboot");
        stateMachine.ApplyPhysicalOccupancySnapshot(partialPhysical);
        if (!stateMachine.IsPhysicalOccupancyConsistentWithInteraction()) {
            (void)stateMachine.ForceEnterPlacementMode();
        }
        Check(!stateMachine.IsPlacementModeActive(),
              "production-order boot scan preserves matching partial castle");
        Check(stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan,
              "partial castle tagged phase round-trips");
        Check(stateMachine.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlaceSecondary,
              "partial castle restores rook placement requirement");
        Check(stateMachine.IsCastlingRookPromptActive(),
              "partial castle restores compatibility rook prompt");

        const uint64_t finalPhysical = partialPhysical | (1ULL << ROOK_TO);
        const uint32_t sequence =
            stateMachine.GetLastBoardObservationSequence() + 1U;
        stateMachine.HandleBoardObservation(1ULL << ROOK_TO,
                                            finalPhysical,
                                            2040U,
                                            sequence);
        coordinator.Update();
        Check(stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle,
              "restored rook placement completes durable plan");
        char afterFen[128] = {};
        Check(engine.GetFen(afterFen, sizeof(afterFen)),
              "read castle FEN after restored placement");
        Check(std::strcmp(afterFen, acknowledgedFen) != 0,
              "restored rook placement applies deferred castle once");
        char stableFen[128] = {};
        coordinator.Update();
        Check(engine.GetFen(stableFen, sizeof(stableFen)) &&
                  std::strcmp(afterFen, stableFen) == 0,
              "restored castle is not applied a second time");
    }
}

void TestCpuCaptureRebootRoundTrip() {
    const char* path = "build/test_interaction_cpu_capture_reboot.json";
    const char* fen = "7k/8/8/3p4/4P3/8/8/7K b - - 0 1";
    constexpr uint8_t FROM = 27U;
    constexpr uint8_t TO = 36U;
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t TO_MASK = 1ULL << TO;
    (void)std::remove(path);
    (void)std::remove("build/test_interaction_cpu_capture_reboot.json.bak");
    (void)std::remove("build/test_interaction_cpu_capture_reboot.json.tmp");

    const uint64_t before = FenOccupancy(fen);
    const uint64_t captureRemoved = before & ~TO_MASK;
    const uint64_t after = before & ~FROM_MASK;
    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            before, 0ULL, 0ULL, FROM_MASK, before & ~FROM_MASK,
            FROM_MASK | TO_MASK));
    runtime::PhysicalTransitionPlan plan;
    plan.kind = runtime::PhysicalTransitionKind::Capture;
    plan.source = runtime::PhysicalTransitionSource::Cpu;
    plan.beforeOccupiedMask = before;
    plan.afterOccupiedMask = after;
    plan.moveFrom = FROM;
    plan.moveTo = TO;
    plan.relocationCount = 1U;
    plan.relocations[0] = {FROM, TO};
    plan.removalRequired = true;
    plan.removalSquare = TO;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePlanRequested(plan));
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            3000U, 1U, TO_MASK, captureRemoved));
    Check(model.phase.plan.progress.removalComplete,
          "CPU capture records removed destination before reboot");

    char encoded[128] = {};
    Check(runtime::EncodeChessInteractionDurableState(
              model, encoded, sizeof(encoded)),
          "encode partial CPU capture interaction");

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    {
        modules::SettingsStore settings(settingsConfig);
        Check(settings.Load(), "CPU capture reboot settings load A");
        modules::GamePersistenceState persisted;
        Check(persisted.fen.Assign(fen), "store CPU capture FEN");
        Check(persisted.undoBaseFen.Assign(fen),
              "store CPU capture undo base");
        Check(persisted.interactionState.Assign(encoded),
              "store CPU capture durable model");
        persisted.cpuMoveValid = true;
        persisted.cpuMoveFrom = FROM;
        persisted.cpuMoveTo = TO;
        persisted.cpuMoveCaptureSquareValid = true;
        persisted.cpuMoveCaptureSquare = TO;
        Check(modules::WriteGamePersistenceState(settings.Doc(), persisted),
              "write CPU capture reboot snapshot");
        Check(settings.SaveNow(), "save CPU capture reboot snapshot");
    }

    {
        modules::SettingsStore settings(settingsConfig);
        Check(settings.Load(), "CPU capture reboot settings load B");
        runtime::GameStateMachineConfig config;
        config.settleDurationMs = 0U;
        config.cpuIdleDelayMs = 999999U;
        config.placementStableRequiredMs = 0U;
        runtime::GameStateMachine stateMachine(config);
        test_support::BoundedHintEngine engine;
        modules::GamePersistenceCoordinator coordinator(
            settings, engine, stateMachine);
        Check(coordinator.LoadOrInitialize(),
              "restore partial CPU capture after reboot");
        stateMachine.ApplyPhysicalOccupancySnapshot(captureRemoved);
        if (!stateMachine.IsPhysicalOccupancyConsistentWithInteraction()) {
            (void)stateMachine.ForceEnterPlacementMode();
        }
        Check(!stateMachine.IsPlacementModeActive(),
              "production-order boot scan preserves matching CPU capture");
        const runtime::PhysicalTransitionPlan& restored =
            stateMachine.GetChessInteractionModel().phase.plan;
        Check(restored.kind == runtime::PhysicalTransitionKind::Capture &&
                  restored.source == runtime::PhysicalTransitionSource::Cpu &&
                  restored.progress.removalComplete,
              "CPU capture kind, source, and removal progress round-trip");

        stateMachine.HandleBoardObservation(
            FROM_MASK, captureRemoved & ~FROM_MASK, 3020U, 2U);
        stateMachine.HandleBoardObservation(
            TO_MASK, after, 3040U, 3U);
        stateMachine.Tick();
        coordinator.Update();
        Check(stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle,
              "restored CPU capture completes exactly once");
        char appliedFen[128] = {};
        Check(engine.GetFen(appliedFen, sizeof(appliedFen)) &&
                  std::strcmp(appliedFen, fen) != 0,
              "restored CPU capture applies to engine");
        char stableFen[128] = {};
        coordinator.Update();
        Check(engine.GetFen(stableFen, sizeof(stableFen)) &&
                  std::strcmp(appliedFen, stableFen) == 0,
              "restored CPU capture is not applied twice");
    }
}

} // namespace

int main() {
    std::puts("[CHESS_INTERACTION_PERSISTENCE] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    TestDurableCodec();
    TestPromotionRebootRoundTrip();
    TestPartialCastleRebootRoundTrip();
    TestCpuCaptureRebootRoundTrip();
    if (g_failures == 0) {
        std::puts("[CHESS_INTERACTION_PERSISTENCE] ok");
        return 0;
    }
    std::printf("[CHESS_INTERACTION_PERSISTENCE] failures=%d\n", g_failures);
    return 1;
}
