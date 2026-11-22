#pragma once

#include <cstdint>

#include "foxie_embedded/deadline_timer.hpp"
#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/persisted_side.hpp"
#include "modules/promotion_ux.hpp"
#include "modules/user_input.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/game_state_machine_render_state.hpp"

namespace modules {
class GamePersistenceCoordinator;
} // namespace modules

namespace runtime {

struct ChessInteractionDurableState;

struct GameStateMachineConfig {
    uint32_t settleDurationMs = 2000;

    // Cadence for settle countdown flash (accelerated vs illegal flash).
    uint32_t settleFlashMs = 200;
    uint32_t cpuIdleDelayMs = 1200;

    // Cadence for flashing illegal markers (human and CPU).
    uint32_t illegalFlashMs = 300;

    // Cadence for capture-return prompts.
    uint32_t captureReturnFlashMs = 300;

    // Placement-mode (mismatch recovery) entry/exit requires sensors to be stable.
    // The state machine uses time since last sensor delta as a stability proxy.
    uint32_t placementStableRequiredMs = 200;

    // Promotion selection UX (DPad/buttons). See modules::PromotionUx.
    modules::PromotionUxConfig promotionUx;
};

// Minimal state machine with deterministic, overrideable time.
// Intentionally incomplete: centralizes guards, timeouts, and transitions.
class GameStateMachine {
  public:
    explicit GameStateMachine(const GameStateMachineConfig& config = {})
        : m_config(config), m_promotionUx(config.promotionUx), m_humanSettleDurationMs(config.settleDurationMs) {}

    void Reset();

    void Tick();

    void SetMoveConfirmPolicy(uint32_t settleDurationMs,
                              bool requireOConfirm);

    void HandleSensorChange(uint8_t square, bool nowOccupied);
    void HandleSensorChange(uint8_t square,
                            bool nowOccupied,
                            uint32_t tMs,
                            uint32_t sequence);

    // Consume one atomic physical-board observation. changedMask describes all
    // squares that changed since the previous hardware scan; occupiedMask is
    // the complete new logical occupancy. No order between changed squares is
    // implied by this event.
    void HandleBoardObservation(uint64_t changedMask,
                                uint64_t occupiedMask,
                                uint32_t tMs,
                                uint32_t sequence,
                                uint8_t deferredDuckDestinationSquare =
                                    CHESS_INTERACTION_INVALID_SQUARE);
    uint8_t GetDeferredDuckDestinationForObservation(
        uint64_t changedMask, uint64_t occupiedMask) const;
    // Coordinator-owned guidance may need to observe a physical mismatch
    // without letting normal move inference turn it into correction. This is
    // the same canonical-board update used by other modals, without changing
    // placement/prompt ownership.
    void HandleGuidedBoardObservation(uint64_t changedMask,
                                      uint64_t occupiedMask,
                                      uint32_t tMs,
                                      uint32_t sequence);
    // Recovery after an input-blind interval cannot generally infer chronology.
    // Preserve an exactly matching in-progress interaction, and admit the one
    // safe continuation whose identity is already proven by a settling plan:
    // one next-side source lifted from the completed position. Everything else
    // updates the canonical board through correction-only authority.
    void HandleRecoveryBoardObservation(uint64_t changedMask,
                                        uint64_t occupiedMask,
                                        uint32_t tMs,
                                        uint32_t sequence);
    void HandleInteractionPositionAcknowledged(uint64_t occupiedMask,
                                               uint64_t kingMask,
                                               uint64_t rookMask,
                                               uint64_t moverMask = 0,
                                               uint64_t opponentMask = 0,
                                               uint64_t pawnMask = 0,
                                               uint8_t enPassantTargetSquare = CHESS_INTERACTION_INVALID_SQUARE,
                                               uint8_t enPassantCaptureSquare = CHESS_INTERACTION_INVALID_SQUARE,
                                               bool sideToMoveValid = false,
                                               bool sideToMoveBlack = false,
                                               bool legalCaptureTargetsValid = false,
                                               uint64_t legalCaptureTargetsMask = 0,
                                               bool legalCastlingTargetsValid = false,
                                               uint64_t legalCastlingKingTargetsMask = 0,
                                               const CaptureLegality* captureLegality = nullptr);
    void HandleInteractionActionResult(uint32_t token, bool succeeded);
    void HandleInteractionSearchResult(bool succeeded);
    void HandleInteractionAdapterResult(bool succeeded);
    bool RestoreInteractionDurableState(
        const ChessInteractionDurableState& state);

