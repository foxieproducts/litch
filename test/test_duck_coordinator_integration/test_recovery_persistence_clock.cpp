#include <cstdint>
#include <cstdio>
#include <cstring>

#include "duck_coordinator_test_support.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/game_persistence.hpp"
#include "runtime/chess_interaction_persistence.hpp"

namespace {

using duck_coordinator_test::Harness;
using duck_coordinator_test::INITIAL_OCCUPANCY;

constexpr uint8_t DUCK_FROM = 24U;
constexpr uint8_t DUCK_TO = 25U;
constexpr uint32_t TIMER_TWO_BASE_MS = 180000U;
constexpr uint32_t TIMER_TWO_INCREMENT_MS = 2000U;
constexpr char E2_E4_FEN[] =
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/"
    "RNBQKBNR b KQkq e3 0 1";

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

uint64_t MaskFor(const uint8_t square) {
    return 1ULL << square;
}

void LatchDuck(Harness& harness) {
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | MaskFor(DUCK_FROM));
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
}

bool LoadFreshWithTimer(Harness& harness, const uint32_t timerSetting) {
    if (!harness.settings.Load()) {
        return false;
    }
    harness.settings.Doc()["VS_CPU_LEVEL"] = 0U;
    harness.settings.Doc()["TIMER"] = timerSetting;
    return harness.coordinator.LoadOrInitialize();
}

uint64_t StageE2E4(Harness& harness) {
    uint64_t physical = INITIAL_OCCUPANCY | MaskFor(DUCK_FROM);
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
    return physical;
}

bool StartPendingDuckTurn(Harness& harness,
                          uint64_t& outPhysical,
                          const uint32_t timerSetting = 0U) {
    if (!LoadFreshWithTimer(harness, timerSetting)) {
        return false;
    }
    LatchDuck(harness);
    if (timerSetting != 0U) {
        fxe::ManualMillis::AdvanceMs(750U);
        harness.coordinator.Update();
    }
    outPhysical = StageE2E4(harness);
    return harness.coordinator.GetGameVariant() ==
               modules::GameVariant::Duck &&
           harness.coordinator.GetDuckTurnPhase() ==
               modules::DuckTurnPhase::DuckRelocation;
}

void TestDuckActivationCancelsQueuedOpeningCpuMove() {
    fxe::ManualMillis::SetNowMs(2500U);
    Harness harness("build/test_duck_coord_cancel_opening_cpu.json");
    Check(harness.LoadFresh(), "opening CPU cancel harness initializes");
    harness.settings.Doc()["VS_CPU_LEVEL"] = 5U;

    Check(harness.stateMachine.QueueCpuMove(52U, 36U, false, 36U, '\0'),
          "fixture queues an opening CPU move while variant is undecided");
    Check(harness.coordinator.GetGameVariant() ==
              modules::GameVariant::Undecided,
          "queued opening CPU move does not lock Classic");

    constexpr uint8_t DUCK_D6 = 19U;
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | MaskFor(DUCK_D6));
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();

    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Duck,
          "valid duck still activates while an opening CPU move is queued");
    Check(!harness.stateMachine.HasQueuedCpuMove(),
          "Duck activation cancels the queued opening CPU move");
    Check(harness.settings.ReadValue("VS_CPU_LEVEL", 99U) == 5U,
          "Duck activation preserves VS-CPU after canceling stale opening work");
}

