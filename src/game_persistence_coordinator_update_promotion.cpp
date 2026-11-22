#include <cstring>

#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include "modules/draw_rules.hpp"
#include "modules/game_persistence_coordinator_update_internal.hpp"
#include "modules/game_persistence_engine_helpers.hpp"
#include "modules/persisted_side.hpp"
#include "modules/promotion_helpers.hpp"
#include "modules/vs_cpu_rng.hpp"
#include "trace/trace_recorder.hpp"

namespace modules {

namespace {

constexpr bool IsGameOver(const EngineDerivedState& d) {
    return d.inCheckmate || d.inDraw || d.inStalemate;
}

bool SafeGetFen(IEngine& engine, FenString& outFen) {
    return game_persistence_engine_helpers::SafeGetFen(
        engine, outFen, true);
}

} // namespace

void GamePersistenceCoordinator::UpdatePromotionChoiceIfPending() {
    if (!m_promotionPending) {
        return;
    }

    // If a promotion choice is pending, consume it and apply the deferred move.
    UpdateLegalDestinationsMask();
    PromotionChoice choice;
    if (!m_stateMachine.ConsumePromotionChoice(choice)) {
        return;
    }

    const runtime::TaggedInteractionState& interaction =
        m_stateMachine.GetChessInteractionModel().phase;
    const runtime::PhysicalTransitionPlan& promotion = interaction.plan;
    const bool pendingPromotion =
        interaction.tag == runtime::ChessInteractionPhase::AwaitingAction &&
        promotion.kind == runtime::PhysicalTransitionKind::Promotion &&
        promotion.source == runtime::PhysicalTransitionSource::Human;
    const bool pendingPromotionUndo =
        choice.wasCancelled &&
        interaction.tag == runtime::ChessInteractionPhase::AwaitingAction &&
        promotion.kind == runtime::PhysicalTransitionKind::Undo &&
        promotion.source == runtime::PhysicalTransitionSource::Undo;
    if ((!pendingPromotion && !pendingPromotionUndo) ||
        promotion.moveFrom >= 64U || promotion.moveTo >= 64U) {
        m_promotionPending = false;
        DispatchPendingInteractionActionResult(false);
        return;
    }

    if (choice.wasCancelled) {
        uint8_t fields[UNDO_HISTORY_FIELDS_PER_MOVE] = {};
        game_persistence_update_internal::BuildUndoPersistFieldsFromFen(
            m_cachedFen.CStr(),
            m_engine.GetSideToMove(),
            promotion.moveFrom,
            promotion.moveTo,
            pendingPromotion ? promotion.removalRequired
                             : promotion.restorationRequired,
            pendingPromotion ? promotion.removalSquare
                             : promotion.restorationSquare,
            0U,
            fields);
        m_stateMachine.ConfigureUndoPromptMetadata(
            PersistedSideFromU8(fields[0]),
            static_cast<char>(fields[1]),
            static_cast<char>(fields[2]),
            fields[3]);
        m_promotionPending = false;
        m_lastUndoEntryPromotionPending = false;
        DispatchPendingInteractionActionResult(true);
        ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
        (void)PersistSnapshotDeferredWithRetries("promotion_cancelled", false);
        return;
    }

    (void)CommitPromotionMove(promotion, choice.piece, /*automatic=*/false);
}

bool GamePersistenceCoordinator::CommitPromotionMove(
    const runtime::PhysicalTransitionPlan& promotion,
    const PromotionPiece piece, const bool automatic) {
    // Both automatic and selected promotions use the same physical action,
    // engine/history/clock transaction and acknowledgement owner.
    // The claimed action owns the completed plan; a next-side pickup may
    // already await replay after acknowledgement. Manual choices have their
    // separate occupancy revalidation in ConsumePromotionChoice.
    const char promo = PromotionPieceToUciChar(piece);
    const runtime::PromotionConfirmation confirmation{
        .square = promotion.moveTo,
        .piece = piece,
        .side = m_engine.GetSideToMove() == SideToMove::White
                    ? PersistedSide::White : PersistedSide::Black,
        .automatic = automatic,
    };

    UciMove uci;
    if (!MakeUciMove(promotion.moveFrom, promotion.moveTo, promo, uci)) {
        m_promotionPending = false;
        DispatchPendingInteractionActionResult(false);
        return false;
    }

    if (IsDuckGame()) {
        m_promotionPending = false;
        m_lastUndoEntryPromotionPending = false;
        if (!StageDuckChessMove(promotion, promo)) {
            return false;
        }
        m_stateMachine.ShowPromotionConfirmation(confirmation);
        return true;
    }

    uint8_t fields[UNDO_HISTORY_FIELDS_PER_MOVE] = {};
    game_persistence_update_internal::BuildUndoPersistFieldsFromFen(
        m_cachedFen.CStr(),
        m_engine.GetSideToMove(),
        promotion.moveFrom,
        promotion.moveTo,
        promotion.removalRequired,
        promotion.removalSquare,
        static_cast<uint8_t>(promo),
        fields);
    const uint32_t nextHalfmoveClock = modules::NextHalfmoveClockPlies(
        m_cachedFen.CStr(),
        m_halfmoveClockPlies,
        promotion.moveFrom,
        promotion.moveTo,
        promotion.removalRequired,
        promotion.removalSquare);
    PrepareTakebackGrace(promotion);
    if (!m_engine.ApplyUciMove(uci.CStr())) {
        ClearTakebackGrace();
        logger::Warn("engine: ApplyUciMove failed (%s) during promotion", uci.CStr());
        m_promotionPending = false;
        m_lastUndoEntryPromotionPending = false;
        DispatchPendingInteractionActionResult(false);
        return false;
    }

    (void)AppendUndoHistoryEntry(fields[0], fields[1], fields[2], fields[3],
                                 fields[4], fields[5], fields[6]);
    m_halfmoveClockPlies = nextHalfmoveClock;
    m_vsCpuRngSeed = NextVsCpuSeedAfterMove(m_vsCpuRngSeed, uci.CStr());
    m_promotionPending = false;
    m_lastUndoEntryPromotionPending = false;
    DispatchPendingInteractionActionResult(true);

    (void)SafeGetFen(m_engine, m_cachedFen);
    (void)PushFenHistory(m_cachedFen.CStr(), m_halfmoveClockPlies);
    ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());

    UpdateLastMovePulseFromUndoHistory();
    m_stateMachine.ShowPromotionConfirmation(confirmation);

    if (m_traceRecorder && m_traceRecorder->IsActive()) {
        m_traceRecorder->RecordCheckpointEngine(m_cachedFen.CStr());
    }

    m_engineDerived.RefreshFromEngine(m_engine);
    RefreshDrawStateFromTracking();

    // Promotion may end the game; ensure VS-CPU is fully quiesced.
    if (IsGameOver(m_engineDerived)) {
        ClearPendingCpuMoveSelection();
        if (m_vsCpuThinking) {
            QuiesceAllThinking();
        }
        m_stateMachine.CancelQueuedCpuMove();
    }

    CommitGameClockMove(ToClockSide(m_engineDerived.sideToMove));
    ArmPreparedTakebackGrace();
    (void)PersistSnapshotDeferredWithRetries("promotion_commit", false);
    return true;
}

} // namespace modules
