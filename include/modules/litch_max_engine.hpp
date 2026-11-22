#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/fixed_string.hpp"
#include "modules/engine.hpp"
#include "modules/engine_benchmark.hpp"
#include <litch_max/classic_chess_search.hpp>
#include <litch_max/classic_game.hpp>
#include <litch_max/fen_constants.hpp>

namespace modules {

class LitchMaxEngine final : public IEngine {
  public:
    LitchMaxEngine() = default;

    using PieceSide = litch_max::ClassicGame::PieceSide;

    void Reset() override;

    bool SetFen(const char* fen) override;

    bool GetFen(char* outFen, size_t outSize) const override;

    bool ApplyUciMove(const char* uciMove) override;

    SideToMove GetSideToMove() const override;

    bool IsInCheck(SideToMove side) const override;

    bool IsInCheckmate(SideToMove side) const override;

    bool IsInStalemate(SideToMove side) const override;

    bool GetLegalDestinationsMask(uint8_t fromSquare, uint64_t& outDestMask64) override;
    bool GetImmediateThinkCandidate(ThinkCandidate& out) override;

    bool StartThink(const ThinkConfig& cfg) override;
    bool StartBenchmark(uint8_t caseId, const ThinkConfig& cfg) override;
    bool StartPonder(const char* predictedHumanMove, const ThinkConfig& cfg) override;
    bool PonderRootMatchesLive() const override;

    bool StartDuckThink(const char* fen,
                        uint8_t duckSquare,
                        const ThinkConfig& cfg) override;

    void CancelThink() override;

    ThinkState PollThinkProgress(uint8_t& outProgressPercent,
                                 UciMove& outBestMoveUci) override;

    bool GetLastThinkStats(ThinkStats& outStats) const override;

    bool GetLastClassicThinkResult(ClassicThinkResult& outResult) const override;

    bool GetLastDuckThinkResult(DuckThinkResult& outResult) const override;

    // Lightweight rendering helper.
    // Returns the piece side at a logical square index (0=a8 .. 63=h1) based on
    // the engine's current position model.
    PieceSide GetPieceSideAtSquare(uint8_t square) const;

  private:
    static bool ClassicSearchStopped(void* userdata);
    static void ClassicCandidateCompleted(void* userdata, const ThinkCandidate& candidate);
    ThinkState PollClassicThinkProgress(uint8_t& outProgressPercent, UciMove& outBestMoveUci);
    static bool DuckSearchStopped(void* userdata);
    static void DuckCandidateCompletedCallback(void* userdata, const ThinkCandidate& candidate);
    static void ContinuousProgress(void* userdata, uint32_t nodes);
    bool HintCalculationMinimumReached() const;
    void PublishContinuousCandidate();
    void ClearContinuousAnalysis();

    bool IsThinkTimeUp();
    ThinkState PollDuckThinkProgress(uint8_t& outProgressPercent,
                                     UciMove& outBestMoveUci);
    enum class ThinkMode : uint8_t {
        Classic = 0,
        Duck,
        Benchmark,
        Ponder,
    };
    litch_max::ClassicGame m_game;
    bool m_thinking = false;
    ThinkMode m_thinkMode = ThinkMode::Classic;
    std::atomic<bool> m_cancelRequested{false};

    // Set when a think is stopped due to cfg.maxThinkMs.
    std::atomic<bool> m_timeUp{false};
    uint32_t m_hashSeed = 1;

    fxe::ElapsedTime m_thinkElapsed;
    uint32_t m_lastThinkNodeCount = 0;
    ThinkStopReason m_lastStopReason = ThinkStopReason::Unavailable;
    mcumax_position m_benchmarkRoot{};

    // A retry on the same position must not replace already published analysis
    // with the root's capture-ordering pass or a shallower completed search.
    fxe::FixedString<litch_max::FEN_BUFFER_CAPACITY> m_continuousAnalysisFen;
    ThinkCandidate m_continuousCandidate;
    ThinkCandidate m_initialCandidate;
    uint8_t m_continuousAnalysisDuckSquare = 255U;
    fxe::ElapsedTime m_hintCalculationElapsed;
    uint32_t m_continuousCalculatedMs = 0U;
    uint32_t m_lastCalculationMs = 0U;
    bool m_continuousCandidatePending = false;
    bool m_continuousCandidateReplayed = false;
    bool m_continuousResultReleased = false;

    ClassicThinkResult m_lastClassicThinkResult;
    bool m_lastClassicThinkResultValid = false;

    ThinkConfig m_lastThinkCfg{};
    litch_max::classic_chess::SearchWorkspace m_searchWorkspace;
    uint32_t m_duckLastYieldMs = 0U;
    fxe::FixedString<litch_max::FEN_BUFFER_CAPACITY> m_duckThinkFen;
    uint8_t m_duckThinkSquare = 255U;
    DuckThinkResult m_lastDuckThinkResult;
    bool m_lastDuckThinkResultValid = false;
};

} // namespace modules
