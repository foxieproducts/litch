#include "modules/game_persistence_coordinator.hpp"

#include <cstring>

#include "foxie_embedded/elapsed_time.hpp"
#include "logger.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/draw_rules.hpp"
#include <litch_max/duck_chess_rules.hpp>
#include "modules/game_persistence_coordinator_update_internal.hpp"
#include "modules/initial_fen.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"

namespace modules {
namespace {

constexpr uint64_t INITIAL_CHESS_OCCUPANCY =
    0xFFFF00000000FFFFULL;
constexpr uint64_t INITIAL_DUCK_CANDIDATE_MASK =
    0x0000FFFFFFFF0000ULL;
constexpr uint32_t DUCK_DETECTION_STABLE_MS = 200U;
constexpr uint8_t SETUP_READY_EVENT_BIT = 1U << 0U;

bool IsSingleSquare(const uint64_t mask) {
    return mask != 0ULL && (mask & (mask - 1ULL)) == 0ULL;
}

uint8_t SquareFromSingleMask(const uint64_t mask) {
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((mask & (1ULL << square)) != 0ULL) {
            return square;
        }
    }
    return INVALID_DUCK_SQUARE;
}

bool IsCanonicalFreshFen(const FenString& fen) {
    return !fen.Empty() && std::strcmp(fen.CStr(), INITIAL_FEN) == 0;
}

bool TryGetInitialDuckCandidate(const uint64_t physical,
                                uint8_t& outSquare) {
    outSquare = INVALID_DUCK_SQUARE;
    if ((physical & INITIAL_CHESS_OCCUPANCY) !=
        INITIAL_CHESS_OCCUPANCY) {
        return false;
    }
    const uint64_t extra = physical & ~INITIAL_CHESS_OCCUPANCY;
    if (!IsSingleSquare(extra) ||
        (extra & INITIAL_DUCK_CANDIDATE_MASK) == 0ULL) {
        return false;
    }
    outSquare = SquareFromSingleMask(extra);
    return outSquare < 64U;
}

// Setup-only identity classification. This intentionally does not activate the
// Duck game until the canonical 32-piece prerequisite in
// TryGetInitialDuckCandidate is also satisfied.
bool TryGetProvisionalSetupDuckCandidate(const uint64_t physical,
                                         uint8_t& outSquare) {
    outSquare = INVALID_DUCK_SQUARE;
    const uint64_t extra = physical & ~INITIAL_CHESS_OCCUPANCY;
    if (!IsSingleSquare(extra) ||
        (extra & INITIAL_DUCK_CANDIDATE_MASK) == 0ULL) {
        return false;
    }
    outSquare = SquareFromSingleMask(extra);
    return outSquare < 64U;
}

bool HasAllStandardSetupPieces(const uint64_t physical) {
    return (physical & INITIAL_CHESS_OCCUPANCY) == INITIAL_CHESS_OCCUPANCY;
}

GameWinner WinnerForSide(const SideToMove side) {
    return side == SideToMove::White ? GameWinner::White
                                     : GameWinner::Black;
}

} // namespace

bool GamePersistenceCoordinator::ShouldDeferInitialPlacementForVariantDetection(
    const uint64_t logicalPhysicalOccupancy) const {
    return ShouldValidateInitialVariantTransition(
        logicalPhysicalOccupancy);
}

bool GamePersistenceCoordinator::ShouldValidateInitialVariantTransition(
    const uint64_t logicalPhysicalOccupancy) const {
    if (!IsInitialVariantSelectionReversible()) {
        return false;
    }

    uint8_t candidate = INVALID_DUCK_SQUARE;
    if (m_gameVariant == GameVariant::Undecided) {
        if (TryGetInitialDuckCandidate(
                logicalPhysicalOccupancy, candidate)) {
            return true;
        }
        uint8_t priorCandidate = INVALID_DUCK_SQUARE;
        const runtime::ChessInteractionModel& interaction =
            m_stateMachine.GetChessInteractionModel();
        return logicalPhysicalOccupancy == INITIAL_CHESS_OCCUPANCY &&
               interaction.board.valid &&
               TryGetInitialDuckCandidate(
                   interaction.board.occupiedMask, priorCandidate);
    }

    return m_duckTurnPhase == DuckTurnPhase::ChessMove &&
           !runtime::IsEarlyDuckLiftAllowed(m_stateMachine.GetChessInteractionModel().phase) &&
           logicalPhysicalOccupancy == INITIAL_CHESS_OCCUPANCY;
}

