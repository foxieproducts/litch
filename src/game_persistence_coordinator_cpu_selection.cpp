#include "modules/game_persistence_coordinator.hpp"
#include <litch_max/chess_notation.hpp>
#include <litch_max/duck_chess_rules.hpp>
#include <litch_max/duck_chess_search.hpp>
#include "modules/game_persistence_coordinator_update_internal.hpp"
#include "modules/game_persistence_coordinator_vs_cpu_internal.hpp"

namespace modules {

void GamePersistenceCoordinator::ClearPendingCpuMoveSelection() {
    m_cpuSeed = ThinkCandidate{};
    m_pendingCpuMoveUciValid = false;
    m_pendingCpuMoveFrom = 0U;
    m_pendingCpuMoveTo = 0U;
    m_pendingCpuMoveCaptureSquareValid = false;
    m_pendingCpuMoveCaptureSquare = 0U;
    m_pendingCpuMoveUci.Clear();
    m_pendingCpuDuckDestinationValid = false;
    m_pendingCpuDuckDestination = INVALID_DUCK_SQUARE;
}

void GamePersistenceCoordinator::PrepareImmediateDecision(
    const ThinkConfig& config, const ThinkRequestKind kind) {
    // Human guidance requires completed calculation; the immediate legal seed
    // remains the CPU's recovery decision only.
    if (kind == ThinkRequestKind::Hint) return;
    if (kind == ThinkRequestKind::Cpu) m_cpuSeed = ThinkCandidate{};
    ThinkCandidate seed;
    if (kind == ThinkRequestKind::Cpu && config.completedRootFloor.IsAvailable() &&
        config.completedRootFloor.source == ThinkCandidateSource::Search &&
        config.completedRootFloor.scoreValid) {
        // A qualified exact-root floor also survives foreground admission failure.
        seed = config.completedRootFloor;
    } else if (IsDuckGame()) {
        litch_max::DuckChessTurn turn;
        litch_max::ParsedUciMove parsed;
        if (!litch_max::FindImmediateDuckChessTurn(m_cachedFen.CStr(), m_duckSquare, turn) ||
            !litch_max::TryParseUciMove(turn.chessMove.CStr(), parsed)) return;
        seed.fromSquare = parsed.fromSquare;
        seed.toSquare = parsed.toSquare;
        seed.promotion = parsed.hasPromotion ? parsed.promotionSuffixLower : '\0';
        seed.isDuck = true;
        seed.duckDestination = turn.duckDestination;
        seed.capturedKing = turn.capturedKing;
        seed.score = turn.score;
        seed.source = ThinkCandidateSource::Seed;
        seed.rootKind = ThinkRootKind::Seed;
    } else if (!m_engine.GetImmediateThinkCandidate(seed)) {
        return;
    }
    if (kind == ThinkRequestKind::Cpu) {
        m_cpuSeed = seed;
    } else if (seed.isDuck) {
        (void)CacheLiveDuckHint(seed);
    } else {
        CacheHintMoveForCurrentPosition(seed.fromSquare, seed.toSquare);
        if (CanPublishHint(seed.fromSquare, seed.toSquare)) {
            m_stateMachine.SetHintMoveSquares(seed.fromSquare, seed.toSquare);
            RecordAcceptedHintDiagnostic();
        }
    }
    config.candidateObserver.Publish(seed);
}

bool GamePersistenceCoordinator::RetainImmediateCpuDecision() {
    if (!m_cpuSeed.IsAvailable()) return false;
    UciMove chosen;
    if (!litch_max::TryFormatUciMove(m_cpuSeed.fromSquare, m_cpuSeed.toSquare,
                         m_cpuSeed.promotion, chosen)) return false;
    litch_max::ParsedUciMove parsed;
    bool capture = false;
    uint8_t captureSquare = m_cpuSeed.toSquare;
    if (m_cpuSeed.isDuck) {
        litch_max::DuckChessMoveResult applied;
        if (!litch_max::TryParseUciMove(chosen.CStr(), parsed) ||
            !litch_max::TryApplyDuckMove(m_cachedFen.CStr(), m_duckSquare, chosen.CStr(), applied)) return false;
        capture = applied.captureSquareValid;
        captureSquare = applied.captureSquare;
    } else {
        UciMove selected;
        if (!game_persistence_vs_cpu_internal::TryChooseClassicCpuMove(
                m_engine, chosen, parsed, selected)) return false;
        chosen = selected;
        (void)game_persistence_update_internal::TryComputeCpuCaptureSquareAndIsCapture(
            m_cachedFen.CStr(), parsed.fromSquare, parsed.toSquare, captureSquare, capture);
    }
    m_pendingCpuMoveUciValid = true;
    m_pendingCpuMoveUci = chosen;
    m_pendingCpuMoveFrom = parsed.fromSquare;
    m_pendingCpuMoveTo = parsed.toSquare;
    m_pendingCpuMoveCaptureSquareValid = capture;
    m_pendingCpuMoveCaptureSquare = capture ? captureSquare : 0U;
    m_pendingCpuDuckDestinationValid = m_cpuSeed.isDuck && !m_cpuSeed.capturedKing;
    m_pendingCpuDuckDestination = m_cpuSeed.duckDestination;
    return true;
}

} // namespace modules