    // CPU thinking guard: while active, observations update canonical physical
    // occupancy and correction feedback but cannot begin a human move.
    void SetCpuThinkingActive(bool active);
    bool IsCpuThinkingActive() const { return m_cpuThinkingActive; }

    // Button input (DPad + player buttons). Events are delivered via AppEvent.
    void HandleButtonEvent(modules::UserButton button, fxe::Button::Event event);

    // Undo prompt: after requesting undo, highlight where the last moved piece
    // must be returned (destination = last move's from square). This is a
    // corrective-only modal that blocks new moves until the occupancy matches.
    bool IsUndoPromptActive() const;
    uint32_t GetUndoPromptElapsedMs() const;
    bool IsUndoPromptMetadataConfigured() const { return m_undoPromptMetadataConfigured; }
    bool TryGetUndoPromptMove(uint8_t& outFromSquare, uint8_t& outToSquare) const;

    // When the active undo prompt corresponds to a castling king move,
    // returns the rook's forward move squares (rookFrom -> rookTo).
    // The undo prompt expects the rook to return from rookTo to rookFrom.
    bool TryGetUndoPromptCastlingRookMove(uint8_t& outRookFrom, uint8_t& outRookTo) const;

    // After a king-first castling move completes, but before the rook has been
    // moved, the board enters a forced rook-completion prompt.
    bool IsCastlingRookPromptActive() const;
    bool TryGetCastlingRookPromptMove(uint8_t& outRookFrom, uint8_t& outRookTo) const;

    // Undo prompt metadata: coordinator can provide engine-aware context so the
    // UX can guide capture restoration (and avoid wrong-color prompts when the
    // engine state intentionally lags physical occupancy).
    //
    // moverSidePersist: PersistedSide::White/Black/Unknown.
    // movedPieceFen: FEN piece letter for the moved piece (e.g. 'P'/'n'/'Q'). '\0' if unknown.
    // capturedPieceFen: FEN piece letter (e.g. 'p'/'P'/'n'/'Q'). '\0' if none.
    // capturedSquare: 0..63 when valid.
    void ConfigureUndoPromptMetadata(modules::PersistedSide moverSidePersist,
                                     char movedPieceFen,
                                     char capturedPieceFen,
                                     uint8_t capturedSquare);

    // Coordinator-owned undo commands enter/leave the reducer through these
    // narrow physical-interaction operations. The state machine owns no undo
    // history and no cancel-undo window.
    bool BeginUndoPrompt(uint8_t fromSquare,
                         uint8_t toSquare,
                         modules::PersistedSide moverSidePersist,
                         char movedPieceFen,
                         char capturedPieceFen,
                         uint8_t capturedSquare);
    bool BeginPendingPromotionUndo();
    void CancelUndoPrompt(bool restoreForwardSide);
    void ReconcileUndoOccupancy();
    void ClearMoveRenderTransition();
    void CancelMovePreviewForUndo();
    bool CancelCompletedSettleForUndo();
    bool IsMoveActionAwaitingResult() const;
    bool CancelUnclaimedMoveActionForUndo();

    bool TryGetUndoPromptMoverSide(modules::PersistedSide& outMoverSidePersist) const;

    bool TryGetUndoPromptMoverPieceFen(char& outMovedPieceFen) const;

    bool TryGetUndoRestoreInfo(uint8_t& outSquare, char& outCapturedPieceFen) const;

    // King/queen swap prompt: a corrective modal requiring the user to lift
    // a set of pieces (by square occupancy only) and then restore them before
    // play proceeds.
    //
    // Note: with occupancy-only sensing we cannot verify piece identity, so the
    // policy is enforced as a choreography:
    // 1) each prompt square becomes empty at least once
    // 2) all prompt squares become occupied again
    // 3) sensors remain stable for placementStableRequiredMs
    bool IsKingQueenSwapPromptActive() const { return m_kingQueenSwapPromptActive; }

    // Special-case escape hatch: if the prompt has started but the user has not
    // lifted any of the prompted pieces yet, we allow canceling the prompt
    // (e.g. user toggles rotate/flip back immediately).
    bool IsKingQueenSwapPromptCancelable() const;

    bool CancelKingQueenSwapPromptIfCancelable();
    bool TryGetKingQueenSwapPromptMask(uint64_t& outSquaresMask64) const;

    // Per-square progress UX: squares stop flashing once they have been lifted
    // at least once and are currently occupied again.
    bool TryGetKingQueenSwapPromptFlashMask(uint64_t& outSquaresMask64) const;
    bool StartKingQueenSwapPrompt(uint64_t squaresMask64);

