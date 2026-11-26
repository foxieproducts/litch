#pragma once

#include "modules/litchcraft_session.hpp"
#include "modules/litchcraft_persistence.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/fixed_string.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/async_think_worker.hpp"
#include "modules/coach_insight.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/cpu_thinking.hpp"
#include "modules/cpu_ponder.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/engine.hpp"
#include "modules/engine_benchmark.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_variant.hpp"
#include "modules/game_variant_state.hpp"
#include "modules/home_assistance_projection.hpp"
#include "modules/settings_store.hpp"
#include "runtime/chess_interaction_persistence.hpp"
#include "runtime/game_clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/ordered_app_event_ingress.hpp"

namespace trace {
class TraceRecorder;
} // namespace trace

namespace runtime {
struct RuntimeHealthSnapshot;
} // namespace runtime

namespace modules {

class EdgeLedCueController;

enum class SetupPresentationEvent : uint8_t {
    None = 0,
    Ready,
};

// Coordinates persistence of game state (FEN, moved squares, undo history) with
// save semantics defined in docs/spec/saved_game_data.md.
//
// Design notes:
// - Avoids expensive engine polling except while the state machine is Idle and
//   not in placement mode.
// - Runtime game changes update the live snapshot and share the settings
//   store's trailing save debounce.
// - External FEN loads update the snapshot and clear undo history.
class GamePersistenceCoordinator final {
  public:
    using GameHistoryIdentityGenerator = bool (*)(
        void* context, game_history::GameId& outIdentity);
    enum class ReplacementKind : uint8_t { NewGame, ExternalFen };
    using ReplacementAdmissionCallback = ControlCoordinatorResult (*)(
        void* context, const GameHistoryPersistenceState& current,
        ReplacementKind kind);

    // Durable-mode wiring supplies the worker's exact published authority.
    // Admission precedes cancellation and every live replacement mutation.
    void SetReplacementAdmissionCallback(ReplacementAdmissionCallback callback,
                                         void* context) {
        m_replacementAdmissionCallback = callback;
        m_replacementAdmissionContext = context;
    }

    GamePersistenceCoordinator(SettingsStore& settings,
                               IEngine& engine,
                               runtime::GameStateMachine& stateMachine,
                               runtime::AppEventQueue* eventQueue = nullptr,
                               AsyncThinkWorker* asyncThinkWorker = nullptr)
        : m_litchcraftPersistence(settings, m_litchcraft),
          m_settings(settings),
          m_engine(engine),
          m_stateMachine(stateMachine),
          m_eventIngress(stateMachine, eventQueue),
          m_asyncThinkWorkerOverride(asyncThinkWorker),
          m_gameClock() {}

    // Synchronous console recovery borrows only the transient snapshot candidate.
    // A concurrent game snapshot or recovery fails without changing settings.
    bool LoadSettingsForRecovery();

    void SetTraceRecorder(trace::TraceRecorder* recorder) { m_traceRecorder = recorder; }

    void SetInteractionEventsExternalized(const bool externalized) {
        m_interactionEventsExternalized = externalized;
        if (!externalized) {
            m_externalizedDuckChessActionPending = false;
            m_externalizedDuckRelocationActionPending = false;
            m_externalizedDuckUndoActionPending = false;
            m_externalizedDuckRedoActionPending = false;
            m_externalizedDuckFixedActionValid = false;
            m_externalizedDuckRedoStartPending = false;
            m_externalizedInitialDuckRelocationActionPending = false;
        }
    }
    bool AreInteractionEventsExternalized() const {
        return m_interactionEventsExternalized;
    }
    // Trace replay resumes coordinator-owned Duck work only after the
    // recorded action-result checkpoint validates the reducer boundary.
    bool ResumeExternalizedDuckActionAfterCheckpoint();
    // Cancel-undo restores the engine position before its recorded position
    // acknowledgement. Replay begins the fixed Duck redo only after that
    // acknowledgement has rebuilt the reducer's expected occupancy and the
    // forward chess pieces have been physically restored. Native Update also
    // retries this same pending owner while ordinary correction completes.
    bool ResumeExternalizedDuckRedoAfterPositionAcknowledged();

    // Sets the base RNG seed used by any coordinator-owned randomness (e.g.
    // VS-CPU move tie-breaking). This should be stable for trace/replay.
    void SetRngSeed(uint32_t rngSeed);
    void SetGameHistoryIdentityGenerator(
        GameHistoryIdentityGenerator generator, void* context = nullptr) {
        m_gameHistoryIdentityGenerator = generator;
        m_gameHistoryIdentityContext = context;
    }
    const GameHistoryPersistenceState& GetGameHistoryPersistenceState() const {
        return m_gameHistoryPersistence;
    }

    // Loads persisted state into the engine/state machine if present.
    // If no FEN is present, resets engine and writes an initial snapshot.
    bool LoadOrInitialize();

    // Best-effort: write the current engine/state-machine snapshot back into
    // the settings document and explicitly flush the queued save.
    //
    // Used for reboot/sleep paths to avoid losing GAME_* keys if another
    // subsystem has mutated the settings doc since the last persisted snapshot.
    bool FlushToSettingsNow();
    bool PrepareSettingsSnapshotForLifecycle();

    // Periodic update:
    // - Consumes committed moves and queues a debounced snapshot save.
    // - Polls external FEN changes only while idle (and not in placement).
    void Update();

    // Lightweight helper: sync the state-machine's valid-destination mask
    // against engine legality while a human move is in progress.
    //
    // This is intentionally separate from Update() so sensor-cadence code can
    // enforce legality without waiting for the slower engine cadence.
    void UpdateLegalDestinationsMask();

    // External engine mutation: set engine to the given FEN, abandon in-flight
    // state, clear undo history, and queue a snapshot save.
    bool LoadExternalFen(const char* fen);

    // External engine mutation: reset engine to initial position, abandon
    // in-flight state, clear undo history, and queue a snapshot save. Returns
    // false without mutating the game while async cancellation is pending.
    bool ResetEngine();

    // Semantic controls share the same ordered ingress as board, button, and
    // adapter events. The coordinator owns policies which cross engine and
    // interaction state.
    bool RequestUndo(uint8_t moveCount);
    bool HasUndoHistory() const;
    uint8_t GetUndoHistoryMoveCount() const;
    bool IsCancelUndoAvailable() const;
    bool IsUndoActionPending() const;
    bool IsDuckUndoRelocationPending() const { return m_duckUndoRelocationPending; }
    bool HasPendingUndoCommand() const;
    void RequestHintToggle();