void TestCompletedClassicLatchPersists() {
    fxe::ManualMillis::SetNowMs(3000U);
    static constexpr char PATH[] =
        "build/test_duck_coord_classic_latch_reboot.json";
    Harness before(PATH);
    Check(before.LoadFresh(), "Classic reboot harness initializes");
    before.stateMachine.ApplyPhysicalOccupancySnapshot(INITIAL_OCCUPANCY);
    before.stateMachine.HandleSensorChange(52U, false);
    before.stateMachine.HandleSensorChange(36U, true);
    fxe::ManualMillis::AdvanceMs(10U);
    before.stateMachine.Tick();
    before.coordinator.Update();
    Check(before.coordinator.GetGameVariant() ==
              modules::GameVariant::Classic,
          "first completed move latches Classic before reboot");
    Check(before.coordinator.FlushToSettingsNow(),
          "Classic decision flushes at the simulated reboot boundary");

    Harness after(PATH, false);
    Check(after.LoadExisting(), "Classic decision and first move restore");
    Check(after.coordinator.GetGameVariant() ==
                  modules::GameVariant::Classic &&
              after.coordinator.GetDuckSquare() ==
                  modules::INVALID_DUCK_SQUARE,
          "reboot cannot reclassify a persisted completed Classic move");
}

void LiftDuck(Harness& harness, uint64_t& physical) {
    physical &= ~MaskFor(DUCK_FROM);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_FROM), physical);
}

uint32_t PlaceDuck(Harness& harness, uint64_t& physical) {
    physical |= MaskFor(DUCK_TO);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_TO), physical);
    return harness.stateMachine.GetChessInteractionModel()
        .phase.pendingActionToken;
}

bool SaveCurrentInteraction(Harness& harness) {
    char encoded[128] = {};
    if (!runtime::EncodeChessInteractionDurableState(
            harness.stateMachine.GetChessInteractionModel(),
            encoded, sizeof(encoded)) ||
        !harness.settings.Doc()[modules::KEY_GAME_INTERACTION_V1]
             .set(encoded)) {
        return false;
    }
    return harness.settings.SaveNow();
}

bool ReplaceVariantWithNoPendingTurn(Harness& harness) {
    modules::DuckGameDurableState durable;
    durable.variant = modules::GameVariant::Duck;
    durable.duckSquare = DUCK_FROM;
    char encoded[32] = {};
    if (!modules::EncodeDuckGameDurableState(
            durable, encoded, sizeof(encoded)) ||
        !harness.settings.Doc()[modules::KEY_GAME_VARIANT_V1]
             .set(encoded)) {
        return false;
    }
    return harness.settings.SaveNow();
}

bool HasRestoredPendingBase(const Harness& harness) {
    return harness.coordinator.GetGameVariant() ==
               modules::GameVariant::Duck &&
           harness.coordinator.GetDuckTurnPhase() ==
               modules::DuckTurnPhase::DuckRelocation &&
           std::strcmp(harness.engine.Fen(), modules::INITIAL_FEN) == 0 &&
           harness.coordinator.GetUndoHistoryMoveCount() == 0U;
}

