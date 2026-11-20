#pragma once

#include "modules/litch_max_engine.hpp"
#include "virtual_litch_session.hpp"

namespace test_support {

// Journey clocks deliberately stand still inside synchronous engine polling.
// Bound only continuous Hint search work so these physical/state journeys can
// finish without advancing their clock behind the fixture's back. All moves,
// legal rules, candidate publication and CPU searches remain production code.
// These tests do not establish the 90-second duration or board responsiveness;
// separate deadline/async tests own that evidence.
class BoundedHintEngine final : public modules::IEngine {
public:
    modules::LitchMaxEngine::PieceSide GetPieceSideAtSquare(uint8_t square) const {
        return m_engine.GetPieceSideAtSquare(square);
    }
    void Reset() override { m_engine.Reset(); }
    bool SetFen(const char* fen) override { return m_engine.SetFen(fen); }
    bool GetFen(char* fen, size_t size) const override { return m_engine.GetFen(fen, size); }
    bool ApplyUciMove(const char* move) override { return m_engine.ApplyUciMove(move); }
    modules::SideToMove GetSideToMove() const override { return m_engine.GetSideToMove(); }
    bool IsInCheck(modules::SideToMove side) const override { return m_engine.IsInCheck(side); }
    bool IsInCheckmate(modules::SideToMove side) const override { return m_engine.IsInCheckmate(side); }
    bool IsInStalemate(modules::SideToMove side) const override { return m_engine.IsInStalemate(side); }
    bool GetLegalDestinationsMask(uint8_t from, uint64_t& mask) override {
        return m_engine.GetLegalDestinationsMask(from, mask);
    }
    bool StartThink(const modules::ThinkConfig& config) override {
        auto bounded = Bounded(config, 1800000U);
        // Real Classic journeys need the first completed five-ply result;
        // their fixed clock does not measure physical calculation time.
        if (bounded.continuousAnalysis && (bounded.maxDepth == 0U || bounded.maxDepth > 5U)) {
            bounded.maxDepth = 5U;
        }
        return m_engine.StartThink(bounded);
    }
    bool StartDuckThink(const char* fen, uint8_t duck, const modules::ThinkConfig& config) override {
        return m_engine.StartDuckThink(fen, duck, Bounded(config, 4096U));
    }
    void CancelThink() override { m_engine.CancelThink(); }
    modules::ThinkState PollThinkProgress(uint8_t& progress, modules::UciMove& move) override {
        return m_engine.PollThinkProgress(progress, move);
    }
    bool GetLastThinkStats(modules::ThinkStats& stats) const override { return m_engine.GetLastThinkStats(stats); }
    bool GetLastClassicThinkResult(modules::ClassicThinkResult& result) const override {
        return m_engine.GetLastClassicThinkResult(result);
    }
    bool GetLastDuckThinkResult(modules::DuckThinkResult& result) const override {
        return m_engine.GetLastDuckThinkResult(result);
    }
private:
    static modules::ThinkConfig Bounded(modules::ThinkConfig config, const uint32_t nodeLimit) {
        // A frozen journey clock cannot establish the real calculation minimum.
        // These fixtures cover physical/state behavior after a scored result.
        config.minimumCalculationMs = 0U;
        if (config.continuousAnalysis &&
            (config.maxNodes == 0U || config.maxNodes > nodeLimit)) {
            config.maxNodes = nodeLimit;
        }
        return config;
    }
    modules::LitchMaxEngine m_engine;
};

// Own the injected engine longer than the production session without wrapping
// or duplicating the session API. Fixtures use the public session reference.
struct BoundedHintSession {
    explicit BoundedHintSession(const virtual_litch_config& config, const char* path = nullptr)
        : session(config, engine, path) {}
    BoundedHintEngine engine;
    runtime::VirtualLitchSession session;
};

} // namespace test_support