    bool TryClaimPendingInteractionMove(PhysicalTransitionPlan& out);

    // Begins the mandatory neutral-duck relocation. An invalid required
    // destination allows any different empty square; undo/redo may provide a
    // fixed destination.
    bool BeginDuckRelocation(
        uint8_t fromSquare,
        uint8_t requiredDestination = CHESS_INTERACTION_INVALID_SQUARE,
        PhysicalTransitionSource source =
            PhysicalTransitionSource::Human,
        DirectedMoveOrigin directedOrigin = DirectedMoveOrigin::Cpu);
    bool CancelDuckRelocationForUndo();
    bool IsDuckRelocationActive() const;
    bool TryGetDuckRelocation(uint8_t& outFromSquare,
                              uint8_t& outToSquare) const;

    bool HasCommittedMovePending() const;

    // Reducer action plans should be applied to the coordinator immediately so
    // engine state, legal masks, and persistence stay in step. Undo flows are
    // intentionally deferred to the next coordinator cadence so the first undo
    // prompt frame can render before heavier bookkeeping runs.
    bool ShouldProcessCommittedMoveImmediately() const;

    bool TryGetVisualActiveSideOverride(modules::PersistedSide& outSide) const;

    // Explicit status-edge overlay owner for transition UX (for example undo
    // entry/exit). When unset, the edge UI falls back to side-to-move.
    bool TryGetStatusEdgeActiveSideOverride(modules::PersistedSide& outSide) const;

    // Moved-squares overlay persistence.
    uint64_t GetMovedSquaresMask() const { return m_movedSquaresMask; }
    uint64_t GetMovedSquaresMaskBeforeLastAcknowledgedHumanMove() const {
        return m_movedSquaresMaskBeforeLastAcknowledgedHumanMove;
    }
    void ClearMovedSquares();
    void SetMovedSquaresMask(uint64_t mask);

    // External engine mutation (e.g. `engine fen`): abandon in-flight state
    // and history while preserving physical occupancy.
    void ResetForExternalEngineMutation();

    // External engine mutation detection should not interrupt modal overlays
    // (e.g., text overlays). Upstream can set this to temporarily suppress
    // polling-based mutation handling.
    void SetTextOverlayActive(bool active);
    bool IsTextOverlayActive() const { return m_textOverlayActive; }

    // External engine mutation (or similar) requires the check/cache layer to
    // refresh derived state. This is a lightweight signal; consumers can poll
    // and clear it.
    bool ConsumeCheckCacheRefreshRequested();

    // Promotion modal: upstream (engine/display) triggers entry when a promotion
    // is pending. Selection is driven by button events and/or timeout.
    bool StartPromotion();

    bool IsPromotionActive() const { return m_promotionUx.IsActive(); }
    uint32_t GetPromotionElapsedMs() const { return m_promotionUx.ElapsedMs(); }
    modules::PromotionPiece GetPromotionSelection() const { return m_promotionUx.GetSelection(); }
    bool ConsumePromotionChoice(modules::PromotionChoice& outChoice);

    // Promotion UX render helpers.
    bool TryGetPromotionMove(uint8_t& outFromSquare, uint8_t& outToSquare, bool& outMoverIsBlack) const;

    // CPU/engine-directed physical move prompting.
    // captureSquareValid + captureSquare are used for en-passant and capture-first.
    bool QueueCpuMove(uint8_t fromSquare,
                      uint8_t toSquare,
                      bool captureSquareValid,
                      uint8_t captureSquare,
                      char promotionSuffixLower);
    bool HasQueuedCpuMove() const { return m_cpuExecution.queued; }

    // Game-over / external mutation hook: cancel any queued CPU move prompt.
    // This returns the state machine to a neutral Idle state and clears any
    // CPU-illegal markers associated with the prompt.
    void CancelQueuedCpuMove();

    bool ConsumeUndoCanceledQueuedCpuMove();

    bool TryGetQueuedCpuMove(uint8_t& outFromSquare,
                             uint8_t& outToSquare,
                             bool& outCaptureSquareValid,
                             uint8_t& outCaptureSquare) const;

    bool TryGetQueuedCpuPromotionSuffix(char& outPromotionSuffixLower) const;

    uint32_t GetCpuMovePromptElapsedMs() const;