void CheckCommittedExactlyOnce(Harness& harness) {
    Check(std::strcmp(harness.engine.Fen(), E2_E4_FEN) == 0 &&
              harness.coordinator.GetDuckSquare() == DUCK_TO &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U,
          "restored staged turn commits once after Duck relocation");
    harness.coordinator.Update();
    Check(std::strcmp(harness.engine.Fen(), E2_E4_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U,
          "restored staged turn cannot commit a second time");
}

void TestRebootRestoresDuckInHand() {
    static constexpr char PATH[] =
        "build/test_duck_coord_reboot_in_hand.json";
    fxe::ManualMillis::SetNowMs(40000U);
    Harness before(PATH);
    uint64_t physical = 0ULL;
    const bool started = StartPendingDuckTurn(before, physical);
    Check(started, "in-hand reboot fixture starts pending Duck turn");
    if (!started) {
        return;
    }
    LiftDuck(before, physical);
    Check(before.coordinator.FlushToSettingsNow(),
          "in-hand Duck relocation flushes durably");

    Harness after(PATH, false);
    Check(after.LoadExisting() && HasRestoredPendingBase(after),
          "new coordinator restores pending Duck turn with Duck in hand");
    const auto& phase = after.stateMachine.GetChessInteractionModel().phase;
    Check(phase.tag == runtime::ChessInteractionPhase::DuckRelocation &&
              phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary &&
              phase.plan.progress.relocationsLiftedMask == 0x01U,
          "in-hand reboot preserves relocation progress and next action");

    after.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    (void)PlaceDuck(after, physical);
    after.coordinator.Update();
    CheckCommittedExactlyOnce(after);
}

void TestRebootRestoresFreshLatchedDuck() {
    static constexpr char PATH[] =
        "build/test_duck_coord_reboot_fresh_latch.json";
    fxe::ManualMillis::SetNowMs(39500U);
    Harness before(PATH);
    Check(before.LoadFresh(), "fresh-latch reboot fixture initializes");
    before.settings.Doc()["VS_CPU_LEVEL"] = 7U;
    LatchDuck(before);
    Check(before.coordinator.GetGameVariant() ==
                  modules::GameVariant::Duck &&
              before.coordinator.GetDuckSquare() == DUCK_FROM,
          "fresh Duck setup latches before reboot");
    Check(before.coordinator.FlushToSettingsNow(),
          "fresh Duck snapshot flushes at the simulated reboot boundary");

    Harness after(PATH, false);
    Check(after.LoadExisting(), "fresh latched Duck snapshot reloads");
    const uint64_t physical = INITIAL_OCCUPANCY | MaskFor(DUCK_FROM);
    after.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    after.coordinator.Update();
    Check(after.coordinator.GetGameVariant() ==
                  modules::GameVariant::Duck &&
              after.coordinator.GetDuckSquare() == DUCK_FROM,
          "fresh latched Duck decision and square survive reboot");
    Check(after.stateMachine.GetExpectedOccupancyMask() == physical &&
              !after.stateMachine.HasHumanIllegal() &&
              !after.stateMachine.HasCpuIllegal(),
          "restored fresh duck is expected instead of invalid");
    Check(after.engine.thinkStartCalls == 0U &&
              after.settings.ReadValue("VS_CPU_LEVEL", 99U) == 7U,
          "restored Duck preserves CPU level and avoids orthodox search");
}

void TestFreshPersistedClassicReopensVariantSelection() {
    static constexpr char PATH[] =
        "build/test_duck_coord_fresh_classic_migration.json";
    fxe::ManualMillis::SetNowMs(39750U);
    Harness before(PATH);
    Check(before.settings.Load(), "fresh Classic migration fixture loads");
    modules::GamePersistenceState state;
    Check(state.fen.Assign(modules::INITIAL_FEN) &&
              state.undoBaseFen.Assign(modules::INITIAL_FEN),
          "fresh Classic migration fixture stores initial FEN");
    modules::DuckGameDurableState durable;
    durable.variant = modules::GameVariant::Classic;
    char encoded[32] = {};
    Check(modules::EncodeDuckGameDurableState(
              durable, encoded, sizeof(encoded)) &&
              state.variantState.Assign(encoded) &&
              modules::WriteGamePersistenceState(
                  before.settings.Doc(), state) &&
              before.settings.SaveNow(),
          "fresh Classic migration fixture persists");

    Harness after(PATH, false);
    Check(after.LoadExisting(), "fresh persisted Classic snapshot reloads");
    Check(after.coordinator.GetGameVariant() ==
              modules::GameVariant::Undecided,
          "fresh Classic snapshot reopens selection before any completed move");
    after.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | MaskFor(DUCK_FROM));
    fxe::ManualMillis::AdvanceMs(200U);
    after.coordinator.Update();
    Check(after.coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              after.coordinator.GetDuckSquare() == DUCK_FROM,
          "fresh migrated snapshot accepts a valid initial Duck");
}