    // Typed control-mutation seam. These methods preserve the existing ordered
    // reducer ingress while exposing stable policy results to transports.
    ControlCoordinatorResult CheckControlStartOnlyGate(
        const char* canonicalKey) const;
    ControlCoordinatorResult AdmitControlStartOnlyMutation(const char* canonicalKey);
    // One coalesced presentation event from authoritative rejection, consumed
    // by the frame owner. Reads and replayed transport receipts never emit it.
    void PresentPendingOccupiedBoardFeedback(EdgeLedCueController& edgeCue);
    ControlCoordinatorResult RequestControlHint(uint16_t requestId);
    ControlCoordinatorResult RequestControlUndo(uint16_t requestId,
                                                uint8_t moveCount);
    ControlCoordinatorResult RequestControlNewGame(uint16_t requestId);
    ControlCoordinatorResult PollControlAction(uint16_t requestId);
    void RequestCancelCpuWork();
    void RequestCancelHintWorkAndRestart();
    // Coaching reuses the ordinary Hint pipeline but requests a stronger
    // bounded search for the active human owner.
    void SetCoachingHintActive(bool active);

    // Requests that any in-progress engine thinking (VS-CPU or hint) be
    // canceled. This is intentionally non-blocking; cancellation is completed
    // during the next Update() tick.
    //
    // Also clears any render-facing "thinking" state immediately so UI stops
    // showing CPU-busy animations on the same frame.
    void RequestCancelAllThinking();

    const EngineDerivedState& GetEngineDerivedState() const { return m_engineDerived; }

    SettingsStore& GetSettings() { return m_settings; }
    const SettingsStore& GetSettings() const { return m_settings; }

    const runtime::GameClock& GetGameClock() const { return m_gameClock; }
    uint32_t GetClockTimerSetting() const {
        // A legacy separate setting write can choose the next game's preset
        // while the current terminal episode retains its durable clock.
        return (HasClockTerminalOutcome() || HasResignationOutcome()) ? m_cachedTimerSetting
                                         : ReadTimerSetting();
    }
    // Runtime health is an additive clock pause reason. Denial pauses
    // immediately at the fact boundary; recovery is reconciled with every
    // other pause reason by the normal coordinator update.
    void SetRuntimeHealthSnapshot(
        const runtime::RuntimeHealthSnapshot& runtimeHealth);
    // Board-owned settings pause the casual clock without changing whose turn
    // it is. Closing the surface lets the normal pause-reason reducer resume.
    void SetBoardSettingsActive(bool active);
    LitchcraftSession& Litchcraft() { return m_litchcraft; }
    const LitchcraftSession& Litchcraft() const { return m_litchcraft; }
    LitchcraftPersistence& ArtPersistence() { return m_litchcraftPersistence; }
    const LitchcraftPersistence& ArtPersistence() const { return m_litchcraftPersistence; }
    struct LitchcraftPresentationAdmission final {
        void* context = nullptr;
        uint32_t (*begin)(void*, LitchcraftSession::PresentationAccess) = nullptr;
        void (*end)(void*, uint32_t) = nullptr;
    };
    void SetLitchcraftPresentationAdmission(LitchcraftPresentationAdmission admission) {
        m_litchcraftPresentationAdmission = admission;
    }
    // The external ingress/batch owner pairs one successful begin with End.
    // Domain wrappers reuse an already-owned admission without acquiring again.
    bool TryBeginLitchcraftPresentation(LitchcraftSession::PresentationAccess access);
    bool TryBeginLitchcraftOrdered(uint8_t opcode, const uint8_t* bytes, size_t size,
                                  bool runtimeEligible = true);
    bool TryBeginLitchcraftPacket(const uint8_t* bytes, size_t size);
    void EndLitchcraftPresentation();
    class LitchcraftPresentationGuard final {
      public:
        explicit LitchcraftPresentationGuard(GamePersistenceCoordinator* owner)
            : m_owner(owner), m_lease(owner == nullptr ? 0U : owner->m_litchcraftPresentationLease) {}
        explicit LitchcraftPresentationGuard(GamePersistenceCoordinator& owner)
            : LitchcraftPresentationGuard(&owner) {}
        ~LitchcraftPresentationGuard() {
            if (m_owner != nullptr) m_owner->EndLitchcraftPresentation(m_lease);
        }
        LitchcraftPresentationGuard(const LitchcraftPresentationGuard&) = delete;
        LitchcraftPresentationGuard& operator=(const LitchcraftPresentationGuard&) = delete;
      private:
        GamePersistenceCoordinator* m_owner;
        uint32_t m_lease;
    };
    bool CanEnterLitchcraft() const;
    bool CanEnterLitchcraftFromBoardControls() const;
    bool IsLitchcraftEntryBlockedByOccupiedBoard() const;
    void HandleLitchcraftPacket(const uint8_t* bytes, size_t size,
                               bool runtimeEligible = true);
    LitchcraftSession::Status HandleLitchcraftConnectionMode(
        const uint8_t* bytes, size_t size, bool runtimeEligible = true);
    void HandleLitchcraftBulkPacket(const uint8_t* bytes, size_t size,
                                   bool runtimeEligible = true);
    LitchcraftSession::Status HandleLitchcraftConnectionOrdered(
        uint8_t opcode, const uint8_t* bytes, size_t size,
        bool runtimeEligible = true, uint32_t connectionEpoch = 0U);
    void UpdateLitchcraft();
    void ObserveLitchcraftButton(const ButtonEvent& event);
    void ObserveLitchcraftButton(const ButtonEvent& event,
                                 uint32_t eventTimeMs);
    LitchcraftSession::ModeChordResult ObserveLitchcraftEntryButton(
        const ButtonEvent& event, const uint32_t eventTimeMs) {
        StopBenchmark();
        return m_litchcraft.ObserveEntryButton(
            event, eventTimeMs, CanEnterLitchcraftFromBoardControls(),
            IsLitchcraftEntryBlockedByOccupiedBoard(),
            m_stateMachine.GetPhysicalOccupancyMask());
    }

    bool IsRestoredClockResumeAuthorizationPending() const {
        return m_restoredClockResumeAuthorizationPending;
    }
    // A restored live clock remains paused until the boot adapter supplies
    // positive evidence from this boot. Cached occupancy is not evidence.
    bool AuthorizeRestoredClockResumeAfterBoot(
        bool freshSuccessfulFullBoardScan,
        const runtime::RuntimeHealthSnapshot& runtimeHealth);
    // The lift belongs to the acknowledged king, independent of whose turn it is.
    UiOwner GetKingResignationOwner() const { return m_kingResignationOwner; }
    uint8_t GetKingResignationSquare() const { return m_kingResignationSquare; }
    uint32_t GetKingResignationElapsedMs() const {
        return m_kingResignationOwner == UiOwner::None ? 0U
            : static_cast<uint32_t>(fxe::Millis() - m_kingResignationStartedAtMs);
    }
    bool IsKingResignationArmed() const {
        return m_kingResignationOwner != UiOwner::None &&
               GetKingResignationElapsedMs() >= KING_RESIGNATION_HOLD_MS;
    }
    bool HandleKingResignationButton(const ButtonEvent& event, uint32_t eventTimeMs);
    void ObserveFreshAcceptedBoardScan(uint64_t logicalMask);
    void ResetKingResignationInput();
    bool HasResignationOutcome() const {
        return m_variantOutcome.reason == VariantOutcomeReason::Resigned;
    }
    uint32_t GetVariantTerminalStartMs() const { return m_variantTerminalStartMs; }

