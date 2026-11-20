#include "modules/litch_max_engine.hpp"

#include <algorithm>
#include <limits>
#include <cstring>

#include <litch_max/chess_notation.hpp>

#include "logger.hpp"
#include "modules/classic_search_config.hpp"
#include <litch_max/classic_chess_evaluation.hpp>
#include <litch_max/classic_chess_opening_book.hpp>
#include "runtime/progress_watchdog.hpp"
#include "modules/litch_max_search_policy.hpp"

namespace modules {
namespace {
constexpr uint32_t PONDER_MAX_THINK_MS = 60000U;

char PromotionChar(const uint8_t piece) {
    switch (piece) {
        case MCUMAX_QUEEN: return 'q';
        case MCUMAX_ROOK: return 'r';
        case MCUMAX_BISHOP: return 'b';
        case MCUMAX_KNIGHT: return 'n';
        default: return '\0';
    }
}

bool MoveToUci(const mcumax_position_move move, UciMove& out) {
    if ((move.from & 0x88U) != 0U || (move.to & 0x88U) != 0U) return false;
    const char text[]{static_cast<char>('a' + (move.from & 7U)),
                      static_cast<char>('8' - (move.from >> 4U)),
                      static_cast<char>('a' + (move.to & 7U)),
                      static_cast<char>('8' - (move.to >> 4U)),
                      PromotionChar(move.promotion), '\0'};
    return out.Assign(text);
}

bool BookFromContext(const mcumax_position& root, const ThinkConfig& config,
                     litch_max::classic_chess::BookMove& out) {
    static_assert(THINK_OPENING_MAX_PLIES == litch_max::classic_chess::OPENING_BOOK_MAX_PLIES);
    if (!config.openingPrefix.fromCanonicalStart ||
        config.openingPrefix.moveCount > THINK_OPENING_MAX_PLIES) return false;
    mcumax_position_move moves[THINK_OPENING_MAX_PLIES]{};
    for (uint8_t ply = 0U; ply < config.openingPrefix.moveCount; ++ply) {
        const auto move = config.openingPrefix.moves[ply];
        if (move.fromSquare >= 64U || move.toSquare >= 64U || move.promotion != '\0') return false;
        moves[ply] = {static_cast<uint8_t>((move.fromSquare / 8U) * 16U + move.fromSquare % 8U),
                      static_cast<uint8_t>((move.toSquare / 8U) * 16U + move.toSquare % 8U), 0U};
    }
    const litch_max::classic_chess::OpeningBookHistory history{moves, config.reversibleHalfmoves,
        static_cast<uint16_t>(1U + config.openingPrefix.moveCount / 2U),
        config.openingPrefix.moveCount, true};
    return litch_max::classic_chess::TryOpeningBook(root, history, out);
}

uint8_t MoveFacts(mcumax_position position, const mcumax_position_move move) {
    mcumax_position_undo undo{};
    if (!mcumax_position_make(&position, move, &undo)) return 0U;
    uint8_t facts = ClassicThinkOutcomeKnown;
    if (mcumax_position_is_check(&position, position.side)) {
        facts |= ClassicThinkGivesCheck;
        if (!mcumax_position_any_legal(&position)) facts |= ClassicThinkGivesMate;
    }
    return facts;
}
} // namespace

bool LitchMaxEngine::GetImmediateThinkCandidate(ThinkCandidate& out) {
    out = ThinkCandidate{};
    if (m_thinking) return false;
    mcumax_position root{};
    if (!m_game.Import(root)) return false;
    return litch_max::classic_chess::FindImmediateCandidate(root, out);
}

bool LitchMaxEngine::StartPonder(const char* predictedHumanMove, const ThinkConfig& config) {
    if (m_thinking || config.continuousAnalysis || config.difficulty < 3U ||
        config.difficulty > 11U || config.maxThinkMs == 0U ||
        config.maxThinkMs > PONDER_MAX_THINK_MS) return false;
    litch_max::ParsedUciMove parsed;
    if (!litch_max::TryParseUciMove(predictedHumanMove, parsed) ||
        !m_game.Import(m_benchmarkRoot)) return false;
    const uint8_t promotion = !parsed.hasPromotion ? 0U :
        parsed.promotionSuffixLower == 'q' ? MCUMAX_QUEEN :
        parsed.promotionSuffixLower == 'r' ? MCUMAX_ROOK :
        parsed.promotionSuffixLower == 'b' ? MCUMAX_BISHOP : MCUMAX_KNIGHT;
    const mcumax_position_move move{
        static_cast<uint8_t>((parsed.fromSquare / 8U) * 16U + parsed.fromSquare % 8U),
        static_cast<uint8_t>((parsed.toSquare / 8U) * 16U + parsed.toSquare % 8U), promotion};
    mcumax_position_undo undo{};
    if (!mcumax_position_make(&m_benchmarkRoot, move, &undo) ||
        !mcumax_position_any_legal(&m_benchmarkRoot)) return false;
    m_initialCandidate = {};
    (void)litch_max::classic_chess::FindImmediateCandidate(m_benchmarkRoot, m_initialCandidate);
    m_lastThinkCfg = config;
    m_thinkMode = ThinkMode::Ponder;
    m_thinking = true;
    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_timeUp.store(false, std::memory_order_relaxed);
    m_lastStopReason = ThinkStopReason::Unavailable;
    m_lastThinkNodeCount = 0U;
    m_lastClassicThinkResultValid = false;
    m_thinkElapsed.Reset();
    return true;
}

bool LitchMaxEngine::PonderRootMatchesLive() const {
    if (m_thinking) return false;
    mcumax_position actual{};
    return m_game.Import(actual) &&
        actual.side == m_benchmarkRoot.side && actual.en_passant == m_benchmarkRoot.en_passant &&
        std::memcmp(actual.board, m_benchmarkRoot.board, sizeof(actual.board)) == 0 &&
        std::memcmp(actual.king_square, m_benchmarkRoot.king_square, sizeof(actual.king_square)) == 0;
}

bool LitchMaxEngine::ClassicSearchStopped(void* userdata) {
    runtime::FeedProgressWatchdog();
    auto& self = *static_cast<LitchMaxEngine*>(userdata);
    self.PublishContinuousCandidate();
    return self.m_cancelRequested.load(std::memory_order_relaxed) || self.IsThinkTimeUp();
}

void LitchMaxEngine::ClassicCandidateCompleted(void* userdata, const ThinkCandidate& candidate) {
    auto& self = *static_cast<LitchMaxEngine*>(userdata);
    if (self.m_cancelRequested.load(std::memory_order_relaxed)) return;
    if (self.m_lastThinkCfg.continuousAnalysis) {
        if (self.m_continuousCandidate.rootKind == ThinkRootKind::ClassicFull &&
            candidate.rootKind == ThinkRootKind::ClassicPreliminary) return;
        const uint8_t retainedDepth = self.m_continuousCandidate.rootKind == ThinkRootKind::ClassicPreliminary &&
            candidate.rootKind == ThinkRootKind::ClassicFull ? 0U : self.m_continuousCandidate.completedDepth;
        if (candidate.completedDepth < CLASSIC_MIN_HINT_PLIES ||
            candidate.completedDepth < (self.m_continuousCandidate.source == ThinkCandidateSource::Book
                ? self.m_continuousCandidate.offlineDepth : retainedDepth)) return;
        self.m_continuousCandidate = candidate;
        self.m_continuousCandidatePending = true;
        self.m_continuousCandidateReplayed = false;
        self.PublishContinuousCandidate();
        return;
    }
    if (self.m_thinkMode == ThinkMode::Benchmark) {
        self.m_lastThinkCfg.candidateObserver.Publish(candidate);
        return;
    }
    const auto& retained = self.m_initialCandidate;
    const bool retainedFull = retained.rootKind == ThinkRootKind::ClassicFull;
    const bool candidateFull = candidate.rootKind == ThinkRootKind::ClassicFull;
    if (retained.source != ThinkCandidateSource::Seed && retained.IsAvailable() &&
        ((retainedFull && !candidateFull) ||
         (retainedFull == candidateFull && candidate.completedDepth < retained.completedDepth))) return;
    self.m_initialCandidate = candidate;
    self.m_lastThinkCfg.candidateObserver.Publish(candidate);
}

ThinkState LitchMaxEngine::PollClassicThinkProgress(uint8_t& outProgressPercent,
                                                UciMove& outBestMoveUci) {
    if (m_cancelRequested.load(std::memory_order_relaxed)) {
        m_thinking = false;
        return ThinkState::Canceled;
    }
    mcumax_position root{};
    const bool benchmark = m_thinkMode == ThinkMode::Benchmark;
    const bool fixedRoot = benchmark || m_thinkMode == ThinkMode::Ponder;
    if (fixedRoot) root = m_benchmarkRoot;
    if (!fixedRoot && !m_game.Import(root)) {
        m_thinking = false;
        return ThinkState::Error;
    }
    litch_max::classic_chess::BookMove book;
    const bool haveBook = !benchmark && BookFromContext(root, m_lastThinkCfg, book);
    if (haveBook && m_lastThinkCfg.continuousAnalysis && m_lastThinkCfg.minimumCalculationMs == 0U &&
        (!m_continuousCandidate.IsAvailable() || m_continuousCandidate.source == ThinkCandidateSource::Seed) &&
        !m_cancelRequested.load(std::memory_order_relaxed)) {
        ThinkCandidate candidate;
        candidate.fromSquare = (book.move.from >> 4U) * 8U + (book.move.from & 7U);
        candidate.toSquare = (book.move.to >> 4U) * 8U + (book.move.to & 7U);
        candidate.source = ThinkCandidateSource::Book;
        candidate.offlineDepth = book.offlineDepth;
        candidate.rootKind = ThinkRootKind::Book;
        candidate.score = book.offlineScore;
        candidate.scoreValid = true;
        candidate.scoreUnits = ThinkScoreUnits::ClassicPawn100;
        candidate.scorePerspective = root.side == MCUMAX_BOARD_WHITE ? SideToMove::White : SideToMove::Black;
        m_continuousCandidate = candidate;
        m_lastThinkCfg.candidateObserver.Publish(candidate);
    }
    auto search = ResolveClassicSearchConfig(m_lastThinkCfg);
#if defined(LITCH_ESP32)
    search.cooperate = &CooperateLitchMaxSearch;
#endif
    if (haveBook && (!m_lastThinkCfg.continuousAnalysis || m_lastThinkCfg.minimumCalculationMs != 0U)) search.rootPreferred = book.move;
    const auto& floor = m_lastThinkCfg.completedRootFloor;
    if (floor.IsAvailable() && floor.source == ThinkCandidateSource::Search) {
        uint8_t promotion = floor.promotion == 'q' ? MCUMAX_QUEEN : floor.promotion == 'r' ? MCUMAX_ROOK :
            floor.promotion == 'b' ? MCUMAX_BISHOP : floor.promotion == 'n' ? MCUMAX_KNIGHT : 0U;
        search.rootPreferred = {static_cast<uint8_t>((floor.fromSquare / 8U) * 16U + floor.fromSquare % 8U),
            static_cast<uint8_t>((floor.toSquare / 8U) * 16U + floor.toSquare % 8U), promotion};
    }
    search.shouldStop = &LitchMaxEngine::ClassicSearchStopped;
    search.stopContext = this;
    search.progressObserver = m_lastThinkCfg.continuousAnalysis
        ? ThinkProgressObserver{&LitchMaxEngine::ContinuousProgress, this} : m_lastThinkCfg.progressObserver;
    search.candidateObserver = {&LitchMaxEngine::ClassicCandidateCompleted, this,
        [](void* context, const ThinkCandidate& candidate) {
            static_cast<LitchMaxEngine*>(context)->m_lastThinkCfg.candidateObserver.RootCompleted(candidate);
        }};
    ThinkProfile effective = ThinkProfile::Requested(m_lastThinkCfg);
    effective.maxNodes = search.maxNodes;
    effective.maxDepth = search.maxDepth;
    effective.minimumDepth = search.minimumDepth;
    effective.firstRootNodeReserve = search.firstRootNodeReserve;
    m_lastThinkCfg.candidateObserver.ProfileResolved(effective);
    const auto result = m_searchWorkspace.Run(root, search);
    m_lastThinkNodeCount = result.nodes;
    m_thinking = false;
    if (m_cancelRequested.load(std::memory_order_relaxed)) {
        m_lastStopReason = ThinkStopReason::Canceled;
        return ThinkState::Canceled;
    }
    if (benchmark) {
        m_lastStopReason = m_timeUp.load(std::memory_order_relaxed) ? ThinkStopReason::Deadline :
            (result.interrupted && result.nodes >= search.maxNodes ? ThinkStopReason::NodeCap :
             (!result.available && !result.terminal ? ThinkStopReason::Error : ThinkStopReason::Exhausted));
        outProgressPercent = 100U;
        if (result.available) (void)MoveToUci(result.move, outBestMoveUci);
        // Deliberately omit novice move selection/facts and all live result caches.
        return m_lastStopReason == ThinkStopReason::Error ? ThinkState::Error : ThinkState::Done;
    }
    const ThinkCandidate& retained = m_lastThinkCfg.continuousAnalysis
        ? m_continuousCandidate : m_initialCandidate;
    const bool useRetained = !result.available ||
        (result.preliminaryOnly &&
         retained.rootKind == ThinkRootKind::ClassicFull) ||
        ((m_lastThinkCfg.continuousAnalysis || retained.rootKind == ThinkRootKind::ClassicFull || result.preliminaryOnly) && result.completedDepth <
            (retained.source == ThinkCandidateSource::Book ? retained.offlineDepth : retained.completedDepth));
    if (useRetained && !retained.IsAvailable()) return ThinkState::Error;
    const auto promotionPiece = [](const char promotion) -> uint8_t {
        switch (promotion) {
            case 'q': return MCUMAX_QUEEN;
            case 'r': return MCUMAX_ROOK;
            case 'b': return MCUMAX_BISHOP;
            case 'n': return MCUMAX_KNIGHT;
            default: return 0U;
        }
    };
    const mcumax_position_move move = useRetained
        ? mcumax_position_move{
            static_cast<uint8_t>((retained.fromSquare / 8U) * 16U + retained.fromSquare % 8U),
            static_cast<uint8_t>((retained.toSquare / 8U) * 16U + retained.toSquare % 8U),
            promotionPiece(retained.promotion)}
        : result.move;
    if (!MoveToUci(move, outBestMoveUci)) return ThinkState::Error;
    m_lastStopReason = m_timeUp.load(std::memory_order_relaxed) ? ThinkStopReason::Deadline :
        (result.interrupted && result.nodes >= search.maxNodes ? ThinkStopReason::NodeCap : ThinkStopReason::Exhausted);

    m_lastClassicThinkResult = ClassicThinkResult{};
    auto& candidate = m_lastClassicThinkResult.candidates[0];
    candidate.fromSquare = (move.from >> 4U) * 8U + (move.from & 7U);
    candidate.toSquare = (move.to >> 4U) * 8U + (move.to & 7U);
    candidate.promotion = PromotionChar(move.promotion);
    m_lastClassicThinkResult.candidateCount = 1U;
    m_lastClassicThinkResult.bestScore = useRetained ? retained.score : result.score;
    m_lastClassicThinkResult.source = useRetained ? retained.source : ThinkCandidateSource::Search;
    m_lastClassicThinkResult.offlineDepth = useRetained ? retained.offlineDepth : 0U;
    m_lastClassicThinkResult.bestScoreValid = true;
    m_lastClassicThinkResult.preliminaryOnly = useRetained
        ? retained.rootKind == ThinkRootKind::ClassicPreliminary : result.preliminaryOnly;
    m_lastClassicThinkResult.bestMoveFacts = MoveFacts(root, move);
    m_lastClassicThinkResultValid = true;
    outProgressPercent = 100U;
    if (m_lastThinkCfg.continuousAnalysis) {
        logger::Trace("classic_hint source=%s live_plies=%u offline_plies=%u nodes=%u horizon_nodes=%u",
            useRetained ? (retained.source == ThinkCandidateSource::Seed ? "seed" : "retained") : "search", static_cast<unsigned>(useRetained ? retained.completedDepth : result.completedDepth),
            static_cast<unsigned>(useRetained ? retained.offlineDepth : 0U), static_cast<unsigned>(result.nodes),
            static_cast<unsigned>(result.horizonNodes));
    }
    return ThinkState::Done;
}

} // namespace modules
