#include "modules/litch_max_engine.hpp"

namespace modules {
bool LitchMaxEngine::ApplyUciMove(const char* uciMove) {
    if (m_thinking || !m_game.ApplyUciMove(uciMove)) return false;
    m_lastClassicThinkResult = ClassicThinkResult{};
    m_lastClassicThinkResultValid = false;
    ClearContinuousAnalysis();
    return true;
}
} // namespace modules