    bool HasClockTerminalOutcome() const {
        return m_clockTerminalWinner != GameWinner::None;
    }
    GameWinner GetClockTerminalWinner() const {
        return m_clockTerminalWinner;
    }
    uint32_t GetClockTerminalStartMs() const { return m_clockTerminalStartMs; }
    // Terminal presentation may retain the acknowledged Duck chess half. The
    // engine, history and durable GAME_FEN still own only complete turns.
    const char* GetPresentationFen() const {
        return m_clockTerminalFen.Empty() ? m_cachedFen.CStr()
                                         : m_clockTerminalFen.CStr();
    }

    // Borrowed until the next Home projection or coordinator mutation. Home
    // presents committed move numbering while engine/cache identity stays raw.
    const char* GetHomePresentationFen() const;

    GameVariant GetGameVariant() const { return m_gameVariant; }
    uint8_t GetDuckSquare() const { return m_duckSquare; }
    DuckTurnPhase GetDuckTurnPhase() const { return m_duckTurnPhase; }
    VariantOutcome GetVariantOutcome() const { return m_variantOutcome; }
    bool IsDuckGame() const { return m_gameVariant == GameVariant::Duck; }
    GameVariantStateSnapshot GetGameVariantStateSnapshot() const;
    const char* GetAuthoritativeFen() const { return m_cachedFen.CStr(); }

    // Boot scans call this after logical orientation mapping. A true result
    // suppresses ordinary extra-piece placement feedback while the exact
    // initial Duck candidate completes its stability latch.
    bool ShouldDeferInitialPlacementForVariantDetection(
        uint64_t logicalPhysicalOccupancy) const;

    // Sensor ingress uses this before normal chess reduction. Exact initial
    // Duck add/remove snapshots belong to variant validation first, so they
    // cannot transiently become chess correction or illegal feedback.
    bool ShouldValidateInitialVariantTransition(
        uint64_t logicalPhysicalOccupancy) const;
    void MarkInitialVariantValidationPending();
    bool IsInitialVariantValidationPending() const;
    bool IsInitialVariantSelectionReversible() const;
    bool IsInitialDuckRetirementObservation() const;
    // Capture before the boot scan can advance a restored physical plan.
    bool TryGetBootResumeOccupancy(uint64_t& outOccupancy) const;
    bool TryHandleEmptyBoardSetupObservation(
        const runtime::AppEvent& event, uint64_t changedMask, uint64_t occupiedMask);
    bool IsInitialDuckObstructionRelocationPromptActive() const {
        return m_initialDuckObstructionRelocationPending;
    }
    bool TryTakeSetupPresentationEvent(SetupPresentationEvent& outEvent);
    void CancelSetupPresentationEvents();

    // Ordered board ingress gives the coordinator first refusal on a recently
    // committed move's exact physical inverse. Returns true only when the
    // event was consumed as takeback choreography; ordinary move/capture input
    // remains owned by GameStateMachine.
    bool TryHandleTakebackGraceBoardEvent(
        const runtime::AppEvent& event,
        uint64_t changedMask,
        uint64_t occupiedMask);

    // Render-only projection while an exact initial Duck setup completes its
    // stability latch. This never changes or substitutes for the durable duck
    // square, which remains invalid until the variant is actually latched.
    uint8_t GetInitialDuckCandidateSquare() const;

    // Current request requires a successful coherent copy; failure clears out.
    // Prior per-kind quiescent results remain history for the same position.
    // A false result never establishes activity/state; no engine read.
    bool TryGetThinkDiagnostics(ThinkRequestKind kind, ThinkDiagnostics& out) const;
    // Borrowed physical-control authority; an unbound owner fails admission closed.
    void SetBenchmarkControlsIdleProbe(void* context, bool (*probe)(void*)) {
        m_benchmarkControlsContext = context;
        m_benchmarkControlsIdle = probe;
    }
    bool IsBenchmarkActive() const { return m_benchmarkActive; }
    bool StartBenchmark(uint8_t caseId, uint8_t level);
    void StopBenchmark();
    bool TryGetBenchmarkStatus(EngineBenchmarkStatus& out) const;
    bool IsEngineLaneActive() const;


    // Hint diagnostics (console/debug): last completed hint think stats.
    bool TryGetLastHintThinkStats(UciMove& outBestUci,
                                  uint32_t& outElapsedMs,
                                  uint32_t& outNodeCount,
                                  uint8_t& outDifficulty) const;

    // Returns coaching only when it belongs to the current Classic position
    // and the cached Hint came from that same verified engine result.
    bool TryGetCurrentCoachInsight(CoachInsight& outInsight) const;

    // Pure projection from coordinator-owned caches and current reducer state.
    // This never consults the engine, including while an async worker owns it.
    HomeAssistanceProjection BuildHomeAssistanceProjection(
        const HomeAssistanceRuntimeInputs& runtimeInputs) const;

    // Legal, still-present own sources for the current victim-first capture.
    // A stale/missing cache or another phase returns zero; never reads engine.
    uint64_t GetCaptureFirstCapturerMask() const;

    // Cached CPU-owner presentation; never reads the active engine or advances
    // the estimate. Hint and canceled/terminal searches publish no CPU state.
    CpuThinkingPresentation GetCpuThinkingPresentation() const;
    // Read-only diagnostics copied from the speculative worker/cache owner.
    CpuPonderStatus GetPonderStatus() const;

  private:
#if defined(LITCH_NATIVE)
    friend struct DiagnosticSnapshotTestAccess;
#endif
    bool IsLitchcraftEntryContextEligible(bool requireEmptyBoard) const;
    void EndLitchcraftPresentation(uint32_t lease);

    LitchcraftSession m_litchcraft;
    LitchcraftPresentationAdmission m_litchcraftPresentationAdmission{};
    uint32_t m_litchcraftPresentationLease = 0U;
    bool m_occupiedBoardFeedbackPending = false;
    LitchcraftPersistence m_litchcraftPersistence;
    static bool FenToOccupancyMask(const char* fen, uint64_t& outOcc64);

    static bool TryGetFenPieceAtSquare(const char* fen, uint8_t square, char& outPieceChar);

    static uint8_t MaskToSquareListAscending(const uint64_t mask64,
                                             uint8_t* outSquares,
                                             const uint8_t outCapacity);

    static bool MakeUciMove(const uint8_t from,
                            const uint8_t to,
                            const char promotionSuffix,
                            UciMove& outUci);

