#include <cstdint>
#include <cstdio>

#include "duck_coordinator_test_support.hpp"
#include "foxie_embedded/millis.hpp"
#include "runtime/interaction_dispatcher.hpp"

namespace {

using duck_coordinator_test::Harness;
using duck_coordinator_test::INITIAL_OCCUPANCY;

constexpr uint8_t DUCK_BEFORE = 24U;
constexpr uint8_t DUCK_AFTER = 25U;
int g_failures = 0;

uint64_t MaskFor(const uint8_t square) {
    return 1ULL << square;
}

void Check(const bool condition, const char* const message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

void Observe(runtime::InteractionDispatcher& dispatcher,
             uint32_t& sequence,
             const uint64_t before,
             const uint64_t after) {
    dispatcher.DispatchBoardObservation(
        runtime::BoardObservationEvent{
            fxe::Millis(), sequence++, before ^ after, after});
}

uint64_t CommitQuietDuckTurn(Harness& harness) {
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | MaskFor(DUCK_BEFORE));
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
    fxe::ManualMillis::AdvanceMs(750U);
    harness.coordinator.Update();

    uint64_t physical =
        INITIAL_OCCUPANCY | MaskFor(DUCK_BEFORE);
    physical &= ~MaskFor(52U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(52U), physical);
    harness.coordinator.Update();
    physical |= MaskFor(36U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(36U), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();

    physical &= ~MaskFor(DUCK_BEFORE);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_BEFORE), physical);
    physical |= MaskFor(DUCK_AFTER);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_AFTER), physical);
    harness.coordinator.Update();
    return physical;
}

void TestDuckChessPieceBounceCannotStartInvisibleTakeback() {
    fxe::ManualMillis::SetNowMs(4500U);
    Harness harness("build/test_duck_coord_grace_takeback.json");
    Check(harness.LoadFresh(),
          "Duck grace-takeback harness initializes");
    uint64_t physical = CommitQuietDuckTurn(harness);
    runtime::InteractionDispatcher dispatcher(
        harness.stateMachine, harness.coordinator);
    uint32_t sequence =
        harness.stateMachine.GetLastBoardObservationSequence() + 1U;

    const uint64_t chessLifted = physical & ~MaskFor(36U);
    Observe(dispatcher, sequence, physical, chessLifted);
    Check(harness.stateMachine.HasHumanIllegal() &&
              !harness.stateMachine.IsUndoPromptActive() &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U,
          "Duck destination bounce becomes correction without silently starting Undo");
    Observe(dispatcher, sequence, chessLifted, physical);
    Check(!harness.stateMachine.HasHumanIllegal() &&
              !harness.stateMachine.IsUndoPromptActive() &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.stateMachine.GetExpectedOccupancyMask() == physical,
          "restoring the bounced piece resumes the committed Duck position");
}

void TestGraceTakebackKeepsDuckPlacementTimeCharged() {
    fxe::ManualMillis::SetNowMs(5000U);
    Harness harness("build/test_duck_coord_grace_clock.json");
    Check(harness.settings.Load(),
          "Duck grace clock settings load");
    harness.settings.Doc()["VS_CPU_LEVEL"] = 0U;
    harness.settings.Doc()["TIMER"] = 2U;
    harness.settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    Check(harness.coordinator.LoadOrInitialize(),
          "Duck grace clock harness initializes");
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | MaskFor(DUCK_BEFORE));
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
    fxe::ManualMillis::AdvanceMs(750U);
    harness.coordinator.Update();

    uint64_t physical =
        INITIAL_OCCUPANCY | MaskFor(DUCK_BEFORE);
    physical &= ~MaskFor(52U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(52U), physical);
    harness.coordinator.Update();
    physical |= MaskFor(36U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(36U), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();

    const runtime::GameClock& clock =
        harness.coordinator.GetGameClock();
    const uint32_t afterChess =
        clock.GetRemainingMs(runtime::ClockSide::White);
    fxe::ManualMillis::AdvanceMs(500U);
    harness.coordinator.Update();
    const uint32_t beforeCombinedCommit =
        clock.GetRemainingMs(runtime::ClockSide::White);
    const uint32_t beforePlacementMs = fxe::Millis();
    Check(beforeCombinedCommit < afterChess,
          "Duck grace clock charges mandatory relocation time");

    physical &= ~MaskFor(DUCK_BEFORE);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_BEFORE), physical);
    physical |= MaskFor(DUCK_AFTER);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_AFTER), physical);
    harness.coordinator.Update();

    const uint32_t confirmedClockBoundary = beforeCombinedCommit -
        (fxe::Millis() - beforePlacementMs);

    runtime::InteractionDispatcher dispatcher(
        harness.stateMachine, harness.coordinator);
    uint32_t sequence =
        harness.stateMachine.GetLastBoardObservationSequence() + 1U;
    const uint64_t duckLifted = physical & ~MaskFor(DUCK_AFTER);
    Observe(dispatcher, sequence, physical, duckLifted);
    const uint64_t duckRestored = duckLifted | MaskFor(DUCK_BEFORE);
    Observe(dispatcher, sequence, duckLifted, duckRestored);
    harness.coordinator.Update();

    const uint64_t chessLifted = duckRestored & ~MaskFor(36U);
    Observe(dispatcher, sequence, duckRestored, chessLifted);
    const uint64_t fullyRestored = chessLifted | MaskFor(52U);
    Observe(dispatcher, sequence, chessLifted, fullyRestored);
    harness.coordinator.Update();

    Check(clock.GetActiveSide() == runtime::ClockSide::White &&
              clock.GetRemainingMs(runtime::ClockSide::White) ==
                  confirmedClockBoundary,
          "Duck grace inverse preserves time charged through Duck confirmation");
}

} // namespace

int RunDuckCoordinatorTakebackGraceTests() {
    TestDuckChessPieceBounceCannotStartInvisibleTakeback();
    TestGraceTakebackKeepsDuckPlacementTimeCharged();
    return g_failures;
}
