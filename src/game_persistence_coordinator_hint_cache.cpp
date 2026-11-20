#include <initializer_list>

#include "modules/game_persistence_coordinator.hpp"

#include <litch_max/chess_notation.hpp>
#include "modules/beginner_mode_policy.hpp"
#include "modules/vs_cpu_turn.hpp"

namespace modules {

void GamePersistenceCoordinator::CacheHintMoveForCurrentPosition(const uint8_t fromSquare,
                                                                 const uint8_t toSquare) {
    m_currentCoachInsight = CoachInsight{};
    m_currentCoachInsightValid = false;
    if (fromSquare >= 64U || toSquare >= 64U || m_cachedFen.Empty()) {
        m_hintCachedMoveValid = false;
        m_hintCachedFen.Clear();
        m_hintCachedFromSquare = 0;
        m_hintCachedToSquare = 0;
        return;
    }

    m_hintCachedMoveValid = true;
    m_hintCachedFen = m_cachedFen;
    m_hintCachedFromSquare = fromSquare;
    m_hintCachedToSquare = toSquare;
    m_hintPendingStart = false;
    ResetHintRetry();
}

void GamePersistenceCoordinator::CaptureHintForCancelUndoRestore() {
    uint8_t fromSquare = 0;
    uint8_t toSquare = 0;
    bool haveHint = m_stateMachine.TryGetHintMoveSquares(fromSquare, toSquare);

    if (!haveHint && m_hintCachedMoveValid && m_hintCachedFen.Equals(m_cachedFen.View())) {
        fromSquare = m_hintCachedFromSquare;
        toSquare = m_hintCachedToSquare;
        haveHint = true;
    }

    if (!haveHint || m_cachedFen.Empty()) {
        m_cancelUndoHintMoveValid = false;
        m_cancelUndoHintFen.Clear();
        m_cancelUndoHintFromSquare = 0;
        m_cancelUndoHintToSquare = 0;
        return;
    }

    m_cancelUndoHintMoveValid = true;
    m_cancelUndoHintFen = m_cachedFen;
    m_cancelUndoHintFromSquare = fromSquare;
    m_cancelUndoHintToSquare = toSquare;
}

bool GamePersistenceCoordinator::TryGetCachedHintMoveForCurrentPosition(uint8_t& outFromSquare,
                                                                        uint8_t& outToSquare) const {
    if (m_hintCachedMoveValid && m_hintCachedFen.Equals(m_cachedFen.View())) {
        outFromSquare = m_hintCachedFromSquare;
        outToSquare = m_hintCachedToSquare;
        return true;
    }

    if (m_cancelUndoHintMoveValid && m_cancelUndoHintFen.Equals(m_cachedFen.View())) {
        outFromSquare = m_cancelUndoHintFromSquare;
        outToSquare = m_cancelUndoHintToSquare;
        return true;
    }

    return false;
}

void GamePersistenceCoordinator::ClearCachedHintResults() {
    m_stateMachine.RetireManualHintRequest();
    RetireThinkDiagnostics(ThinkRequestKind::Hint);
    m_hintCompletedDifficulty = 0U;
    m_hintAnalysisFen.Clear();
    m_hintAnalysisDuckSquare = INVALID_DUCK_SQUARE;
    m_hintInitialWindow = fxe::ElapsedTime{};
    m_hintSearchElapsed = fxe::ElapsedTime{};
    m_hintCalculatedMs = 0U;
    m_hintScannedNodes = 0U;
    m_lastHintThinkStatsValid = false;
    m_lastHintThinkElapsedMs = 0U;
    m_lastHintThinkNodeCount = 0U;
    m_lastHintThinkBestUci.Clear();
    m_hintCachedMoveValid = false;
    m_hintCachedFen.Clear();
    m_hintCachedFromSquare = 0U;
    m_hintCachedToSquare = 0U;
    m_currentCoachInsight = CoachInsight{};
    m_currentCoachInsightValid = false;
    m_duckHintTurnValid = false;
    m_duckHintFen.Clear();
    m_duckHintSquare = INVALID_DUCK_SQUARE;
    m_duckHintTurn = DuckThinkResult{};
    m_displayedDuckHintTurn = DuckThinkResult{};

    m_cancelUndoHintMoveValid = false;
    m_cancelUndoHintFen.Clear();
    m_cancelUndoHintFromSquare = 0U;
    m_cancelUndoHintToSquare = 0U;
    ResetHintRetry();
}

bool GamePersistenceCoordinator::TryGetCurrentCoachInsight(
    CoachInsight& outInsight) const {
    outInsight = CoachInsight{};
    if (!m_coachingHintActive ||
        !m_currentCoachInsightValid ||
        m_gameVariant != GameVariant::Classic ||
        !m_hintCachedMoveValid ||
        !m_hintCachedFen.Equals(m_cachedFen.View()) ||
        m_currentCoachInsight.fromSquare != m_hintCachedFromSquare ||
        m_currentCoachInsight.toSquare != m_hintCachedToSquare) {
        return false;
    }
    outInsight = m_currentCoachInsight;
    return true;
}

HomeAssistanceProjection
GamePersistenceCoordinator::BuildHomeAssistanceProjection(
    const HomeAssistanceRuntimeInputs& runtimeInputs) const {
    HomeAssistanceProjection projection;
    projection.valid = true;
    projection.hintRequested =
        m_stateMachine.IsHintDisplayRequested();
    projection.coachingForced =
        runtimeInputs.coachingForcedHintDisplay;

    const runtime::TaggedInteractionState& interaction =
        m_stateMachine.GetChessInteractionModel().phase;
    const bool humanDuckRelocation =
        m_gameVariant == GameVariant::Duck &&
        IsHumanDuckRelocation(interaction);
    const auto shown = m_stateMachine.GetVisibleHintPresentation();
    const uint8_t renderedFrom = shown.fromSquare;
    const uint8_t renderedTo = shown.toSquare;
    const uint8_t renderedDuckFrom = shown.duckFromSquare;
    const uint8_t renderedDuckTo = shown.duckToSquare;
    const bool renderedPrimary = renderedFrom < 64U && renderedTo < 64U;
    const bool renderedDuck = renderedDuckFrom < 64U && renderedDuckTo < 64U;
    const DuckThinkResult* shownDuckTurn = nullptr;
    if (!m_takebackGrace.hintOwned && renderedPrimary &&
        m_gameVariant == GameVariant::Duck && !humanDuckRelocation &&
        m_duckHintTurnValid && m_duckHintFen.Equals(m_cachedFen.View()) &&
        m_duckHintSquare == m_duckSquare) {
        // A held gesture may still own the outgoing refinement tuple.
        for (const auto* candidate : {&m_duckHintTurn, &m_displayedDuckHintTurn}) {
            litch_max::ParsedUciMove parsed;
            if (litch_max::TryParseUciMove(candidate->chessMove.CStr(), parsed) &&
                renderedFrom == parsed.fromSquare && renderedTo == parsed.toSquare &&
                (candidate->capturedKing ||
                 (renderedDuck && renderedDuckFrom == m_duckSquare &&
                  renderedDuckTo == candidate->duckDestination))) {
                shownDuckTurn = candidate;
                break;
            }
        }
    }
    const bool matchingDuckChessProgress =
        shownDuckTurn != nullptr && !shownDuckTurn->capturedKing &&
        IsMatchingDuckHintChessProgress(interaction, renderedFrom, renderedTo);
    const uint32_t vsCpuLevel = ReadVsCpuLevelSetting();
    const bool firstPlayerIsP2 =
        m_settings.ReadValue(
            "GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U;
    const bool cpuPlayerIsP2 =
        m_settings.ReadValue(
            "VS_CPU_PLAYER", static_cast<uint32_t>(2U)) == 2U;
    const bool humanTurn = !modules::IsVsCpuTurn(
        m_engineDerived.sideToMove,
        vsCpuLevel,
        firstPlayerIsP2,
        cpuPlayerIsP2);
    const bool foregroundSuppressed =
        runtimeInputs.menuOverlayActive ||
        runtimeInputs.quickSettingsOverlayActive ||
        runtimeInputs.gameResetHoldActive ||
        m_stateMachine.IsTextOverlayActive() ||
        m_stateMachine.IsPlacementModeActive() ||
        m_stateMachine.IsKingQueenSwapPromptActive() ||
        m_stateMachine.IsUndoPromptActive() ||
        m_stateMachine.IsPromotionActive() ||
        (m_stateMachine.IsMoveSettlingActive() && !matchingDuckChessProgress) ||
        m_stateMachine.HasQueuedCpuMove() ||
        m_stateMachine.HasHumanIllegal() ||
        m_stateMachine.HasCpuIllegal() ||
        m_promotionPending ||
        IsGameOverVariantAware() || IsKingResignationArmed() ||
        !humanTurn;
    bool cachedPrimary = false;
    if (!m_takebackGrace.hintOwned && renderedPrimary &&
        m_gameVariant != GameVariant::Duck) {
        uint8_t cachedFrom = runtime::CHESS_INTERACTION_INVALID_SQUARE;
        uint8_t cachedTo = runtime::CHESS_INTERACTION_INVALID_SQUARE;
        cachedPrimary = TryGetCachedHintMoveForCurrentPosition(cachedFrom, cachedTo) &&
            renderedFrom == cachedFrom && renderedTo == cachedTo;
    }

    const bool assistancePhase =
        interaction.tag == runtime::ChessInteractionPhase::Idle ||
        humanDuckRelocation || matchingDuckChessProgress;
    projection.manualAffordanceUseful =
        !foregroundSuppressed && !matchingDuckChessProgress && assistancePhase &&
        m_stateMachine.IsPhysicalOccupancyConsistentWithInteraction();

    if (!foregroundSuppressed && humanTurn &&
        interaction.tag == runtime::ChessInteractionPhase::PieceLifted &&
        interaction.selectedSquare < 64U &&
        m_stateMachine.GetValidDestMask() != 0ULL) {
        projection.presentation =
            HomeAssistancePresentation::LegalGuide;
        projection.legalSourceSquare = interaction.selectedSquare;
        projection.legalDestinationMask =
            m_stateMachine.GetValidDestMask();
        // Project the already-revealed target; search admission owns timing.
        if (projection.hintRequested && !m_takebackGrace.hintOwned &&
            renderedPrimary && renderedTo < 64U &&
            IsMatchingHumanHintPickup(interaction, renderedFrom) &&
            (projection.legalDestinationMask & (UINT64_C(1) << renderedTo)) != 0ULL) {
            projection.fromSquare = renderedFrom;
            projection.toSquare = renderedTo;
            if (shownDuckTurn != nullptr && !shownDuckTurn->capturedKing) {
                projection.duckFromSquare = renderedDuckFrom;
                projection.duckToSquare = renderedDuckTo;
            }
        }
        return projection;
    }

    if (foregroundSuppressed || !assistancePhase) {
        projection.presentation = HomeAssistancePresentation::Hidden;
        return projection;
    }
    if (!projection.hintRequested) {
        projection.presentation = HomeAssistancePresentation::Off;
        return projection;
    }

    bool ready = false;

    if (!m_takebackGrace.hintOwned &&
        m_gameVariant == GameVariant::Duck && humanDuckRelocation) {
        ready = m_pendingDuckTurn.valid &&
                m_pendingDuckTurn.requiredDuckDestination < 64U &&
                renderedPrimary && renderedFrom == m_duckSquare &&
                renderedTo ==
                    m_pendingDuckTurn.requiredDuckDestination;
        if (ready) {
            projection.presentation =
                HomeAssistancePresentation::DuckRelocation;
            projection.duckFromSquare = renderedFrom;
            projection.duckToSquare = renderedTo;
            return projection;
        }
    } else if (shownDuckTurn != nullptr) {
        if (shownDuckTurn->capturedKing) {
            projection.presentation =
                HomeAssistancePresentation::HintReady;
            projection.fromSquare = renderedFrom;
            projection.toSquare = renderedTo;
            return projection;
        }
        projection.presentation = matchingDuckChessProgress
            ? HomeAssistancePresentation::DuckRelocation
            : HomeAssistancePresentation::DuckCompound;
        if (!matchingDuckChessProgress) {
            projection.fromSquare = renderedFrom;
            projection.toSquare = renderedTo;
        }
        projection.duckFromSquare = renderedDuckFrom;
        projection.duckToSquare = renderedDuckTo;
        return projection;
    } else if (!m_takebackGrace.hintOwned &&
               m_gameVariant != GameVariant::Duck) {
        ready = cachedPrimary;
        if (ready) {
            projection.presentation = m_hintThinking
                ? HomeAssistancePresentation::HintRefreshing
                : HomeAssistancePresentation::HintReady;
            projection.fromSquare = renderedFrom;
            projection.toSquare = renderedTo;
            return projection;
        }
    }

    projection.presentation = m_stateMachine.IsHintThinkingActive()
                                  ? HomeAssistancePresentation::Thinking
                                  : HomeAssistancePresentation::Pending;
    return projection;
}

} // namespace modules