    // Placement mode (mismatch recovery) is an orthogonal corrective mode that
    // blocks normal move progression.
    // Entry is guarded by stability: the caller must pass the current time and
    // sensors must have been stable (no deltas) for placementStableRequiredMs.
    bool TryEnterPlacementMode();

    // Console/diagnostics hook: enter placement mode regardless of current
    // state/stability checks. This is useful for manually forcing mismatch
    // correction from the console.
    bool ForceEnterPlacementMode();
    void ExitPlacementMode();

    bool IsPlacementModeActive() const { return m_placementModeActive; }
    uint64_t GetPlacementMissingMask() const { return m_placementMissingMask; }
    uint64_t GetPlacementExtraMask() const { return m_placementExtraMask; }
    bool DidCurrentInteractionStartFromMismatch() const { return m_interactionStartedFromMismatch; }

    uint8_t GetLastSensorSquare() const { return m_lastSensorSquare; }

    uint8_t GetSettleSquare() const { return m_renderState.settleSquare; }

    bool IsMoveSettlingActive() const;
    bool IsHumanPieceInHand() const;
    bool TryGetHumanPieceInHandSquare(uint8_t& outSquare) const;
    bool IsHumanMoveInProgress() const;

    // Diagnostics only. This is a derived label, never transition authority.
    const char* DescribeInteractionState() const;

    // Human-illegal markers are latched until explicitly corrected, or until
    // the physical occupancy matches the expected occupancy everywhere.
    bool HasHumanIllegal() const { return m_humanIllegalMask != 0; }
    uint64_t GetHumanIllegalMask() const { return m_humanIllegalMask; }

    // Physical occupancy (sensor-driven), in logical square indices.
    uint64_t GetPhysicalOccupancyMask() const {
        return m_chessInteractionModel.board.occupiedMask;
    }
    uint32_t GetLastBoardObservationTimestampMs() const {
        return m_chessInteractionModel.board.timestampMs;
    }
    uint32_t GetLastBoardObservationSequence() const {
        return m_chessInteractionModel.board.sequence;
    }
    uint64_t GetLastBoardObservationChangedMask() const {
        return m_chessInteractionModel.board.changedMask;
    }
    uint64_t GetLastBoardObservationOccupancyMask() const {
        return m_chessInteractionModel.board.occupiedMask;
    }
    const ChessInteractionModel& GetChessInteractionModel() const {
        return m_chessInteractionModel;
    }
    const InteractionInvariantStatus& GetInteractionInvariantStatus() const {
        return m_interactionInvariantStatus;
    }
    // Firmware-only boot/resync helper: apply a full occupancy snapshot.
    // This updates the canonical physical occupancy without treating it as a
    // user interaction (no pickup/placing transitions).
    void ApplyPhysicalOccupancySnapshot(uint64_t occ64);

    // Records an exact setup snapshot owned by a higher-level validation gate.
    // No chess move or correction feedback may begin from this observation.
    void HandleVariantValidationBoardObservation(uint64_t changedMask,
                                                 uint64_t occupiedMask,
                                                 uint32_t tMs,
                                                 uint32_t sequence = 0U);

    // True when physical occupancy matches the expected occupancy everywhere.
    // This is a useful proxy for "playable" vs correction flows.
    bool IsExpectedOccupancyMatched() const {
        return m_chessInteractionModel.board.occupiedMask ==
               m_chessInteractionModel.acknowledged.occupiedMask;
    }
    bool IsPhysicalOccupancyConsistentWithInteraction() const;

    // Expected occupancy (engine position model), in logical square indices.
    uint64_t GetExpectedOccupancyMask() const {
        return m_chessInteractionModel.acknowledged.occupiedMask;
    }

    // Expected mover/opponent occupancy (engine piece-side model) for the
    // current side-to-move.
    //
    // This is used to support OTB-friendly capture workflows like
    // "target-first" (captured piece removed before the moving piece is lifted).
    uint64_t GetExpectedMoverOccupancyMask() const {
        return m_chessInteractionModel.acknowledged.moverOccupiedMask;
    }
    uint64_t GetExpectedOpponentOccupancyMask() const {
        return m_chessInteractionModel.acknowledged.opponentOccupiedMask;
    }

    // During Settling, a capture square may be pending (en-passant or
    // capture-first). When valid, renderers should consider highlighting it.
    bool TryGetPendingCaptureSquare(uint8_t& outSquare) const;

    // During CpuMoving, a target occupancy is computed from the queued move.
    bool TryGetCpuMoveTargetOccupancyMask(uint64_t& outOcc64) const;

