#pragma once
#include <algorithm>
#include <limits>
#include <litch_max/classic_chess_search.hpp>
#include "modules/vs_cpu_difficulty.hpp"
namespace modules {
inline constexpr uint8_t CLASSIC_MIN_HINT_PLIES = 1U;
inline litch_max::classic_chess::SearchConfig ResolveClassicSearchConfig(const ThinkConfig& config) {
    auto profile = MakeThinkConfigForVsCpuLevel(
        config.difficulty == 0U ? 1U : config.difficulty, config.rngSeed);
    // Unspecified engine requests retain the legacy general-purpose defaults.
    if (config.difficulty == 0U) { profile.maxNodes = 180000U; profile.maxDepth = 3U; }
    litch_max::classic_chess::SearchConfig search;
    search.maxNodes = config.maxNodes != 0U ? config.maxNodes :
        (config.continuousAnalysis ? std::numeric_limits<uint32_t>::max() : profile.maxNodes);
    const uint32_t depth = config.maxDepth != 0U ? config.maxDepth :
        (config.continuousAnalysis ? 31U : profile.maxDepth);
    search.maxDepth = static_cast<uint8_t>(std::min<uint32_t>(depth, 31U));
    search.minimumDepth = config.continuousAnalysis ? CLASSIC_MIN_HINT_PLIES : 1U;
    search.firstRootNodeReserve = config.continuousAnalysis ? 0U : config.firstRootNodeReserve;
    search.reversibleHalfmoves = config.reversibleHalfmoves;
    search.recaptureOrdering = config.continuousAnalysis;
    search.preliminaryRoot = config.continuousAnalysis && config.minimumCalculationMs != 0U;
    search.staticLeafOnly = !config.continuousAnalysis &&
        config.difficulty >= 1U && config.difficulty <= 2U;
    return search;
}
} // namespace modules
