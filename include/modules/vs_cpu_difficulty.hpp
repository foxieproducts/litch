#pragma once

#include <algorithm>
#include <cstdint>

#include "modules/engine.hpp"

namespace modules {

enum class CpuSearchVariant : uint8_t { Classic, Duck };
inline constexpr const char* CLASSIC_CPU_POLICY_ID = "classic-fast-deadlines-v3";

inline ThinkConfig MakeThinkConfigForVsCpuLevel(const uint32_t vsCpuLevel,
                                                const uint32_t rngSeed,
                                                const CpuSearchVariant variant = CpuSearchVariant::Classic) {
    const uint8_t lvl = static_cast<uint8_t>(std::clamp<uint32_t>(vsCpuLevel, 0u, 11u));

    // Novice Classic levels complete one/two static plies. Higher levels use
    // increasing Full search budgets. No deliberate move replacement or
    // measured Elo rating is implied.
    // Retain node/depth profiles. Optimized Classic levels 3-10 use 70% of the
    // former deadline; work remains position-dependent instead of using
    // one conservative calibration-panel node minimum as a product ceiling.
    // Actual latency and playing strength still need device calibration.
    static constexpr uint32_t NODE_MAX_BY_LEVEL[12] = {
        0U, 256U, 4096U, 180000U, 210000U, 225000U,
        300000U, 360000U, 540000U, 810000U, 1200000U, 1800000U,
    };

    static constexpr uint32_t DEPTH_MAX_BY_LEVEL[12] = {
        0U,
        1U,
        2U,
        4U,
        6U,
        7U,
        8U,
        9U,
        11U,
        12U,
        14U,
        16U,
    };

    static constexpr uint32_t THINK_MS_MAX_BY_LEVEL[12] = {
        0U, 900U, 900U, 900U, 2000U, 4000U,
        7000U, 10000U, 15000U, 25000U, 40000U, 60000U,
    };

    ThinkConfig cfg;
    cfg.difficulty = lvl;
    cfg.maxThinkMs = THINK_MS_MAX_BY_LEVEL[lvl];
    if (variant == CpuSearchVariant::Classic && lvl >= 3U && lvl <= 10U)
        cfg.maxThinkMs = cfg.maxThinkMs * 7U / 10U;
    cfg.firstRootNodeReserve = static_cast<uint16_t>(std::min<uint32_t>(16384U, NODE_MAX_BY_LEVEL[lvl]));
    cfg.rngSeed = rngSeed;

    cfg.maxNodes = NODE_MAX_BY_LEVEL[lvl];
    cfg.maxDepth = DEPTH_MAX_BY_LEVEL[lvl];
    if (variant == CpuSearchVariant::Duck && lvl >= 1U && lvl <= 2U) {
        cfg.maxNodes = 180000U;
        cfg.maxDepth = 3U;
        cfg.firstRootNodeReserve = 16384U;
    }

    return cfg;
}

inline ThinkConfig MakeThinkConfigForVsCpuTurn(const uint32_t vsCpuLevel,
                                               const uint32_t completedTurns,
                                               const uint32_t rngSeed,
                                               const CpuSearchVariant variant = CpuSearchVariant::Classic) {
    const uint32_t level = std::min<uint32_t>(vsCpuLevel, 11U);
    // Three CPU turns: start with competent analyzed work and reach the selected
    // profile on turn three. Every level keeps its best completed decision.
    const uint32_t openingLevel = level <= 3U ? level
        : 3U + ((level - 3U) * std::min<uint32_t>(completedTurns, 2U)) / 2U;
    ThinkConfig config = MakeThinkConfigForVsCpuLevel(openingLevel, rngSeed, variant);
    // The ramp reduces search work, while the selected level owns the time bound.
    config.maxThinkMs = MakeThinkConfigForVsCpuLevel(level, rngSeed, variant).maxThinkMs;
    return config;
}

} // namespace modules