    bool HasCaptureReturn() const { return m_captureReturnMask != 0; }
    uint64_t GetCaptureReturnMask() const { return m_captureReturnMask; }

    bool HasCaptureFirstStaging() const;
    uint64_t GetCaptureFirstStagingMask() const;

    bool HasCpuIllegal() const { return m_cpuExecution.illegalMask != 0; }
    uint64_t GetCpuIllegalMask() const { return m_cpuExecution.illegalMask; }
    bool HasCpuTentativeCorrection() const {
        return m_cpuExecution.tentativeCorrectionMask != 0ULL;
    }
    uint64_t GetCpuTentativeCorrectionMask() const {
        return m_cpuExecution.tentativeCorrectionMask;
    }

    uint64_t GetValidDestMask() const { return m_validDestMask; }
    void SetValidDestMask(uint64_t mask);
    void ClearValidDestMask();

    // Reject the current pickup (or in-flight move) as an illegal interaction.
    // This marks the square as human-illegal (so the user is prompted to correct
    // the physical board state) and abandons the move-in-progress.
    bool RejectCurrentPickupAsIllegal(uint8_t illegalSquare);

    // Explain an exposed king while an absolutely pinned piece is held, or
    // while an illegal self-check pickup still needs restoration. Legal moves
    // along the pin line keep their ordinary interaction authority.
    void SetKingSafetyPrompt(uint8_t triggerSquare, uint8_t kingSquare, uint8_t attackerSquare);
    void ClearKingSafetyPrompt();
    bool TryGetKingSafetyPrompt(uint8_t& outKingSquare, uint8_t& outAttackerSquare) const;

    bool IsCaptureReturnFlashOn() const;

    bool IsSettleFlashOn() const;

    // True when the state machine is in a settling window that was started as
    // a CPU-move bounce suppression phase (not a human move settle).
    bool IsCpuMoveSettlingActive() const { return m_cpuExecution.moveSettleActive; }

    // Bounded restore/relift and local castling-relocation acknowledgement.
    // Completing normal settling clears this feedback without replaying it.
    bool TryGetSettleAfterglow(uint8_t& outSquare, uint32_t& outElapsedMs) const;

    // True for relift/return feedback, rather than an intermediate castling
    // relocation acknowledgement.
    bool IsSettleAfterglowAbort() const { return m_renderState.settleAfterglowAbort; }

    // Settling animation variant selection.
    //
    // Renderers can use this to select among multiple settle effects without
    // changing core settle semantics. Variant selection policy is intentionally
    // separate from the renderer (it may be driven by a future global animation
    // mode, not necessarily by automatic cycling).
    uint8_t GetSettleAnimationVariant() const { return m_renderState.settleAnimVariant; }

    // Returns true exactly once per settle-start, and provides the selected
    // variant index (0..SETTLE_ANIM_VARIANT_COUNT-1).
    bool ConsumeSettleAnimationStarted(uint8_t& outVariant);

    // Settling timing (only valid while a real settle timer is running).
    bool TryGetSettleElapsedAndDurationMs(uint32_t& outElapsedMs, uint32_t& outDurationMs) const;
    uint32_t GetSettlePresentationDurationMs() const {
        return m_humanSettleActive && !m_activeHumanSettleRequiresO
            ? m_activeSettleDurationMs
            : m_config.settleDurationMs;
    }

    // Best-move hint UX (render-only state).
    void SetHintDisplayRequested(bool requested);
    void NoteManualHintRequest();
    void RetireManualHintRequest();
    bool IsManualHintRequested() const { return m_renderState.manualHintRequested; }
    bool IsHintDisplayRequested() const { return m_renderState.hintDisplayRequested; }
    void HandleHintButtonTap();
    void ReplayHintMove();
    void SetHintFeedbackPending(bool pending);

    void SetHintThinkingActive(bool active);
    bool IsHintThinkingActive() const { return m_renderState.hintThinkingActive; }

    uint32_t GetIllegalFlashElapsedMs() const;
    uint32_t GetIllegalFlashPeriodMs() const;
    uint32_t GetCaptureReturnFlashElapsedMs() const;
    uint32_t GetCaptureReturnFlashPeriodMs() const {
        return m_config.captureReturnFlashMs == 0U ? 1U : m_config.captureReturnFlashMs;
    }
    uint32_t GetSettleFlashPeriodMs() const {
        return m_config.settleFlashMs == 0U ? 1U : m_config.settleFlashMs;
    }
    uint32_t GetLastInteractionElapsedMs() const;