void TestRebootRestoresDuckAwaitingAction() {
    static constexpr char PATH[] =
        "build/test_duck_coord_reboot_awaiting_action.json";
    fxe::ManualMillis::SetNowMs(41000U);
    Harness before(PATH);
    uint64_t physical = 0ULL;
    const bool started = StartPendingDuckTurn(before, physical);
    Check(started, "action-ready reboot fixture starts pending Duck turn");
    if (!started) {
        return;
    }
    LiftDuck(before, physical);
    const uint32_t token = PlaceDuck(before, physical);
    Check(token != 0U && SaveCurrentInteraction(before),
          "action-ready Duck relocation stores its durable token");

    Harness after(PATH, false);
    Check(after.LoadExisting() && HasRestoredPendingBase(after),
          "new coordinator restores action-ready pending Duck turn");
    const auto& phase = after.stateMachine.GetChessInteractionModel().phase;
    Check(phase.tag == runtime::ChessInteractionPhase::AwaitingAction &&
              phase.pendingActionToken == token &&
              phase.plan.progress.relocationsPlacedMask == 0x01U,
          "action-ready reboot preserves exact token and placement progress");

    after.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    after.coordinator.Update();
    CheckCommittedExactlyOnce(after);
}

void TestRebootRestoresDuckActionCorrection() {
    static constexpr char PATH[] =
        "build/test_duck_coord_reboot_action_correction.json";
    fxe::ManualMillis::SetNowMs(42000U);
    Harness before(PATH);
    uint64_t physical = 0ULL;
    const bool started = StartPendingDuckTurn(before, physical);
    Check(started, "correction reboot fixture starts pending Duck turn");
    if (!started) {
        return;
    }
    LiftDuck(before, physical);
    const uint32_t token = PlaceDuck(before, physical);
    before.stateMachine.HandleInteractionActionResult(token, false);
    const auto& correction =
        before.stateMachine.GetChessInteractionModel().phase;
    Check(correction.tag == runtime::ChessInteractionPhase::DuckRelocation &&
              correction.plan.kind ==
                  runtime::PhysicalTransitionKind::DuckRelocation &&
              correction.requiredAction ==
                  runtime::RequiredPhysicalAction::CorrectBoard &&
              before.coordinator.FlushToSettingsNow(),
          "pending Duck action rollback correction flushes with its variant half");

    Harness after(PATH, false);
    Check(after.LoadExisting() && HasRestoredPendingBase(after),
          "valid pending turn plus Duck Correction restores");
    const auto& restored =
        after.stateMachine.GetChessInteractionModel().phase;
    Check(restored.tag == runtime::ChessInteractionPhase::DuckRelocation &&
              restored.plan.kind ==
                  runtime::PhysicalTransitionKind::DuckRelocation &&
              restored.requiredAction ==
                  runtime::RequiredPhysicalAction::CorrectBoard &&
              restored.correctionMask ==
                  (MaskFor(DUCK_FROM) | MaskFor(DUCK_TO)),
          "Duck rollback correction reboot preserves mask and relocation plan");

    physical &= ~MaskFor(DUCK_TO);
    physical |= MaskFor(DUCK_FROM);
    after.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    after.coordinator.Update();
    const auto& restarted =
        after.stateMachine.GetChessInteractionModel().phase;
    Check(restarted.tag ==
                  runtime::ChessInteractionPhase::DuckRelocation &&
              restarted.requiredAction ==
                  runtime::RequiredPhysicalAction::LiftPrimary &&
              HasRestoredPendingBase(after),
          "corrected rollback restarts the mandatory Duck relocation");

    LiftDuck(after, physical);
    (void)PlaceDuck(after, physical);
    after.coordinator.Update();
    CheckCommittedExactlyOnce(after);
}