bool GamePersistenceCoordinator::IsInitialVariantSelectionReversible() const {
    const bool variantEligible =
        (m_gameVariant == GameVariant::Undecided &&
         m_variantDetectionArmed) ||
        m_gameVariant == GameVariant::Duck;
    if (HasClockTerminalOutcome() ||
        !variantEligible || !IsCanonicalFreshFen(m_cachedFen) ||
        m_stateMachine.GetMovedSquaresMask() != 0ULL ||
        m_undoHistoryFieldCount != 0U ||
        m_pendingDuckTurn.valid || m_variantOutcome.IsTerminal()) {
        return false;
    }
    return m_gameVariant != GameVariant::Duck ||
           m_duckTurnPhase == DuckTurnPhase::ChessMove;
}

bool GamePersistenceCoordinator::IsInitialDuckRetirementObservation() const {
    if (m_gameVariant != GameVariant::Duck ||
        !IsInitialVariantSelectionReversible() ||
        m_initialDuckObstructionRelocationPending || m_duckSquare >= 64U) {
        return false;
    }
    const auto& interaction = m_stateMachine.GetChessInteractionModel();
    const uint64_t duckMask = 1ULL << m_duckSquare;
    const bool bootSnapshot = interaction.board.changedMask == 0ULL &&
        interaction.phase.tag == runtime::ChessInteractionPhase::Idle;
    return interaction.board.valid &&
           !runtime::IsEarlyDuckLiftAllowed(interaction.phase) &&
           HasAllStandardSetupPieces(interaction.board.occupiedMask) &&
           (bootSnapshot || (interaction.board.changedMask & duckMask) != 0ULL) &&
           (interaction.board.occupiedMask & duckMask) == 0ULL;
}

void GamePersistenceCoordinator::MarkInitialVariantValidationPending() {
    m_initialVariantValidationPending = true;
}

bool GamePersistenceCoordinator::IsInitialVariantValidationPending() const {
    const runtime::ChessInteractionModel& interaction =
        m_stateMachine.GetChessInteractionModel();
    return m_initialVariantValidationPending ||
           (interaction.board.valid &&
           ShouldValidateInitialVariantTransition(
               interaction.board.occupiedMask));
}

bool GamePersistenceCoordinator::TryTakeSetupPresentationEvent(
    SetupPresentationEvent& outEvent) {
    if ((m_pendingSetupPresentationEvents & SETUP_READY_EVENT_BIT) != 0U) {
        m_pendingSetupPresentationEvents &=
            static_cast<uint8_t>(~SETUP_READY_EVENT_BIT);
        outEvent = SetupPresentationEvent::Ready;
        return true;
    }
    outEvent = SetupPresentationEvent::None;
    return false;
}

void GamePersistenceCoordinator::CancelSetupPresentationEvents() {
    m_pendingSetupPresentationEvents = 0U;
}

uint8_t GamePersistenceCoordinator::GetInitialDuckCandidateSquare() const {
    const runtime::ChessInteractionModel& interaction =
        m_stateMachine.GetChessInteractionModel();
    if (!interaction.board.valid ||
        m_gameVariant != GameVariant::Undecided) {
        return INVALID_DUCK_SQUARE;
    }
    // Provisional Duck identity belongs only to fresh-board arrangement. A
    // chess destination must not become a yellow Duck while Classic latching
    // catches up with the settle/action boundary.
    const runtime::ChessInteractionPhase phase = interaction.phase.tag;
    if (phase != runtime::ChessInteractionPhase::Idle &&
        phase != runtime::ChessInteractionPhase::Correction) {
        return INVALID_DUCK_SQUARE;
    }
    if (m_setupDuckCandidateSquare < 64U &&
        (interaction.board.occupiedMask &
         (1ULL << m_setupDuckCandidateSquare)) != 0ULL) {
        return m_setupDuckCandidateSquare;
    }
    uint8_t candidate = INVALID_DUCK_SQUARE;
    if (!TryGetProvisionalSetupDuckCandidate(
            interaction.board.occupiedMask, candidate)) {
        return INVALID_DUCK_SQUARE;
    }
    return candidate;
}