    void ClearHintMove();
    void SetDuckRelocationHint(uint8_t fromSquare, uint8_t toSquare);
    void CancelHintRefinement();
    void SetHintMoveSquares(uint8_t fromSquare, uint8_t toSquare);
    bool TryGetHintMoveSquares(uint8_t& outFromSquare, uint8_t& outToSquare) const;
    void ClearHintDuckMove();
    void SetHintDuckMoveSquares(uint8_t fromSquare, uint8_t toSquare);
    bool TryGetHintDuckMoveSquares(uint8_t& outFromSquare,
                                   uint8_t& outToSquare) const;

    // Hint cancellation signaling: state machine requests cancel based on
    // observed physical interaction.
    bool ConsumeHintCancelRequested();

    void SetHintMoverIsBlack(bool moverIsBlack);
    void ClearHintMoverSide();

    // Computes the current flash phase for illegal markers.
    bool IsIllegalFlashOn() const;

    // Last-move UX: coordinator updates this from engine-aware history so the
    // LED renderer can show subtle "most recent move" pulses.
    void SetLastMoveForSide(modules::PersistedSide side,
                            uint8_t fromSquare,
                            uint8_t toSquare);
    void ClearLastMoveForSide(modules::PersistedSide side);
    bool TryGetLastMoveForSide(modules::PersistedSide side,
                               uint8_t& outFromSquare,
                               uint8_t& outToSquare) const;
    void SetLastMoveMostRecentSide(modules::PersistedSide side);
    void ClearLastMoveMostRecentSide();
    bool TryGetLastMoveMostRecentSide(modules::PersistedSide& outSide) const;

    // UX cue: briefly flash all piece LEDs (occupied squares).
    // Used when a menu toggle is blocked because the game is not at the starting position.
    void TriggerAllPiecesFlash(uint32_t durationMs);
    // Presentation still advances while modes such as Litchcraft pause gameplay.
    void RetireExpiredAllPiecesFlash(const uint32_t nowMs) {
        m_renderState.allPiecesFlash.RetireExpired(nowMs);
    }
    bool IsAllPiecesFlashActive() const;
    bool IsAllPiecesFlashOn() const;

    // Fresh-start identity assist. Hall sensors can prove occupancy but not
    // that the expected kings are the pieces on these two squares.
    void TriggerInitialKingPlacementPrompt();
    void CancelInitialKingPlacementPrompt();
    bool TryGetInitialKingPlacementPrompt(
        uint64_t& outSquaresMask64) const;

    using HintPresentationObserver = HintPresentationTuple (*)(void*, bool retire);
    using HintPresentationPublisher = void (*)(void*);
    void SetHintPresentationObserver(HintPresentationObserver observer,
        HintPresentationPublisher publisher, void* context) {
        m_hintPresentationObserver = observer;
        m_hintPresentationPublisher = publisher;
        m_hintPresentationContext = context;
    }
    HintPresentationTuple GetVisibleHintPresentation(bool retire = false) const;
    const GameStateMachineRenderState& GetRenderState() const { return m_renderState; }
    void ShowPromotionConfirmation(PromotionConfirmation confirmation) {
        confirmation.startedAtMs = fxe::Millis();
        m_renderState.promotionConfirmation = confirmation;
    }

    // Coordinator ownership boundary: production code should supply the full
    // engine-derived expected board model through GamePersistenceCoordinator.
    // Native tests may still seed expected state directly through these debug
    // helpers so isolated state-machine slices remain easy to exercise.
#ifndef ARDUINO
    void DebugSetExpectedPositionModel(const ChessPositionState& model);

    void SetExpectedOccupancyMask(uint64_t expectedOcc64);

    void SetExpectedSideToMove(modules::PersistedSide side);

    void SetExpectedMoverAndOpponentOccupancyMasks(uint64_t moverOcc64,
                                                   uint64_t opponentOcc64);

    void SetExpectedKingAndRookOccupancyMasks(uint64_t kingOcc64,
                                              uint64_t rookOcc64);

    void SetLegalCaptureTargetsMask(uint64_t legalCaptureTargetsMask64,
                                    bool valid);
#endif

    bool TryGetExpectedMoverKingSquare(uint8_t& outSquare) const;

#ifndef ARDUINO
    // Native-test hook: seed physical occupancy without treating it as a user
    // interaction.
    void DebugSetPhysicalOccupancyMask(uint64_t occ64);
    void DebugSetPendingCaptureSquare(uint8_t captureSquare) {
        m_chessInteractionModel.phase.plan.removalRequired =
            captureSquare < 64U;
        m_chessInteractionModel.phase.plan.removalSquare =
            captureSquare < 64U ? captureSquare
                                : CHESS_INTERACTION_INVALID_SQUARE;
    }
#endif