void TestRebootRestoresDuckSettlingCorrection() {
    static constexpr char PATH[] =
        "build/test_duck_coord_reboot_settling_correction.json";
    fxe::ManualMillis::SetNowMs(42500U);
    Harness before(PATH);
    uint64_t physical = 0ULL;
    const bool started = StartPendingDuckTurn(before, physical);
    Check(started, "settling-correction reboot fixture starts pending Duck turn");
    if (!started) {
        return;
    }

    LiftDuck(before, physical);
    physical |= MaskFor(DUCK_TO);
    before.stateMachine.HandleBoardObservation(
        MaskFor(DUCK_TO), physical, fxe::Millis(),
        before.stateMachine.GetLastBoardObservationSequence() + 1U);
    Check(before.stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Settling,
          "selectable Duck placement is still pre-commit at reboot fixture");

    const uint8_t replacementSquare = DUCK_TO + 1U;
    physical &= ~MaskFor(DUCK_TO);
    physical |= MaskFor(replacementSquare);
    before.stateMachine.HandleBoardObservation(
        MaskFor(DUCK_TO) | MaskFor(replacementSquare), physical,
        fxe::Millis(),
        before.stateMachine.GetLastBoardObservationSequence() + 1U);
    const auto& correction =
        before.stateMachine.GetChessInteractionModel().phase;
    Check(correction.tag == runtime::ChessInteractionPhase::Correction &&
              correction.plan.kind ==
                  runtime::PhysicalTransitionKind::DuckRelocation &&
              correction.plan.source ==
                  runtime::PhysicalTransitionSource::Human &&
              correction.selectedSquare ==
                  runtime::CHESS_INTERACTION_INVALID_SQUARE &&
              before.coordinator.FlushToSettingsNow(),
          "plan-preserving settling correction flushes with pending Duck turn");

    Harness after(PATH, false);
    Check(after.LoadExisting() && HasRestoredPendingBase(after),
          "plan-preserving Duck settling correction restores after reboot");
    physical &= ~MaskFor(replacementSquare);
    physical |= MaskFor(DUCK_TO);
    after.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    const auto& resumed = after.stateMachine.GetChessInteractionModel().phase;
    Check(resumed.tag == runtime::ChessInteractionPhase::Settling &&
              resumed.plan.kind ==
                  runtime::PhysicalTransitionKind::DuckRelocation &&
              resumed.plan.afterOccupiedMask == physical &&
              !after.stateMachine.HasHumanIllegal(),
          "boot snapshot restored to pending Duck destination resumes settling");
    fxe::ManualMillis::AdvanceMs(10U);
    after.stateMachine.Tick();
    after.coordinator.Update();
    CheckCommittedExactlyOnce(after);
}

void TestRebootFailedDuckActionWithRetainedNextPickupRollsBack() {
    static constexpr char PATH[] =
        "build/test_duck_coord_reboot_failed_action_with_pickup.json";
    fxe::ManualMillis::SetNowMs(42750U);
    Harness before(PATH);
    uint64_t physical = 0ULL;
    const bool started = StartPendingDuckTurn(before, physical);
    Check(started, "failed-action pickup reboot fixture starts pending Duck turn");
    if (!started) {
        return;
    }

    LiftDuck(before, physical);
    const uint32_t token = PlaceDuck(before, physical);
    constexpr uint8_t F7 = 13U;
    physical &= ~MaskFor(F7);
    before.stateMachine.HandleBoardObservation(
        MaskFor(F7), physical, fxe::Millis(),
        before.stateMachine.GetLastBoardObservationSequence() + 1U);
    Check(before.stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction &&
              before.stateMachine.GetChessInteractionModel()
                      .phase.pendingActionToken == token,
          "early P2 pickup remains fenced behind the pending Duck action result");
    before.stateMachine.HandleInteractionActionResult(token, false);
    const auto& rejected =
        before.stateMachine.GetChessInteractionModel().phase;
    Check(rejected.tag == runtime::ChessInteractionPhase::DuckRelocation &&
              rejected.requiredAction ==
                  runtime::RequiredPhysicalAction::CorrectBoard &&
              before.coordinator.FlushToSettingsNow(),
          "failed selectable Duck action persists as explicit rollback correction");

    Harness after(PATH, false);
    Check(after.LoadExisting() && HasRestoredPendingBase(after),
          "failed selectable Duck action and retained pickup restore together");
    after.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    after.coordinator.Update();
    Check(after.stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::DuckRelocation &&
              after.stateMachine.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::CorrectBoard &&
              after.coordinator.GetUndoHistoryMoveCount() == 0U,
          "boot cannot resume or commit the rejected Duck destination");

    physical |= MaskFor(F7);
    after.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    after.coordinator.Update();
    Check(after.stateMachine.GetChessInteractionModel().phase.requiredAction ==
              runtime::RequiredPhysicalAction::CorrectBoard &&
              after.coordinator.GetUndoHistoryMoveCount() == 0U,
          "restoring P2 alone still requires rejected Duck rollback");
    physical &= ~MaskFor(DUCK_TO);
    physical |= MaskFor(DUCK_FROM);
    after.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    after.coordinator.Update();
    Check(after.stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::DuckRelocation &&
              after.stateMachine.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::LiftPrimary &&
              after.coordinator.GetUndoHistoryMoveCount() == 0U &&
              HasRestoredPendingBase(after),
          "complete rollback restarts mandatory Duck relocation without committing");
}

