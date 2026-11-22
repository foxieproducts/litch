#include "runtime/progress_watchdog.hpp"
#include "modules/litch_max_engine.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>


#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include <litch_max/fen_constants.hpp>
#include "modules/vs_cpu_difficulty.hpp"

namespace modules {

namespace {

constexpr uint32_t DEFAULT_HASH_SEED = 1;
uint32_t NormalizeHashSeed(const uint32_t seed) {
    return (seed == 0) ? DEFAULT_HASH_SEED : seed;
}

} // namespace

bool LitchMaxEngine::IsThinkTimeUp() {
    if (m_timeUp.load(std::memory_order_relaxed)) {
        return true;
    }
    const uint32_t maxThinkMs = m_lastThinkCfg.maxThinkMs;
    if (maxThinkMs == 0U) {
        return false;
    }
    if (m_thinkElapsed.HasElapsed(maxThinkMs)) {
        m_timeUp.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void LitchMaxEngine::ClearContinuousAnalysis() {
    m_continuousAnalysisFen.Clear();
    m_continuousCandidate = ThinkCandidate{};
    m_continuousAnalysisDuckSquare = 255U;
    m_hintCalculationElapsed = fxe::ElapsedTime{};
    m_continuousCalculatedMs = 0U;
    m_lastCalculationMs = 0U;
    m_continuousCandidatePending = false;
    m_continuousCandidateReplayed = false;
    m_continuousResultReleased = false;
}

bool LitchMaxEngine::HintCalculationMinimumReached() const {
    const uint32_t minimum = m_lastThinkCfg.minimumCalculationMs;
    return m_continuousCalculatedMs >= minimum ||
        (m_hintCalculationElapsed.Started() &&
         m_hintCalculationElapsed.HasElapsed(minimum - m_continuousCalculatedMs));
}

void LitchMaxEngine::PublishContinuousCandidate() {
    if (!m_lastThinkCfg.continuousAnalysis || !m_continuousCandidatePending ||
        m_cancelRequested.load(std::memory_order_relaxed) ||
        (!m_continuousResultReleased && !HintCalculationMinimumReached()) ||
        m_continuousCandidate.source == ThinkCandidateSource::Seed ||
        !m_continuousCandidate.scoreValid) return;
    auto candidate = m_continuousCandidate;
    candidate.replayed = m_continuousCandidateReplayed;
    m_continuousCandidatePending = false;
    m_lastThinkCfg.candidateObserver.Publish(candidate);
}

void LitchMaxEngine::ContinuousProgress(void* userdata, const uint32_t nodes) {
    auto& self = *static_cast<LitchMaxEngine*>(userdata);
    self.m_lastThinkCfg.progressObserver.Publish(nodes);
    self.PublishContinuousCandidate();
}

void LitchMaxEngine::Reset() {
    m_hashSeed = DEFAULT_HASH_SEED;
    m_game.Reset(m_hashSeed);
    m_thinking = false;
    m_thinkMode = ThinkMode::Classic;
    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_thinkElapsed = fxe::ElapsedTime();
    m_lastThinkNodeCount = 0;
    ClearContinuousAnalysis();
    m_lastClassicThinkResult = ClassicThinkResult{};
    m_lastClassicThinkResultValid = false;
    m_lastThinkCfg = ThinkConfig{};
    m_duckThinkFen.Clear();
    m_duckThinkSquare = 255U;
    m_lastDuckThinkResult = DuckThinkResult{};
    m_lastDuckThinkResultValid = false;
}

bool LitchMaxEngine::SetFen(const char* fen) {
    if (!m_game.SetFen(fen, m_hashSeed)) return false;
    m_lastClassicThinkResult = ClassicThinkResult{};
    m_lastClassicThinkResultValid = false;
    ClearContinuousAnalysis();
    return true;
}

bool LitchMaxEngine::GetFen(char* outFen, const size_t outSize) const {
    return m_game.GetFen(outFen, outSize);
}

SideToMove LitchMaxEngine::GetSideToMove() const { return m_game.GetSideToMove(); }

LitchMaxEngine::PieceSide LitchMaxEngine::GetPieceSideAtSquare(const uint8_t square) const {
    return m_game.GetPieceSideAtSquare(square);
}

bool LitchMaxEngine::IsInCheck(const SideToMove side) const { return m_game.IsInCheck(side); }
bool LitchMaxEngine::IsInCheckmate(const SideToMove side) const { return m_game.IsInCheckmate(side); }
bool LitchMaxEngine::IsInStalemate(const SideToMove side) const { return m_game.IsInStalemate(side); }

bool LitchMaxEngine::GetLegalDestinationsMask(const uint8_t fromSquare, uint64_t& outDestMask64) {
    outDestMask64 = 0ULL;
    if (m_thinking) return false;
    return m_game.GetLegalDestinationsMask(fromSquare, outDestMask64);
}

bool LitchMaxEngine::StartThink(const ThinkConfig& cfg) {
    if (m_thinking) {
        return false;
    }

    if (m_continuousAnalysisDuckSquare < 64U) {
        ClearContinuousAnalysis();
    }
    if (cfg.continuousAnalysis) {
        char fen[litch_max::FEN_BUFFER_CAPACITY] = {};
        if (!GetFen(fen, sizeof(fen))) {
            return false;
        }
        if (!m_continuousAnalysisFen.Equals(fen)) {
            ClearContinuousAnalysis();
            if (!m_continuousAnalysisFen.Assign(fen)) {
                return false;
            }
        }
    }

    const uint32_t desiredSeed = NormalizeHashSeed(cfg.rngSeed);
    if (desiredSeed != m_hashSeed) {
        char fen[litch_max::FEN_BUFFER_CAPACITY];
        std::memset(fen, 0, sizeof(fen));
        const bool hadFen = GetFen(fen, sizeof(fen));

        m_hashSeed = desiredSeed;
        if (hadFen) {
            (void)m_game.SetFen(fen, m_hashSeed);
        } else {
            m_game.Reset(m_hashSeed);
        }
    }
    m_game.PrepareSearch();

    (void)GetImmediateThinkCandidate(m_initialCandidate);
    if (!cfg.continuousAnalysis && cfg.completedRootFloor.IsAvailable() &&
        cfg.completedRootFloor.source == ThinkCandidateSource::Search &&
        cfg.completedRootFloor.scoreValid) m_initialCandidate = cfg.completedRootFloor;
    if (cfg.continuousAnalysis && !m_continuousCandidate.IsAvailable()) {
        m_continuousCandidate = m_initialCandidate;
    }
    m_thinking = true;
    m_thinkMode = ThinkMode::Classic;
    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_timeUp.store(false, std::memory_order_relaxed);
    m_lastThinkNodeCount = 0;
    m_lastStopReason = ThinkStopReason::Unavailable;
    m_lastClassicThinkResult = ClassicThinkResult{};
    m_lastClassicThinkResultValid = false;

    m_thinkElapsed.Reset();
    m_lastThinkCfg = cfg;
    m_lastDuckThinkResultValid = false;
    m_hintCalculationElapsed = fxe::ElapsedTime{};
    m_lastCalculationMs = 0U;
    ThinkCandidate immediate = cfg.continuousAnalysis && m_continuousCandidate.IsAvailable()
        ? m_continuousCandidate : m_initialCandidate;
    immediate.replayed = immediate.replayed ||
        (cfg.continuousAnalysis && immediate.source != ThinkCandidateSource::Seed);
    if (cfg.continuousAnalysis) {
        m_continuousCandidatePending = immediate.source != ThinkCandidateSource::Seed;
        m_continuousCandidateReplayed = true;
        PublishContinuousCandidate();
    } else if (immediate.IsAvailable()) cfg.candidateObserver.Publish(immediate);

    return true;
}

void LitchMaxEngine::CancelThink() {
    m_cancelRequested.store(true, std::memory_order_relaxed);
}

ThinkState LitchMaxEngine::PollThinkProgress(uint8_t& outProgressPercent,
                                           UciMove& outBestMoveUci) {
    outProgressPercent = 0;
    outBestMoveUci.Clear();

    if (!m_thinking) {
        return ThinkState::Idle;
    }

    if (m_cancelRequested.load(std::memory_order_relaxed)) {
        m_thinking = false;
        m_lastStopReason = ThinkStopReason::Canceled;
        return ThinkState::Canceled;
    }

    if (m_lastThinkCfg.continuousAnalysis) m_hintCalculationElapsed.Reset();
    const uint32_t priorCalculationMs = m_continuousCalculatedMs;
    ThinkState state = m_thinkMode == ThinkMode::Duck
        ? PollDuckThinkProgress(outProgressPercent, outBestMoveUci)
        : PollClassicThinkProgress(outProgressPercent, outBestMoveUci);
    if (m_lastThinkCfg.continuousAnalysis) {
        const auto releaseElapsed = m_hintCalculationElapsed;
        const uint32_t elapsed = m_hintCalculationElapsed.ElapsedMs();
        m_lastCalculationMs = elapsed;
        m_continuousCalculatedMs = elapsed > UINT32_MAX - m_continuousCalculatedMs
            ? UINT32_MAX : m_continuousCalculatedMs + elapsed;
        m_hintCalculationElapsed = fxe::ElapsedTime{};
        if (state == ThinkState::Done && m_lastStopReason == ThinkStopReason::Exhausted &&
            m_continuousCandidatePending && m_continuousCandidate.scoreValid &&
            m_continuousCandidate.source != ThinkCandidateSource::Seed &&
            !HintCalculationMinimumReached()) {
            // The search genuinely ended. Hold its scored result in the same
            // owner until 500 ms; do no more node work and exclude this wait above.
            m_thinking = true;
            const uint32_t releaseAfterMs = m_lastThinkCfg.minimumCalculationMs - priorCalculationMs;
            while (!releaseElapsed.HasElapsed(releaseAfterMs) &&
                   !m_cancelRequested.load(std::memory_order_relaxed) && !IsThinkTimeUp()) {
                runtime::DelayDiagnosticSlice(1U);
            }
            m_thinking = false;
            if (m_cancelRequested.load(std::memory_order_relaxed)) {
                m_lastStopReason = ThinkStopReason::Canceled;
                outBestMoveUci.Clear();
                state = ThinkState::Canceled;
            } else if (!IsThinkTimeUp()) {
                m_continuousResultReleased = true;
            } else {
                m_lastStopReason = ThinkStopReason::Deadline;
            }
        }
        PublishContinuousCandidate();
    }
    return state;
}

bool LitchMaxEngine::GetLastThinkStats(ThinkStats& outStats) const {
    outStats.nodeCount = m_lastThinkNodeCount;
    outStats.calculationMs = m_lastCalculationMs;
    outStats.calculationMsValid = m_lastThinkCfg.continuousAnalysis;
    outStats.stopReason = m_lastStopReason;
    return true;
}

bool LitchMaxEngine::GetLastClassicThinkResult(
    ClassicThinkResult& outResult) const {
    if (!m_lastClassicThinkResultValid) {
        outResult = ClassicThinkResult{};
        return false;
    }
    outResult = m_lastClassicThinkResult;
    return true;
}

} // namespace modules