bool GamePersistenceCoordinator::LatchGameVariant(
    const GameVariant variant,
    const uint8_t duckSquare) {
    if (m_gameVariant != GameVariant::Undecided ||
        (variant != GameVariant::Classic &&
         variant != GameVariant::Duck) ||
        (variant == GameVariant::Duck && duckSquare >= 64U)) {
        return false;
    }

    // Duck activation replaces orthodox search ownership and rebuilds the
    // physical projection. Leave the classification retryable until the
    // shared worker is completely out of the engine/watchdog lane.
    if (variant == GameVariant::Duck && !QuiesceAllThinking()) {
        m_cancelAllThinkingRequested = true;
        return false;
    }

    m_gameVariant = variant;
    m_duckSquare = variant == GameVariant::Duck
                       ? duckSquare
                       : INVALID_DUCK_SQUARE;
    m_duckTurnPhase = DuckTurnPhase::ChessMove;
    m_variantOutcome = VariantOutcome{};
    m_pendingDuckTurn = PendingDuckChessTurn{};
    m_stagedDuckChessUndoPending = false;
    m_externalizedDuckChessActionPending = false;
    m_externalizedDuckRelocationActionPending = false;
    m_externalizedDuckUndoActionPending = false;
    m_externalizedDuckRedoActionPending = false;
    m_externalizedDuckFixedActionValid = false;
    m_externalizedDuckRedoStartPending = false;
    m_externalizedDuckFrom = INVALID_DUCK_SQUARE;
    m_externalizedDuckTo = INVALID_DUCK_SQUARE;
    m_variantDetectionArmed = false;
    m_setupDuckCandidateSquare = INVALID_DUCK_SQUARE;
    if (variant == GameVariant::Duck) {
        m_stateMachine.CancelQueuedCpuMove();
        ClearPendingCpuMoveSelection();
        // Cancel work selected while the variant was undecided. The user's
        // VS-CPU setting remains authoritative; Duck-aware search will restart
        // with the compound move model on a later stable update.
        ClearCachedHintResults();
        ClearCachedLegalDestinations();
        m_stateMachine.SetHintDisplayRequested(false);
        m_stateMachine.ClearHintMove();
        ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
    }
    RecordVariantTraceCheckpoint();
    logger::Info("game: variant=%s duck=%u",
                 GameVariantToString(variant),
                 static_cast<unsigned>(m_duckSquare));
    (void)PersistSnapshotDeferredWithRetries("variant_latched", false);
    return true;
}

void GamePersistenceCoordinator::ResetGameVariantForNewGame() {
    ClearClockTerminalOutcome();
    m_gameVariant = GameVariant::Undecided;
    m_duckSquare = INVALID_DUCK_SQUARE;
    m_duckTurnPhase = DuckTurnPhase::ChessMove;
    m_variantOutcome = VariantOutcome{};
    m_pendingDuckTurn = PendingDuckChessTurn{};
    m_stagedDuckChessUndoPending = false;
    m_externalizedDuckChessActionPending = false;
    m_externalizedDuckRelocationActionPending = false;
    m_externalizedDuckUndoActionPending = false;
    m_externalizedDuckRedoActionPending = false;
    m_externalizedDuckFixedActionValid = false;
    m_externalizedDuckRedoStartPending = false;
    m_externalizedDuckFrom = INVALID_DUCK_SQUARE;
    m_externalizedDuckTo = INVALID_DUCK_SQUARE;
    m_variantDetectionArmed = true;
    m_initialVariantValidationPending = false;
    m_setupDuckCandidateSquare = INVALID_DUCK_SQUARE;
    m_pendingSetupPresentationEvents = 0U;
    m_setupPresentationEventObservationSequence = 0U;
    m_standardSetupWasComplete = false;
    m_hintInitialSetupObservedComplete = false;
    m_initialDuckObstructionRelocationPending = false;
    m_initialDuckObstructedFromSquare = INVALID_DUCK_SQUARE;
    m_externalizedInitialDuckRelocationActionPending = false;
    m_duckUndoRelocationPending = false;
    m_duckRedoRelocationPending = false;
    m_duckUndoRequestedCount = 0U;
}