void TestDuckInteractionWithoutPendingVariantFailsClosed() {
    static constexpr char PATH[] =
        "build/test_duck_coord_interaction_without_pending.json";
    fxe::ManualMillis::SetNowMs(43000U);
    Harness before(PATH);
    uint64_t physical = 0ULL;
    const bool started = StartPendingDuckTurn(before, physical);
    Check(started, "missing-pending-half fixture starts Duck relocation");
    if (!started) {
        return;
    }
    Check(ReplaceVariantWithNoPendingTurn(before),
          "fixture removes pending move from only the variant half");

    Harness after(PATH, false);
    Check(!after.LoadExisting(),
          "DuckRelocation interaction without pending variant half fails closed");
}

void TestTimerTwoIncrementOccursOnceAtCombinedCommit() {
    fxe::ManualMillis::SetNowMs(44000U);
    Harness harness("build/test_duck_coord_timer_two_increment.json");
    uint64_t physical = 0ULL;
    const bool started = StartPendingDuckTurn(harness, physical, 2U);
    Check(started, "increment fixture starts timed pending Duck turn");
    if (!started) {
        return;
    }
    const auto& clock = harness.coordinator.GetGameClock();
    const uint32_t beforeIncrement =
        clock.GetRemainingMs(runtime::ClockSide::White);
    Check(clock.IsEnabled() && clock.IsRunning() &&
              clock.GetActiveSide() == runtime::ClockSide::White &&
              beforeIncrement != 0U &&
              beforeIncrement <= TIMER_TWO_BASE_MS,
          "chess phase starts without switching or applying increment");

    LiftDuck(harness, physical);
    harness.coordinator.Update();
    Check(clock.GetActiveSide() == runtime::ClockSide::White &&
              clock.GetRemainingMs(runtime::ClockSide::White) ==
                  beforeIncrement,
          "Duck-in-hand phase does not apply increment");
    const uint32_t beforePlacementMs = fxe::Millis();
    (void)PlaceDuck(harness, physical);
    const uint32_t confirmationElapsedMs = fxe::Millis() - beforePlacementMs;
    Check(clock.GetRemainingMs(runtime::ClockSide::White) ==
              beforeIncrement,
          "action-ready relocation still has no increment");

    harness.coordinator.Update();
    Check(clock.GetActiveSide() == runtime::ClockSide::Black &&
              clock.GetRemainingMs(runtime::ClockSide::White) ==
                  beforeIncrement - confirmationElapsedMs + TIMER_TWO_INCREMENT_MS,
          "combined turn applies one increment only on relocation commit");
    harness.coordinator.Update();
    Check(clock.GetRemainingMs(runtime::ClockSide::White) ==
                  beforeIncrement - confirmationElapsedMs + TIMER_TWO_INCREMENT_MS &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U,
          "later updates cannot apply a second combined-turn increment");
}