    // Fully stop the one shared engine search lane and clear only its active
    // projections. Pending hint/retry policy and queued moves remain owned by
    // the calling transition.
    bool QuiesceAllThinking();
    bool UpdatePonder(uint32_t vsCpuLevel, bool firstPlayerIsP2, bool cpuPlayerIsP2);
    void CollectPonderResult();
    void CancelPonder(bool discard);
    bool TryUsePonder(uint32_t selectedLevel, ThinkConfig& config);
    bool IsBenchmarkAdmissionStable() const;
    bool UpdateBenchmark();
    bool UsesAsyncThinkWorker() const;
    AsyncThinkWorker& ThinkWorker();
    const AsyncThinkWorker& ThinkWorker() const;
    void BeginCpuThinking(const ThinkConfig& config);
    void BeginThinkDiagnostics(const ThinkConfig& config, ThinkRequestKind kind);
    void RefreshThinkDiagnostics();
    void RetireThinkDiagnostics(ThinkRequestKind kind);
    void RecordAcceptedHintDiagnostic();
    ThinkConfig MakeCpuThinkConfig(uint32_t vsCpuLevel) const;
    void UpdateCpuThinkingPresentation();

    bool PersistSnapshot(bool clearUndo, bool scheduleSave = true);
    bool PersistSnapshotState(const runtime::GameClockState& clockState,
                              bool clearUndo, bool scheduleSave);

    bool PersistSnapshotDeferredWithRetries(const char* reason,
                                            bool clearUndo);

    void ScheduleSnapshotRetry(const char* reason, bool clearUndo);
    void ClearSnapshotRetry();
    void RetrySnapshotIfPending();

    void UpdateKingQueenSwapPrompt(bool boardRotateEnabled,
                                   bool firstPlayerIsP2,
                                   bool atEitherInitialFen);

    void LogStateTransitions();

    void UpdatePromotionChoiceIfPending();
    bool CommitPromotionMove(const runtime::PhysicalTransitionPlan& promotion,
                             PromotionPiece piece, bool automatic);
    void ConsumeCommittedMoves();
    bool ApplyHumanMoveTransactional(
        const runtime::PhysicalTransitionPlan& move);
    bool StageDuckChessMove(const runtime::PhysicalTransitionPlan& move,
                            char promotionSuffix);
    bool CommitDuckRelocation(
        const runtime::PhysicalTransitionPlan& relocation);
    bool CommitInitialDuckObstructionRelocation(
        const runtime::PhysicalTransitionPlan& relocation);
    bool FinalizeInitialDuckObstructionRelocation(uint8_t fromSquare,
                                                  uint8_t toSquare);
    void UpdateInitialDuckObstructionRelocation();
    bool RetireInitialDuckForSetup(uint8_t replacementCandidate);
    bool CommitStagedDuckTurn(uint8_t duckFrom,
                              uint8_t duckTo,
                              bool duckRelocated);
    bool ContinueStagedDuckTurnAfterChessAction();
    bool BeginStagedDuckChessUndo();
    void HandleDuckUndoRelocationAction(
        const runtime::PhysicalTransitionPlan& relocation);
    void HandleDuckRedoRelocationAction(
        const runtime::PhysicalTransitionPlan& relocation);
    bool BeginDuckCancelUndoRedoRelocation();
    void ContinueDuckUndoRelocationAction(bool valid);
    void ContinueDuckRedoRelocationAction(bool valid, uint8_t duckTo);
    void UpdateGameVariantActivation();
    bool LatchGameVariant(GameVariant variant, uint8_t duckSquare);
    void ResetGameVariantForNewGame();
    void RearmFreshGameAfterCompletedUndo();
    bool RestoreDuckGameState(const GamePersistenceState& persisted,
                              bool loadedFreshStart);
    bool EncodeDuckGameState(GamePersistenceState& out) const;
    bool IsGameOverVariantAware() const;
    void RecordVariantTraceCheckpoint();

    // Returns true if Update() should return immediately.
    bool UpdateVsCpu(uint32_t vsCpuLevel,
                     bool firstPlayerIsP2,
                     bool cpuPlayerIsP2,
                     bool allowVsCpuThinkStart,
                     bool vsCpuThinkMaintainOk,
                     bool cancelAllThinkingThisTick);

    // Returns true if Update() should return immediately.
    bool UpdateHints(uint32_t vsCpuLevel,
                     bool firstPlayerIsP2,
                     bool cpuPlayerIsP2,
                     bool cancelAllThinkingThisTick);

    // Returns true while Duck-specific hint/CPU search consumed this cadence.
    bool UpdateDuckSearch(uint32_t vsCpuLevel,
                          bool firstPlayerIsP2,
                          bool cpuPlayerIsP2,
                          bool cancelAllThinkingThisTick);
    bool IsSearchStableIdle() const;
    bool IsHintSearchStableIdle() const;
    bool CanPublishHint(uint8_t fromSquare, uint8_t toSquare) const;
    bool PollDuckSearch(bool stableIdle);
    bool ApplyCompletedDuckSearchResult(bool wasCpu, bool stableIdle);
    void QueuePendingDuckCpuMoveIfStable(bool stableIdle);
    bool QueueLiveCpuMove(uint8_t from,
                          uint8_t to,
                          bool isCapture,
                          uint8_t captureSquare,
                          char promotionSuffix);
    void ClearPendingCpuMoveSelection();
    void LogMoveSummary(const char* actor,
                        const char* fenBefore,
                        uint8_t fromSquare,
                        uint8_t toSquare) const;
    bool ShowCachedDuckHint();
    bool StartDuckSearchIfReady(bool isCpuTurn,
                                uint32_t vsCpuLevel,
                                bool stableIdle,
                                bool cancelAllThinkingThisTick);

    void PollExternalEngineMutationAndApply();
    bool IsCurrentEngineFenEquivalent(const FenString& nowFen) const;

    void ApplyEngineFenToStateMachineExpected(const char* fen);
    void DispatchInteractionPositionAcknowledged(runtime::AppEvent& position,
                                               const runtime::CaptureLegality& captureLegality);
    void DispatchInteractionPositionAcknowledged(
        const runtime::ChessInteractionModel& model);
    void DispatchPendingInteractionActionResult(bool succeeded);
    void DispatchInteractionAdapterResult(runtime::AppEventType type,
                                          const char* adapter,
                                          bool succeeded);

    void ClearLegalDestinationsMask();

    void ClearCachedLegalDestinations();
    bool BuildCachedLegalDestinationsForCurrentPosition();
    bool TryGetCachedLegalDestinationsMask(const uint8_t fromSquare, uint64_t& outDestMask64) const;