void GamePersistenceCoordinator::RearmFreshGameAfterCompletedUndo() {
    if (!IsCanonicalFreshFen(m_cachedFen) ||
        m_undoHistoryFieldCount != 0U ||
        m_pendingUndoCommand.kind != UndoCommandKind::None ||
        m_stateMachine.IsUndoPromptActive()) {
        return;
    }

    RequestCancelAllThinking();
    ClearCachedHintResults();
    ClearCachedLegalDestinations();
    m_stateMachine.ResetForExternalEngineMutation();
    ResetGameVariantForNewGame();
    // Returning every move to Fresh Setup opens a new selectable game. Reuse
    // New Game's identity boundary so the prior variant cannot constrain it;
    // the existing settings transaction retains old disk authority on failure.
    // Untimed final Undo is a complete pregame boundary. Timed games retain
    // their accepted-play provenance and consumed clock until explicit New Game.
    const bool hasAcceptedMove = m_gameClock.IsEnabled() && m_gameHasAcceptedMove;
    (void)BeginNewGameHistory(false);
    m_gameHasAcceptedMove = hasAcceptedMove;
    m_stateMachine.ClearMovedSquares();
    m_stateMachine.ClearLastMoveForSide(PersistedSide::White);
    m_stateMachine.ClearLastMoveForSide(PersistedSide::Black);
    m_stateMachine.ClearLastMoveMostRecentSide();
    m_undoBaseFen = m_cachedFen;
    m_undoBaseMovedSquaresMask = 0ULL;
    ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
    if (!ShouldDeferInitialPlacementForVariantDetection(
            m_stateMachine.GetPhysicalOccupancyMask())) {
        (void)m_stateMachine.TryEnterPlacementMode();
    }
    m_stateMachine.TriggerInitialKingPlacementPrompt();
    RecordVariantTraceCheckpoint();
    logger::Info("game: final undo restored fresh start");
    (void)PersistSnapshotDeferredWithRetries("undo_to_fresh", false);
}