void TestUntouchedFreshSetupRetiresStaleTimerRecord() {
    static constexpr char PATH[] =
        "build/test_duck_coord_fresh_stale_timer.json";
    fxe::ManualMillis::SetNowMs(45000U);
    Harness before(PATH);
    Check(LoadFreshWithTimer(before, 3U),
          "stale-timer fixture initializes with the five-minute preset");
    Check(before.coordinator.FlushToSettingsNow(),
          "stale-timer fixture persists its untouched initial setup");
    Check(before.settings.Doc()[modules::KEY_GAME_CLOCK_V1]
              .is<JsonArrayConst>(),
          "stale-timer fixture contains a durable clock record");
    before.settings.Doc()["TIMER"] = 0U;
    Check(before.settings.SaveNow(),
          "stale-timer fixture persists a newer disabled clock setting");

    Harness after(PATH, false);
    Check(after.LoadExisting(),
          "untouched initial setup tolerates a stale full-time clock record");
    Check(!after.coordinator.GetGameClock().IsEnabled(),
          "recovery applies the current disabled clock setting");
    Check(!after.settings.Doc().containsKey(modules::KEY_GAME_CLOCK_V1),
          "recovery retires the stale durable clock record");
}

void TestUntouchedTimedOutSetupPreservesTerminalAcrossNewerTimer() {
    static constexpr char PATH[] =
        "build/test_duck_coord_timed_out_stale_timer.json";
    fxe::ManualMillis::SetNowMs(45500U);
    Harness before(PATH);
    Check(LoadFreshWithTimer(before, 1U),
          "timed-out stale-timer fixture initializes");
    Check(before.coordinator.FlushToSettingsNow(),
          "timed-out stale-timer fixture persists its initial setup");
    JsonArray record =
        before.settings.Doc()[modules::KEY_GAME_CLOCK_V1].as<JsonArray>();
    Check(record.size() == modules::GAME_CLOCK_RECORD_FIELD_COUNT,
          "timed-out stale-timer fixture has a complete clock record");

    // Exact shape captured from a physical S3 that timed out before its first
    // move, then accepted a start-owned TIMER change before reboot.
    record[6] = 19U;  // enabled | started | timed_out
    record[7] = 0U;   // White remains the engine/active side.
    record[8] = 0U;
    record[9] = 51179U;
    record[10] = 0U;  // White timed out.
    before.settings.Doc()["TIMER"] = 0U;
    Check(before.settings.SaveNow(),
          "timed-out fixture persists the newer disabled timer setting");

    Harness after(PATH, false);
    Check(after.LoadExisting(),
          "legacy TIMER mismatch restores the valid stopped terminal clock");
    Check(after.coordinator.HasClockTerminalOutcome() &&
              after.coordinator.GetClockTerminalWinner() == modules::GameWinner::Black &&
              after.coordinator.GetGameClock().IsEnabled() &&
              after.coordinator.GetGameClock().GetRemainingMs(
                  runtime::ClockSide::White) == 0U &&
              after.coordinator.GetGameClock().GetRemainingMs(
                  runtime::ClockSide::Black) == 51179U,
          "terminal result and exact times survive newer disabled TIMER");
    Check(after.settings.ReadValue("TIMER", 99U) == 0U,
          "restore retains the accepted next-game timer preference");
    fxe::ManualMillis::AdvanceMs(3000U);
    after.coordinator.Update();
    Check(after.coordinator.HasClockTerminalOutcome() &&
              after.coordinator.FlushToSettingsNow(),
          "terminal update and actual coordinator flush do not revive the clock");
    modules::GamePersistenceState saved;
    Check(modules::ReadGamePersistenceState(after.settings.Doc(), saved) &&
              saved.clockState.clock.timedOut && saved.clockState.timerSetting == 1U &&
              saved.clockState.clock.remainingMsWhite == 0U &&
              saved.clockState.clock.remainingMsBlack == 51179U,
          "terminal snapshot captures the saved episode preset instead of next-game TIMER");
    Harness again(PATH, false);
    Check(again.LoadExisting() && again.coordinator.HasClockTerminalOutcome(),
          "second save/reboot preserves terminal core without optional presentation");
    Harness explicitLoad(PATH, false);
    Check(explicitLoad.LoadExisting() &&
              explicitLoad.coordinator.HasClockTerminalOutcome(),
          "explicit FEN load starts from a restored terminal episode");
    Check(!explicitLoad.coordinator.LoadExternalFen("bad\nfen") &&
              explicitLoad.coordinator.GetClockTerminalWinner() == modules::GameWinner::Black &&
              explicitLoad.coordinator.GetGameClock().GetRemainingMs(
                  runtime::ClockSide::White) == 0U &&
              std::strcmp(explicitLoad.coordinator.GetPresentationFen(),
                          modules::INITIAL_FEN) == 0,
          "rejected external FEN input preserves the terminal result and presentation");
    Check(explicitLoad.coordinator.LoadExternalFen(modules::INITIAL_FEN) &&
              !explicitLoad.coordinator.HasClockTerminalOutcome() &&
              !explicitLoad.coordinator.GetGameClock().IsEnabled() &&
              !explicitLoad.settings.Doc().containsKey("GAME_CLOCK_TERMINAL_V1"),
          "actual external FEN load retires the old terminal clock and uses newer TIMER");
    Check(again.coordinator.ResetEngine() &&
              !again.coordinator.HasClockTerminalOutcome() &&
              !again.coordinator.GetGameClock().IsEnabled() &&
              again.settings.ReadValue("TIMER", 99U) == 0U,
          "explicit New Game consumes the retained disabled timer preference");
}