    void ClearCachedHintResults();
    void CacheHintMoveForCurrentPosition(uint8_t fromSquare, uint8_t toSquare);
    void CaptureHintForCancelUndoRestore();
    bool TryGetCachedHintMoveForCurrentPosition(uint8_t& outFromSquare, uint8_t& outToSquare) const;
    void SyncHintMoveForCurrentPosition(bool hintEligible, bool isHumanTurn);
    void ResetHintRetry();
    void ScheduleHintRetry(const char* reason);
    bool IsHintRetryReady();
    bool RecoverEngineAfterFailedThinkStart();
    bool IsCpuRetryReady() const;
    ThinkConfig MakeHintThinkConfig() const;
    void FillChessKnowledgeContext(ThinkConfig& config) const;
    uint8_t NextHintDifficulty() const;
    bool IsCurrentHintAnalysisPosition() const;
    void BeginHintSearch(ThinkConfig& config);
    void PrepareImmediateDecision(const ThinkConfig& config, ThinkRequestKind kind);
    bool RetainImmediateCpuDecision();
    void ConsumeLiveHintCandidate();
    bool CacheLiveDuckHint(const ThinkCandidate& candidate);
    void CaptureDisplayedDuckHintTurn();
    void LogRequestedHint();
    void RecordHintSearchStats(uint32_t elapsedMs, uint32_t nodes);
    bool HasCachedHintForCurrentPosition() const;

    // Undo persistence uses the 7-field schema defined in
    // docs/spec/saved_game_data.md:
    //   (side, movedPiece, capturedPiece, capturedSquare, promotedPiece, from, to)
    // The coordinator is the sole owner of the complete persisted history.
    static constexpr uint16_t UNDO_HISTORY_FIELD_CAPACITY =
        static_cast<uint16_t>(modules::UNDO_HISTORY_FIELDS_PER_MOVE) *
        static_cast<uint16_t>(modules::UNDO_HISTORY_MOVE_CAPACITY);

    struct PendingDuckChessTurn {
        bool valid = false;
        UciMove uci;
        FenString afterFen;
        uint8_t undoFields[UNDO_HISTORY_FIELDS_PER_MOVE] = {};
        uint16_t nextHalfmoveClockPlies = 0U;
        bool capturedKing = false;
        GameWinner winner = GameWinner::None;
        uint8_t requiredDuckDestination = INVALID_DUCK_SQUARE;
        runtime::PhysicalTransitionSource source =
            runtime::PhysicalTransitionSource::Human;
        runtime::DirectedMoveOrigin directedOrigin =
            runtime::DirectedMoveOrigin::Cpu;
    };

    void ClearUndoHistory();
    [[gnu::noinline]] bool RestoreOrInitializeState(
        bool& outPersistSnapshot,
        bool& outClearUndoForSnapshot);
    bool AppendUndoHistoryEntry(uint8_t side,
                                uint8_t movedPiece,
                                uint8_t capturedPiece,
                                uint8_t capturedSquare,
                                uint8_t promotedPiece,
                                uint8_t from,
                                uint8_t to);
    bool CanAppendUndoHistoryEntry() const;
    bool EnsureUndoHistoryAppendCapacity();

    ControlCoordinatorResult CheckReplacementAdmission(ReplacementKind kind) const;
    bool ResetEngineInternal(bool resetKqPromptTracking);
    bool BeginNewGameHistory(bool captureIncomplete = false);
    void RecordGameHistoryPlyMutation(bool appended);
    void RestoreGameHistoryIdentity(const GamePersistenceState& persisted);
    void CaptureGameHistoryPersistenceState(GamePersistenceState& snapshot) const;
    struct ControlDispatchContext {
        GamePersistenceCoordinator& coordinator;
        bool result = false;
    };

    static bool DispatchControlEvent(void* context,
                                     runtime::GameStateMachine& stateMachine,
                                     const runtime::AppEvent& event);
    enum class UndoCommandOrigin : uint8_t {
        Manual = 0,
        GraceTakeback,
    };
    bool ApplyUndoRequest(uint8_t moveCount);
    bool BeginNextUndo(
        UndoCommandOrigin origin = UndoCommandOrigin::Manual);
    void CloseCompletedUndoWindow();
    bool ApplyResetRequest();
    bool PopUndoHistoryEntry(uint8_t outFields[UNDO_HISTORY_FIELDS_PER_MOVE]);
    bool UpdateLastUndoHistoryPromotionPiece(uint8_t promotedPiece);
    bool BeginDuckUndoIfNeeded(
        uint8_t moveCount,
        UndoCommandOrigin origin = UndoCommandOrigin::Manual);
    bool TryPeekLastDuckUndo(uint8_t& outFrom, uint8_t& outTo) const;
    void ClearDuckUndoHistory();
    bool EncodeDuckUndoState(GamePersistenceState& out,
                             uint16_t historyStartMove,
                             uint16_t keepMoves) const;
    bool RestoreDuckUndoState(const GamePersistenceState& persisted);

    SettingsStore& m_settings;
    // Reused candidate keeps snapshot preparation off the loop stack and atomic.
    StaticJsonDocument<fxe::SETTINGS_STORE_JSON_CAPACITY> m_snapshotDocument;
    std::atomic<bool> m_snapshotScratchBusy{false};
    IEngine& m_engine;
    runtime::GameStateMachine& m_stateMachine;
    runtime::OrderedAppEventIngress m_eventIngress;
    GameHistoryIdentityGenerator m_gameHistoryIdentityGenerator = nullptr;
    void* m_gameHistoryIdentityContext = nullptr;
    GameHistoryPersistenceState m_gameHistoryPersistence{};
    // Timed games retain accepted play after final Undo; untimed fresh setup clears it.
    bool m_gameHasAcceptedMove = true;
    bool CanReturnEmptyBoardToSetup(uint64_t occupiedMask) const;
    bool UpdateEmptyBoardSetup();
    ReplacementAdmissionCallback m_replacementAdmissionCallback = nullptr;
    void* m_replacementAdmissionContext = nullptr;

    trace::TraceRecorder* m_traceRecorder = nullptr;
    bool m_interactionEventsExternalized = false;

    void UpdateGameClock();
    void CommitGameClockMove(runtime::ClockSide newActiveSide);
    bool RestoreGameClockFromPersistence(
        const GamePersistenceState& persisted,
        bool initializedNewGame,
        bool untouchedInitialSetup);
    GameClockPersistenceState CaptureGameClockPersistenceState(
        bool newSessionBoundary,
        const runtime::GameClockState& clockState);
    static uint32_t NextClockIdentity(uint32_t current);
    struct ClockPauseReasons {
        bool placement = false;
        bool correction = false;
        bool undo = false;
        bool humanIllegal = false;
        bool cpuIllegal = false;
        bool cpuMoveQueued = false;
        bool occupancy = false;
        bool runtimeHealth = false;
        bool boardSettings = false;