void GamePersistenceCoordinator::UpdateGameVariantActivation() {
    m_initialVariantValidationPending = false;
    const runtime::ChessInteractionModel& interaction =
        m_stateMachine.GetChessInteractionModel();
    const bool stable = interaction.board.valid &&
                        fxe::ElapsedTime(interaction.board.stableSinceMs)
                            .HasElapsed(DUCK_DETECTION_STABLE_MS);
    if (m_pendingSetupPresentationEvents != 0U &&
        interaction.board.valid &&
        interaction.board.changedMask != 0ULL &&
        interaction.board.sequence !=
            m_setupPresentationEventObservationSequence) {
        m_pendingSetupPresentationEvents = 0U;
    }
    const bool untouchedFreshSetup =
        IsCanonicalFreshFen(m_cachedFen) &&
        m_stateMachine.GetMovedSquaresMask() == 0ULL &&
        m_undoHistoryFieldCount == 0U;
    if (interaction.board.valid && untouchedFreshSetup) {
        uint8_t readyDuck = INVALID_DUCK_SQUARE;
        const uint64_t extra = interaction.board.occupiedMask &
                               ~INITIAL_CHESS_OCCUPANCY;
        const bool playableSetup =
            HasAllStandardSetupPieces(interaction.board.occupiedMask) &&
            (extra == 0ULL ||
             TryGetProvisionalSetupDuckCandidate(
                 interaction.board.occupiedMask, readyDuck));
        if (playableSetup && stable && !m_standardSetupWasComplete) {
            m_pendingSetupPresentationEvents |= SETUP_READY_EVENT_BIT;
            m_setupPresentationEventObservationSequence =
                interaction.board.sequence;
            m_standardSetupWasComplete = true;
        }
    }
    if (IsInitialDuckRetirementObservation()) {
        uint8_t replacementCandidate = INVALID_DUCK_SQUARE;
        // A replacement may already be present when the tracked Duck leaves.
        // Preserve it as the provisional setup candidate so the first
        // publishable Home snapshot cannot mislabel it as an unexpected extra.
        (void)TryGetInitialDuckCandidate(
            interaction.board.occupiedMask, replacementCandidate);
        (void)RetireInitialDuckForSetup(replacementCandidate);
        return;
    }

    if (!m_variantDetectionArmed ||
        m_gameVariant != GameVariant::Undecided) {
        return;
    }
    if (!IsCanonicalFreshFen(m_cachedFen) ||
        m_stateMachine.GetMovedSquaresMask() != 0ULL ||
        m_undoHistoryFieldCount != 0U) {
        (void)LatchGameVariant(
            GameVariant::Classic, INVALID_DUCK_SQUARE);
        return;
    }

    uint8_t candidate = INVALID_DUCK_SQUARE;
    const bool provisionalCandidate = interaction.board.valid &&
        TryGetProvisionalSetupDuckCandidate(
            interaction.board.occupiedMask, candidate);
    const bool establishedCandidatePresent =
        m_setupDuckCandidateSquare < 64U && interaction.board.valid &&
        (interaction.board.occupiedMask &
             (1ULL << m_setupDuckCandidateSquare)) != 0ULL;
    if (m_setupDuckCandidateSquare < 64U &&
        !establishedCandidatePresent) {
        m_setupDuckCandidateSquare = INVALID_DUCK_SQUARE;
    }
    if (provisionalCandidate &&
        m_setupDuckCandidateSquare >= 64U) {
        m_setupDuckCandidateSquare = candidate;
    }

    uint8_t activatableCandidate = INVALID_DUCK_SQUARE;
    const bool completeDuckSetup = interaction.board.valid && stable &&
        TryGetInitialDuckCandidate(
            interaction.board.occupiedMask, activatableCandidate);
    if (completeDuckSetup &&
        m_setupDuckCandidateSquare == activatableCandidate) {
        (void)LatchGameVariant(GameVariant::Duck, activatableCandidate);
        return;
    }

    const runtime::ChessInteractionPhase phase =
        interaction.phase.tag;
    // Variant selection stays reversible throughout untouched setup. Picking
    // up a chess piece and returning it is not a move and must not permanently
    // lock Classic. Lock only once a complete physical chess move has reached
    // its settle/action boundary.
    const bool firstChessMoveCompleted =
        phase == runtime::ChessInteractionPhase::Settling ||
        phase == runtime::ChessInteractionPhase::AwaitingAction;
    if (firstChessMoveCompleted) {
        (void)LatchGameVariant(
            GameVariant::Classic, INVALID_DUCK_SQUARE);
        return;
    }

}