void TestElapsedClockWithStaleTimerStillFailsClosed() {
    static constexpr char PATH[] =
        "build/test_duck_coord_elapsed_stale_timer.json";
    fxe::ManualMillis::SetNowMs(46000U);
    Harness before(PATH);
    Check(LoadFreshWithTimer(before, 3U),
          "elapsed stale-timer fixture initializes");
    Check(before.coordinator.FlushToSettingsNow(),
          "elapsed stale-timer fixture persists its initial setup");
    JsonArray record =
        before.settings.Doc()[modules::KEY_GAME_CLOCK_V1].as<JsonArray>();
    Check(record.size() == modules::GAME_CLOCK_RECORD_FIELD_COUNT,
          "elapsed stale-timer fixture has a complete clock record");
    record[8] = 299999U;
    before.settings.Doc()["TIMER"] = 0U;
    Check(before.settings.SaveNow(),
          "elapsed stale-timer fixture persists one lost millisecond");

    Harness after(PATH, false);
    Check(!after.LoadExisting(),
          "elapsed clock with a stale timer setting still fails closed");
    Check(after.settings.Doc().containsKey(modules::KEY_GAME_CLOCK_V1),
          "failed recovery preserves the elapsed durable clock record");
}

} // namespace

int RunDuckCoordinatorPersistenceClockTests() {
    TestDuckActivationCancelsQueuedOpeningCpuMove();
    TestCompletedClassicLatchPersists();
    TestRebootRestoresFreshLatchedDuck();
    TestFreshPersistedClassicReopensVariantSelection();
    TestRebootRestoresDuckInHand();
    TestRebootRestoresDuckAwaitingAction();
    TestRebootRestoresDuckActionCorrection();
    TestRebootRestoresDuckSettlingCorrection();
    TestRebootFailedDuckActionWithRetainedNextPickupRollsBack();
    TestDuckInteractionWithoutPendingVariantFailsClosed();
    TestTimerTwoIncrementOccursOnceAtCombinedCommit();
    TestUntouchedFreshSetupRetiresStaleTimerRecord();
    TestUntouchedTimedOutSetupPreservesTerminalAcrossNewerTimer();
    TestElapsedClockWithStaleTimerStillFailsClosed();
    return g_failures;
}