        constexpr bool Any() const {
            return placement || correction || undo || humanIllegal ||
                   cpuIllegal || cpuMoveQueued || occupancy || runtimeHealth ||
                   boardSettings;
        }

        constexpr bool AnyExceptCpuMoveQueued() const {
            return placement || correction || undo || humanIllegal ||
                   cpuIllegal || occupancy || runtimeHealth || boardSettings;
        }
    };
    ClockPauseReasons ComputeClockPauseReasons() const;
    void ClearClockTerminalOutcome();
    bool RestoreClockTerminalPresentation(std::string_view move);
    bool SetClockTerminalPresentation(std::string_view move,
                                      const FenString& afterFen);
    bool ConsumePendingClockTimeout();
    void LatchClockTerminalOutcome(runtime::ClockSide losingSide);
    bool AbandonStagedWorkForTerminalOutcome();
    static constexpr uint32_t KING_RESIGNATION_HOLD_MS = 5000U;
    void UpdateKingResignation();
    void ClearKingResignation();
    void ConfirmKingResignation();
    UiOwner m_kingResignationOwner = UiOwner::None;
    uint8_t m_kingResignationSquare = 255U;
    uint32_t m_kingResignationStartedAtMs = 0U;
    uint32_t m_variantTerminalStartMs = 0U;
    std::array<bool, 2> m_kingResignationCircleHeld{};
    uint8_t m_kingResignationConsumedReleaseMask = 0U;
    UiOwner m_kingResignationPendingOwner = UiOwner::None;
    uint8_t m_kingResignationPendingSquare = 255U;
    uint32_t m_kingResignationPendingLiftMs = 0U;
    void ResumeGameClockAfterTerminalUndo();
    uint32_t ReadTimerSetting() const;
    uint32_t ReadVsCpuLevelSetting() const;
    uint32_t ReadTakebackGraceSeconds() const;
    static runtime::ClockSide ToClockSide(SideToMove side);

    uint32_t m_cachedTimerSetting = UINT32_MAX;
    uint32_t m_clockSessionId = 0U;
    uint32_t m_clockDurableEpoch = 0U;
    bool m_clockSessionStarted = false;
    bool m_pregameClockGraceArmed = false;
    bool m_pregameClockProgressSeen = false;
    uint32_t m_pregameClockProgressStartedAtMs = 0U;
    fxe::ElapsedTime m_pregameClockGraceElapsed;
    bool m_restoredClockResumeAuthorizationPending = false;
    bool m_runtimeHealthClocksAllowed = true;
    bool m_boardSettingsActive = false;

    // Cached engine state used for change detection and persistence.
    FenString m_cachedFen;
    mutable FenString m_homePresentationFen;

    bool m_lastLoggedHumanPieceInHand = false;
    uint8_t m_lastLoggedPickupFromSquare = 255;

    // Cached derived engine state used for overlays.
    EngineDerivedState m_engineDerived;

    // Snapshot reliability: if encoding is temporarily unsafe, keep a pending
    // retry so the latest stable state reaches the shared debounce window.
    bool m_snapshotRetryPending = false;
    bool m_snapshotRetryClearUndo = false;
    uint8_t m_snapshotRetryFailures = 0;
    fxe::ElapsedTime m_snapshotRetryTime;
    fxe::FixedString<32> m_snapshotRetryReason;

    // Undo base snapshot (cleared on external mutations).
    FenString m_undoBaseFen;
    uint64_t m_undoBaseMovedSquaresMask = 0;

    // Persisted undo history (7-field schema). Coordinator-owned.
    uint8_t m_undoHistoryFields[UNDO_HISTORY_FIELD_CAPACITY] = {};
    uint16_t m_undoHistoryFieldCount = 0;
    uint32_t m_undoHistoryRevision = 0U;
    uint8_t m_duckUndoFrom[UNDO_HISTORY_MOVE_CAPACITY] = {};
    uint8_t m_duckUndoTo[UNDO_HISTORY_MOVE_CAPACITY] = {};
    uint16_t m_duckUndoMoveCount = 0;

    enum class UndoCommandKind : uint8_t {
        None = 0,
        Undo,
        CancelUndo,
    };
    struct PendingUndoCommand {
        UndoCommandKind kind = UndoCommandKind::None;
        uint8_t from = 0U;
        uint8_t to = 0U;
        UndoCommandOrigin origin = UndoCommandOrigin::Manual;
    };
    PendingUndoCommand m_pendingUndoCommand{};
    uint8_t m_chainedUndoRemaining = 0U;

    // A grace window is RAM-only and bounded to the last acknowledged human
    // transaction. It deliberately reuses the normal semantic Undo command;
    // these snapshots only make that specific inverse exact for clocks and
    // deterministic coordinator state, including cancel-Undo.
    struct TakebackGraceState {
        bool prepared = false;
        bool armed = false;
        bool ambiguousCapture = false;
        bool duckChessFirstGuidance = false;
        bool duckInverseSelected = false;
        bool undoInProgress = false;
        bool hintOwned = false;
        bool requiresDuckFirst = false;
        uint32_t armedAtMs = 0U;
        uint32_t durationMs = 0U;
        uint32_t timerSetting = 0U;
        bool boardRotate = false;
        bool firstPlayerIsP2 = false;
        uint16_t historyFieldCountBefore = 0U;
        uint16_t historyFieldCountAfter = 0U;
        uint32_t historyRevisionBefore = 0U;
        uint32_t historyRevisionAfter = 0U;
        uint8_t from = runtime::CHESS_INTERACTION_INVALID_SQUARE;
        uint8_t to = runtime::CHESS_INTERACTION_INVALID_SQUARE;
        uint8_t secondaryFrom = runtime::CHESS_INTERACTION_INVALID_SQUARE;
        uint8_t secondaryTo = runtime::CHESS_INTERACTION_INVALID_SQUARE;
        uint8_t restorationSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
        uint8_t duckFrom = INVALID_DUCK_SQUARE;
        uint8_t duckTo = INVALID_DUCK_SQUARE;
        uint64_t afterOccupiedMask = 0ULL;
        runtime::GameClockState beforeClock{};
        runtime::GameClockState afterClock{};
        uint32_t beforeRngSeed = 1U;
        uint32_t afterRngSeed = 1U;
        uint64_t beforeMovedSquares = 0ULL;
        uint64_t afterMovedSquares = 0ULL;
    };
    TakebackGraceState m_takebackGrace{};