    // Hook: true once the idle-delay timeout fires. Consumers can poll
    // and clear this to kick off CPU thinking.
    bool ConsumeCpuIdleTimeoutFired();

  private:
    static constexpr uint8_t INVALID_SQUARE = 255;
    class PromotionInputAdapter;
    friend class PromotionInputAdapter;
    friend class modules::GamePersistenceCoordinator;

    struct ResetOptions {
        bool stabilizePlacement = false;
        bool resetHumanSettlePolicy = true;
        bool clearTextOverlay = true;
        bool requestCheckCacheRefresh = false;
    };

    void ApplyResetOptions(uint32_t nowMs, const ResetOptions& options);
    void RefreshThinkingControls();
    bool IsContinuousDuckHint() const;
    ChessPositionState GetInteractionExpectedPosition() const;
    bool TryGetDeferredFollowupSource(uint8_t& outSquare) const;
    bool IsConfirmedNextSidePickupObservation(
        const TaggedInteractionState& phase,
        uint64_t changedMask,
        uint64_t occupiedMask) const;

    bool IsSensorsStableForPlacement() const;
    bool HasPlacementMismatch() const;
    void EnterPlacementMode();
    void ClearMovePreviewForPlacementMode();

    void UpdatePromotion();

    void ClearKingQueenSwapPrompt();
    void UpdateKingQueenSwapPromptProgress();

    void StartSettling(uint8_t toSquare);
    void StartCpuMoveSettling(uint8_t settleSquare);
    void FinalizeMove();
    void FinalizeCpuMoveSettling(bool replaySuppressedDestination = true);

    const PhysicalTransitionPlan* GetActiveHumanCastlingPlan() const;
    void UpdateCastlingRelocationFeedback(
        const TaggedInteractionState& before,
        const TaggedInteractionState& after);
    void CommitCastlingComposite();
    void ReduceInteraction(const ChessInteractionEvent& event,
                           const CaptureLegality* captureLegality = nullptr);
    void ReduceInteractionBoardObservation(uint64_t changedMask,
                                           uint64_t occupiedMask,
                                           uint32_t tMs,
                                           uint32_t sequence,
                                           uint8_t deferredDuckDestinationSquare =
                                               CHESS_INTERACTION_INVALID_SQUARE);
    void ReconcileObservedMoveProgress(
        const TaggedInteractionState& before,
        const ChessInteractionEvent& event,
        bool confirmedSettleForObservation);
    void ReconcileCpuPlanAfterModal();
    void ReconcileHumanPlanAfterModal();
    void HandleBlockedHumanSettleObservation(
        const ChessInteractionEvent& event,
        bool restartConfirmationAfterCorrection);
    bool ProjectInteractionCorrection();
    void ClearVariantValidationFeedback();
    bool TryAcknowledgeInteractionPlan(PhysicalTransitionKind kind,
                                       uint8_t from,
                                       uint8_t to);
    void AcknowledgePendingInteractionLegacyMove();

    void CancelCurrentMove();
    void HandleUndoPromptSensorChange(uint8_t square, bool nowOccupied);
    void TryResolveUndoPromptIfSatisfied();
    bool IsUndoPromptAllowedRemoval(uint8_t square) const;
    bool IsUndoPromptAllowedPlacement(uint8_t square) const;

    bool IsEnPassantSettleBlocked() const;
    bool IsHumanOConfirmSettleActive() const;
    void StartHumanOConfirmFinalizeSettle();
    bool HandleQueuedCpuCorrectionObservation(
        const TaggedInteractionState& before,
        uint64_t changedMask,
        uint64_t occupiedMask,
        uint32_t tMs,
        uint32_t sequence);
    void ClearCpuTentativeCorrection();
    void RestartIllegalFlashCadence();
    void RestartCaptureReturnFlashCadence();
    void UpdatePlacementMode();

    bool BuildCpuTransitionPlan(uint8_t from,
                                uint8_t to,
                                bool captureSquareValid,
                                uint8_t captureSquare,
                                char promotionSuffixLower,
                                PhysicalTransitionPlan& outPlan) const;
    void ClearCpuMove(bool preservePendingAction = false);
    bool TryFinalizeCpuMove();