bool GamePersistenceCoordinator::RestoreDuckGameState(
    const GamePersistenceState& persisted,
    const bool loadedFreshStart) {
    ResetGameVariantForNewGame();
    if (persisted.variantState.Empty()) {
        if (!loadedFreshStart || !IsCanonicalFreshFen(m_cachedFen)) {
            m_gameVariant = GameVariant::Classic;
            m_variantDetectionArmed = false;
        }
        return true;
    }

    DuckGameDurableState durable;
    if (!DecodeDuckGameDurableState(
            persisted.variantState.CStr(), durable)) {
        return false;
    }
    if (durable.variant == GameVariant::Undecided &&
        (!loadedFreshStart || !IsCanonicalFreshFen(m_cachedFen))) {
        return false;
    }
    if (durable.variant == GameVariant::Classic && !durable.outcome.IsTerminal() && loadedFreshStart &&
        IsCanonicalFreshFen(m_cachedFen)) {
        // Older builds locked Classic on the first pickup. With no completed
        // move or durable interaction remaining, setup is still reversible.
        // Reopen classification so a valid 33rd piece can enable Duck.
        return true;
    }
    uint64_t fenOccupancy = 0ULL;
    if (durable.variant == GameVariant::Duck &&
        (!FenToOccupancyMask(m_cachedFen.CStr(), fenOccupancy) ||
         (fenOccupancy & (1ULL << durable.duckSquare)) != 0ULL)) {
        return false;
    }

    m_gameVariant = durable.variant;
    m_duckSquare = durable.duckSquare;
    m_duckTurnPhase = durable.phase;
    m_variantOutcome = durable.outcome;
    m_variantTerminalStartMs = fxe::Millis();
    m_variantDetectionArmed =
        durable.variant == GameVariant::Undecided;
    if (!durable.pendingChessMoveValid) {
        return true;
    }

    UciMove uci;
    if (!MakeUciMove(durable.pendingMoveFrom,
                     durable.pendingMoveTo,
                     durable.pendingPromotionSuffix,
                     uci)) {
        return false;
    }
    litch_max::DuckChessMoveResult applied;
    if (!litch_max::TryApplyDuckMove(m_cachedFen.CStr(), m_duckSquare,
                          uci.CStr(), applied)) {
        return false;
    }
    m_pendingDuckTurn.valid = true;
    m_pendingDuckTurn.uci = uci;
    m_pendingDuckTurn.afterFen = applied.resultingFen;
    m_pendingDuckTurn.capturedKing = applied.capturedKing;
    m_pendingDuckTurn.winner = WinnerForSide(
        m_engine.GetSideToMove());
    game_persistence_update_internal::BuildUndoPersistFieldsFromFen(
        m_cachedFen.CStr(), m_engine.GetSideToMove(),
        durable.pendingMoveFrom, durable.pendingMoveTo,
        applied.captureSquareValid, applied.captureSquare,
        static_cast<uint8_t>(durable.pendingPromotionSuffix),
        m_pendingDuckTurn.undoFields);
    m_pendingDuckTurn.nextHalfmoveClockPlies =
        NextHalfmoveClockPlies(
            m_cachedFen.CStr(), m_halfmoveClockPlies,
            durable.pendingMoveFrom, durable.pendingMoveTo,
            applied.captureSquareValid, applied.captureSquare);
    return !applied.capturedKing;
}

bool GamePersistenceCoordinator::EncodeDuckGameState(
    GamePersistenceState& out) const {
    DuckGameDurableState durable;
    durable.variant = m_gameVariant;
    durable.duckSquare = m_duckSquare;
    durable.phase = m_duckTurnPhase;
    durable.outcome = m_variantOutcome;
    durable.pendingChessMoveValid = m_pendingDuckTurn.valid;
    if (m_pendingDuckTurn.valid) {
        const char* uci = m_pendingDuckTurn.uci.CStr();
        if (uci == nullptr || std::strlen(uci) < 4U ||
            !litch_max::TryParseUciSquare(uci, durable.pendingMoveFrom) ||
            !litch_max::TryParseUciSquare(uci + 2, durable.pendingMoveTo)) {
            return false;
        }
        durable.pendingPromotionSuffix =
            std::strlen(uci) >= 5U ? uci[4] : '\0';
    }
    char encoded[32] = {};
    return EncodeDuckGameDurableState(
               durable, encoded, sizeof(encoded)) &&
           out.variantState.Assign(encoded);
}

bool GamePersistenceCoordinator::IsGameOverVariantAware() const {
    if (HasClockTerminalOutcome() || m_variantOutcome.IsTerminal()) {
        return true;
    }
    if (m_gameVariant == GameVariant::Duck) {
        return m_variantOutcome.IsTerminal();
    }
    return m_engineDerived.inCheckmate || m_engineDerived.inDraw ||
           m_engineDerived.inStalemate;
}

} // namespace modules