    void PrepareTakebackGrace(
        const runtime::PhysicalTransitionPlan& move);
    void ArmPreparedTakebackGrace(
        bool requiresDuckFirst = false,
        uint8_t duckFrom = INVALID_DUCK_SQUARE,
        uint8_t duckTo = INVALID_DUCK_SQUARE);
    void ClearTakebackGrace();
    void RefreshPreparedTakebackGraceClockSnapshot();
    void RestoreTakebackGraceSnapshots(bool forward);
    bool BeginTakebackGraceUndo(const runtime::AppEvent& event,
                                bool observationAlreadyApplied = false);
    bool ValidateTakebackGraceBoardEvent(
        const runtime::AppEvent& event);
    void ShowTakebackGraceDuckGuidance(
        const runtime::AppEvent& event,
        uint64_t changedMask,
        uint64_t occupiedMask);
    bool ContinueTakebackGraceDuckGuidance(
        const runtime::AppEvent& event,
        uint64_t changedMask,
        uint64_t occupiedMask);
    bool HandleTakebackGraceDuckFirst(
        const runtime::AppEvent& event,
        uint64_t changedMask,
        uint64_t occupiedMask);
    bool HandleTakebackGraceCaptureAmbiguity(
        const runtime::AppEvent& event,
        uint64_t changedMask,
        uint64_t occupiedMask);
    bool TryStageTakebackGraceCaptureAmbiguity(
        const runtime::ChessInteractionModel& model,
        uint64_t removedMask,
        uint64_t placedMask);
    bool IsTakebackGraceControlledChange(
        uint64_t changedMask,
        uint64_t destinationMask) const;
    bool HandleTakebackGraceIdleInverse(
        const runtime::AppEvent& event,
        uint64_t changedMask,
        uint64_t occupiedMask);

    // Last-entry patching for promotion moves (promotion choice arrives later).
    bool m_lastUndoEntryPromotionPending = false;

    // Cancel-undo reconstruction helper: preserve the pre-undo FEN and the
    // last popped undo-history entry.
    bool m_cancelUndoFenValid = false;
    FenString m_cancelUndoFen;
    bool m_cancelUndoHistoryEntryValid = false;
    uint8_t m_cancelUndoHistoryEntry[UNDO_HISTORY_FIELDS_PER_MOVE] = {};
    bool m_cancelUndoDuckValid = false;
    uint8_t m_cancelUndoDuckFrom = INVALID_DUCK_SQUARE;
    uint8_t m_cancelUndoDuckTo = INVALID_DUCK_SQUARE;
    VariantOutcome m_cancelUndoVariantOutcome{};

    // Fixed in-RAM ply-position ring for robust undo/cancel-undo mid-session.
    static constexpr uint8_t FEN_HISTORY_CAPACITY = static_cast<uint8_t>(UNDO_HISTORY_MOVE_CAPACITY + 1U);
    // The existing position codec retains every FEN field in 43 bytes. Duck
    // placement remains in the aligned duck history, outside the engine FEN.
    game_history::Position m_fenHistory[FEN_HISTORY_CAPACITY] = {};
    uint16_t m_halfmoveHistoryPlies[FEN_HISTORY_CAPACITY] = {};
    uint8_t m_fenHistoryStart = 0;
    uint8_t m_fenHistorySize = 0;

    void ClearFenHistory();
    bool ReadFenHistory(uint8_t index, FenString& out) const;
    void CopyFenWithCommittedFullmove(const FenString& fen, uint16_t retainedPlies,
                                      FenString& out) const;
    void SeedFenHistoryFromCurrentFen();
    bool PushFenHistory(const char* fen, uint16_t halfmoveClockPlies);
    bool RebuildFenHistoryFromUndoState();
    bool InitializeUndoHistoryReplayDuck(uint8_t& outDuckSquare) const;
    bool ApplyUndoHistoryReplayMove(uint16_t fieldOffset,
                                    FenString& inOutFen,
                                    uint8_t& inOutDuckSquare);
    bool TryUndoUsingFenHistory();
    bool TryCancelUndoUsingFenHistory();
    bool RebuildEngineFromUndoState();

    void UpdateLastMovePulseFromUndoHistory();
    void OverlayPendingDuckMovePulse();

    uint16_t ComputeHalfmoveClockPliesFromUndoHistory() const;

    void RefreshDrawStateFromTracking();

    // 50-move rule tracking (plies since last pawn move or capture).
    uint16_t m_halfmoveClockPlies = 0;
    bool m_cancelUndoHalfmoveValid = false;
    uint16_t m_cancelUndoHalfmovePlies = 0;

    // Choose mode defers applying promotion until the modal produces a choice.
    bool m_promotionPending = false;
    // A restored pending action must never turn into a new automatic choice.
    bool m_restoredPromotionRequiresChoice = false;

    // Coordinators used before LoadOrInitialize retain legacy Classic behavior.
    // A real fresh-game load/reset explicitly arms Undecided detection.
    GameVariant m_gameVariant = GameVariant::Classic;
    uint8_t m_duckSquare = INVALID_DUCK_SQUARE;
    DuckTurnPhase m_duckTurnPhase = DuckTurnPhase::ChessMove;
    VariantOutcome m_variantOutcome{};
    PendingDuckChessTurn m_pendingDuckTurn;
    bool m_stagedDuckChessUndoPending = false;
    bool m_variantDetectionArmed = false;
    bool m_initialVariantValidationPending = false;
    uint8_t m_setupDuckCandidateSquare = INVALID_DUCK_SQUARE;
    uint8_t m_pendingSetupPresentationEvents = 0U;
    uint32_t m_setupPresentationEventObservationSequence = 0U;
    bool m_standardSetupWasComplete = false;
    bool m_initialDuckObstructionRelocationPending = false;
    uint8_t m_initialDuckObstructedFromSquare = INVALID_DUCK_SQUARE;
    bool m_duckUndoRelocationPending = false;
    uint8_t m_duckUndoRequestedCount = 0U;
    bool m_duckRedoRelocationPending = false;
    bool m_externalizedDuckChessActionPending = false;
    bool m_externalizedDuckRelocationActionPending = false;
    bool m_externalizedDuckUndoActionPending = false;
    bool m_externalizedDuckRedoActionPending = false;
    bool m_externalizedDuckFixedActionValid = false;
    bool m_externalizedDuckRedoStartPending = false;
    bool m_externalizedInitialDuckRelocationActionPending = false;
    uint8_t m_externalizedDuckFrom = INVALID_DUCK_SQUARE;
    uint8_t m_externalizedDuckTo = INVALID_DUCK_SQUARE;

    // Legal destination caching: only reuse when both the picked-up origin and
    // current FEN match the last computed legality snapshot.
    uint8_t m_cachedLegalFromSquare = 255;
    FenString m_cachedLegalFromFen;

    // Legal destination cache for the current position, computed before starting
    // hint analysis so legality highlights remain responsive while the engine is
    // busy thinking.
    bool m_cachedLegalDestMapValid = false;
    FenString m_cachedLegalDestFen;
    uint64_t m_cachedLegalComputedFromSquaresMask64 = 0;
    uint64_t m_cachedLegalDestByFromSquare[64] = {};