    struct CpuExecutionState {
        bool queued = false;
        char promotionSuffixLower = '\0';
        bool undoCanceledQueuedMove = false;
        fxe::ElapsedTime promptElapsed;
        uint64_t illegalMask = 0;
        uint64_t tentativeCorrectionMask = 0;
        bool moveSettleActive = false;
        bool settleDestinationSuppressed = false;
        bool idleTimeoutFired = false;
    };

    GameStateMachineConfig m_config;

    uint8_t m_lastSensorSquare = 0;

    fxe::ElapsedTime m_lastInteractionElapsed;

    ChessInteractionModel m_chessInteractionModel{};
    InteractionInvariantStatus m_interactionInvariantStatus{};

    bool m_cpuThinkingActive = false;

    CpuExecutionState m_cpuExecution;

    // Human-illegal marker state.
    uint64_t m_humanIllegalMask = 0;
    uint32_t m_illegalFlashStartMs = 0;

    // Capture-return prompt state.
    uint64_t m_captureReturnMask = 0;
    uint32_t m_captureReturnFlashStartMs = 0;

    // Valid destinations for the currently picked-up piece. When non-zero,
    // placements outside this mask are treated as human-illegal.
    uint64_t m_validDestMask = 0;
    bool m_validDestMaskKnown = false;

    uint8_t m_kingSafetyTriggerSquare = INVALID_SQUARE;
    uint8_t m_kingSafetyKingSquare = INVALID_SQUARE;
    uint8_t m_kingSafetyAttackerSquare = INVALID_SQUARE;

    // Placement mode state (mismatch recovery).
    bool m_placementModeActive = false;
    uint64_t m_placementMissingMask = 0;
    uint64_t m_placementExtraMask = 0;
    bool m_interactionStartedFromMismatch = false;

    bool m_textOverlayActive = false;
    bool m_checkCacheRefreshRequested = false;

    // Promotion modal state.
    modules::PromotionUx m_promotionUx;
    bool m_promotionChoicePending = false;
    modules::PromotionChoice m_promotionChoice;

    static constexpr uint8_t BUTTON_QUEUE_CAPACITY = 8;
    modules::ButtonEvent m_buttonQueue[BUTTON_QUEUE_CAPACITY]{};
    uint8_t m_buttonQueueHead = 0;
    uint8_t m_buttonQueueTail = 0;
    uint8_t m_buttonQueueCount = 0;

    // Pending move metadata for the current settle.
    // Human move settle policy (runtime-configurable from settings).
    uint32_t m_humanSettleDurationMs = 2000;
    bool m_humanSettleRequiresO = false;
    bool m_activeHumanSettleRequiresO = false;
    bool m_humanSettleActive = false;

    fxe::DeadlineTimer m_settleTimer;

    static constexpr uint8_t SETTLE_ANIM_VARIANT_COUNT = 5;
    static constexpr uint32_t CIRCLE_CONFIRM_FINALIZE_SETTLE_MS = 1000U;
    uint32_t m_activeSettleDurationMs = 0;

    HintPresentationObserver m_hintPresentationObserver = nullptr;
    HintPresentationPublisher m_hintPresentationPublisher = nullptr;
    void* m_hintPresentationContext = nullptr;
    GameStateMachineRenderState m_renderState{};

    uint32_t m_claimedInteractionActionToken = 0U;
    uint32_t m_undoPromptStartMs = 0;
    bool m_undoPromptMetadataConfigured = true;

    // Undo prompt metadata (coordinator-provided). This is used only for UX
    // prompts and for tightening the prompt's target occupancy when undoing a
    // capture.
    modules::PersistedSide m_undoPromptMoverSidePersist = modules::PersistedSide::Unknown;
    char m_undoPromptMoverPieceFen = '\0';
    modules::PersistedSide m_visualActiveSideOverridePersist = modules::PersistedSide::Unknown;
    modules::PersistedSide m_statusEdgeActiveSideOverridePersist = modules::PersistedSide::Unknown;

    bool m_kingQueenSwapPromptActive = false;
    uint64_t m_kingQueenSwapSquaresMask64 = 0;
    uint64_t m_kingQueenSwapLiftedMask64 = 0;
    uint64_t m_kingQueenSwapInitialEmptyMask64 = 0;
    bool m_kingQueenSwapReadyToExit = false;

    // Moved-squares overlay tracking.
    uint64_t m_movedSquaresMask = 0;
    uint64_t m_movedSquaresMaskBeforeLastAcknowledgedHumanMove = 0;
    uint32_t m_movedSquaresSnapshotActionToken = 0U;
};

} // namespace runtime