    // VsCPU glue: coordinator owns engine think/poll and queues CpuMoving.
    bool m_vsCpuThinking = false;
    fxe::ElapsedTime m_cpuRetryElapsed;
    CpuThinkingEstimate m_cpuThinkingEstimate;
    // Existing cached FEN/variant owns correlation; this is never game authority.
    void* m_benchmarkControlsContext = nullptr;
    bool (*m_benchmarkControlsIdle)(void*) = nullptr;
    EngineBenchmarkStatus m_benchmarkStatus{};
    bool m_benchmarkActive = false;
    CpuPonderStatus m_ponderStatus{};
    bool m_ponderThinking = false;
    bool m_ponderRootValid = false;
    bool m_ponderCompleted = false;
    bool m_ponderCancelRequested = false;
    uint32_t m_ponderRequestId = 0U;
    fxe::ElapsedTime m_ponderElapsed;
    ThinkCandidate m_ponderFloor{};
    ThinkConfig m_ponderConfig{};
    uint32_t m_ponderHistoryRevision = 0U;
    uint16_t m_ponderHistoryFieldCount = 0U;
    uint16_t m_ponderHalfmoveClock = 0U;
    game_history::GameId m_ponderGameId{};
    FenString m_ponderAttemptFen;
    uint32_t m_ponderAttemptRevision = 0U;

    FenString m_diagnosticFen;
    GameVariant m_diagnosticVariant = GameVariant::Undecided;
    uint32_t m_diagnosticRequestId = 0U;
    ThinkDiagnostics m_cpuDiagnostics{};
    ThinkDiagnostics m_hintDiagnostics{};

    // VS-CPU RNG seed.
    //
    // This is intentionally coordinator-owned so that:
    // - We can reseed after each CPU move (requested UX behavior).
    // - Trace/replay can provide a stable seed for deterministic replays.
    uint32_t m_vsCpuRngSeed = 1;
    ThinkCandidate m_cpuSeed;

    // Human hints expose a legal seed immediately and retain completed
    // refinements through one 90-second background search.
    bool m_hintThinking = false;
    bool m_hintPendingStart = false;
    bool m_hintInitialSetupObservedComplete = false;
    bool m_hintLastSideToMoveValid = false;
    SideToMove m_hintLastSideToMove = SideToMove::White;

    fxe::ElapsedTime m_hintInitialWindow;
    fxe::ElapsedTime m_hintSearchElapsed;
    uint32_t m_hintCalculatedMs = 0U;
    uint32_t m_hintScannedNodes = 0U;
    uint8_t m_hintThinkDifficulty = 0;
    uint8_t m_hintCompletedDifficulty = 0;
    FenString m_hintAnalysisFen;
    uint8_t m_hintAnalysisDuckSquare = INVALID_DUCK_SQUARE;

    bool m_hintRetryDelayActive = false;
    uint8_t m_hintRetryFailureCount = 0;
    uint32_t m_hintRetryDelayMs = 0;
    fxe::ElapsedTime m_hintRetryElapsed;

    bool m_lastHintThinkStatsValid = false;
    uint32_t m_lastHintThinkElapsedMs = 0;
    uint32_t m_lastHintThinkNodeCount = 0;
    uint8_t m_lastHintThinkDifficulty = 0;
    UciMove m_lastHintThinkBestUci;

    bool m_hintCachedMoveValid = false;
    FenString m_hintCachedFen;
    uint8_t m_hintCachedFromSquare = 0;
    uint8_t m_hintCachedToSquare = 0;
    CoachInsight m_currentCoachInsight;
    bool m_currentCoachInsightValid = false;
    bool m_coachingHintActive = false;
    bool m_hintThinkAutomatic = false;

    bool m_duckHintTurnValid = false;
    FenString m_duckHintFen;
    uint8_t m_duckHintSquare = INVALID_DUCK_SQUARE;
    DuckThinkResult m_duckHintTurn;
    // Exact UCI (including promotion) for the outgoing visible compound turn.
    // New cache publications cannot change the Duck half of a held Hint.
    DuckThinkResult m_displayedDuckHintTurn;

    bool m_cancelUndoHintMoveValid = false;
    FenString m_cancelUndoHintFen;
    uint8_t m_cancelUndoHintFromSquare = 0;
    uint8_t m_cancelUndoHintToSquare = 0;

    AsyncThinkWorker m_asyncThinkWorker;
    // Optional, non-owning host seam. When supplied it must outlive this
    // coordinator. Firmware uses the owned worker above; ordinary native tests
    // retain synchronous engine polling unless they explicitly supply one.
    AsyncThinkWorker* m_asyncThinkWorkerOverride = nullptr;

    // When a CPU move is queued, preserve the engine-selected UCI string so
    // promotions are applied correctly when the move is committed.
    bool m_pendingCpuMoveUciValid = false;
    uint8_t m_pendingCpuMoveFrom = 0;
    uint8_t m_pendingCpuMoveTo = 0;
    bool m_pendingCpuMoveCaptureSquareValid = false;
    uint8_t m_pendingCpuMoveCaptureSquare = 0;
    UciMove m_pendingCpuMoveUci;
    bool m_pendingCpuDuckDestinationValid = false;
    uint8_t m_pendingCpuDuckDestination = INVALID_DUCK_SQUARE;

    bool m_lastCastlingRookPromptActive = false;

    // One-shot cancellation request (used by input/UI to stop active thinking
    // immediately without blocking the main loop).
    bool m_cancelAllThinkingRequested = false;
    bool m_cancelCpuWorkRequested = false;
    bool m_cancelHintWorkRequested = false;

    // Setup correction: when either orientation setting changes the effective
    // logical-to-physical square mapping at the initial position, require a
    // king/queen swap choreography prompt.
    //
    // Important: This prompt should only appear in response to a setting toggle
    // (not simply because a setting is currently enabled), otherwise it would
    // re-trigger on every new game.
    bool m_kqPromptInitialized = false;
    bool m_lastBoardRotateEnabled = false;
    uint16_t m_kqPromptGeneration = 0;
    uint16_t m_kqPromptLastStartedGeneration = 0;

    // Effective square-orientation baseline from before the pending/active
    // change. Returning to it before a prompted lift cancels the prompt.
    bool m_kqPromptBaselineValid = false;
    bool m_kqPromptBaselineSquareRotate = false;

    // Track player-end placement toggles so we can reinitialize the engine (and derived
    // caches) immediately when the setting changes at the starting position.
    bool m_firstPlayerInitialized = false;
    bool m_lastFirstPlayerIsP2 = false;

    runtime::GameClock m_gameClock;
    GameWinner m_clockTerminalWinner = GameWinner::None;
    uint32_t m_clockTerminalStartMs = 0U;
    fxe::FixedString<6> m_clockTerminalDuckChessMove;
    FenString m_clockTerminalFen;
    bool m_terminalAbandonPending = false;
};

} // namespace modules
