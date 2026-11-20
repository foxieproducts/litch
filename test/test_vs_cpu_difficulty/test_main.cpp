#include <cstdio>

#include "modules/vs_cpu_difficulty.hpp"
#include "modules/classic_search_config.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[VS_CPU_DIFFICULTY] start");

    const modules::ThinkConfig c0 = modules::MakeThinkConfigForVsCpuLevel(0, 123);
    Check(c0.difficulty == 0, "level0 difficulty=0");
    Check(c0.maxNodes == 0, "level0 disables node budget");
    Check(c0.maxDepth == 0, "level0 disables depth budget");
    Check(c0.maxThinkMs == 0U, "level0 disables the think deadline");
    Check(c0.rngSeed == 123, "rngSeed preserved");

    const modules::ThinkConfig c11 = modules::MakeThinkConfigForVsCpuLevel(11, 9);
    Check(c11.difficulty == 11, "level11 difficulty=11");
    Check(c11.maxNodes == 1800000U, "level11 maxNodes");
    Check(c11.maxThinkMs == 60000U, "level11 uses the measured one-minute deadline");
    Check(c11.firstRootNodeReserve == 16384U && c0.firstRootNodeReserve == 0U,
          "CPU first-root allowance is bounded and disabled at level0");
    Check(c11.maxDepth == 16U, "level11 maxDepth");

    const modules::ThinkConfig c3 = modules::MakeThinkConfigForVsCpuLevel(3, 1);
    Check(c3.maxNodes == 180000U, "level3 maxNodes");
    Check(c3.maxDepth == 4U, "level3 maxDepth");

    const auto c1 = modules::MakeThinkConfigForVsCpuLevel(1U, 1U);
    const auto c2 = modules::MakeThinkConfigForVsCpuLevel(2U, 1U);
    Check(c1.minimumCalculationMs == 0U && c2.minimumCalculationMs == 0U &&
              !modules::ResolveClassicSearchConfig(c1).preliminaryRoot &&
              !modules::ResolveClassicSearchConfig(c2).preliminaryRoot,
          "human Hint minimum does not delay or change novice CPU calculation");
    Check(c1.maxNodes == 256U && c2.maxNodes == 4096U &&
              c1.maxDepth == 1U && c2.maxDepth == 2U &&
              c1.firstRootNodeReserve == 256U && c2.firstRootNodeReserve == 4096U,
          "novice Classic levels use strict one/two-ply work budgets");
    for (uint32_t level = 1U; level <= 2U; ++level) {
        const auto duck = modules::MakeThinkConfigForVsCpuTurn(level, 0U, 1U, modules::CpuSearchVariant::Duck);
        Check(duck.maxNodes == 180000U && duck.maxDepth == 3U && duck.firstRootNodeReserve == 16384U,
              "Classic novice changes preserve existing Duck search profiles");
    }

    auto hint = c1;
    hint.continuousAnalysis = true;
    Check(modules::ResolveClassicSearchConfig(c1).staticLeafOnly &&
              modules::ResolveClassicSearchConfig(c2).staticLeafOnly &&
              !modules::ResolveClassicSearchConfig(c3).staticLeafOnly &&
              !modules::ResolveClassicSearchConfig(hint).staticLeafOnly,
          "static novice horizons never restrict stronger CPU or continuous Hint");

    const modules::ThinkConfig c99 = modules::MakeThinkConfigForVsCpuLevel(99, 1);
    Check(c99.difficulty == 11, "level clamps to 11");
    Check(c99.maxNodes == c11.maxNodes, "clamped nodes match level11");
    Check(c99.maxDepth == c11.maxDepth, "clamped depth matches level11");
    Check(c99.maxThinkMs == c11.maxThinkMs, "clamped deadline matches level11");

    constexpr uint32_t DEADLINE_MS_BY_LEVEL[] = {
        0U, 900U, 900U, 630U, 1400U, 2800U,
        4900U, 7000U, 10500U, 17500U, 28000U, 60000U,
    };
    constexpr uint8_t OPENING_LEVELS[12][3] = {
        {0U, 0U, 0U}, {1U, 1U, 1U}, {2U, 2U, 2U}, {3U, 3U, 3U},
        {3U, 3U, 4U}, {3U, 4U, 5U}, {3U, 4U, 6U}, {3U, 5U, 7U},
        {3U, 5U, 8U}, {3U, 6U, 9U}, {3U, 6U, 10U}, {3U, 7U, 11U},
    };
    for (uint32_t level = 0U; level <= 11U; ++level) {
        const auto baseline = modules::MakeThinkConfigForVsCpuLevel(level, 19U);
        Check(baseline.maxThinkMs == DEADLINE_MS_BY_LEVEL[level],
              "every selected CPU level has its approved wall-time ceiling");
        for (uint32_t turn = 0U; turn < 5U; ++turn) {
            const uint8_t expectedLevel = OPENING_LEVELS[level][std::min(turn, 2U)];
            const auto expected = modules::MakeThinkConfigForVsCpuLevel(expectedLevel, 19U);
            const auto opening = modules::MakeThinkConfigForVsCpuTurn(level, turn, 19U);
            Check(opening.difficulty == expectedLevel &&
                      opening.maxNodes == expected.maxNodes &&
                      opening.maxDepth == expected.maxDepth && opening.rngSeed == 19U &&
                      opening.firstRootNodeReserve == expected.firstRootNodeReserve,
                  "all opening phases preserve work budgets, seed and first-root allowance");
            Check(opening.maxThinkMs == DEADLINE_MS_BY_LEVEL[level],
                  "the selected-level deadline stays fixed through every opening phase");
        }
        std::printf("[CPU_PROFILE_CEILING] level=%u max_think_ms=%u ramp=%u/%u/%u\n",
                    level, baseline.maxThinkMs, OPENING_LEVELS[level][0],
                    OPENING_LEVELS[level][1], OPENING_LEVELS[level][2]);
    }

    if (g_failures == 0) {
        std::puts("[VS_CPU_DIFFICULTY] ok");
        return 0;
    }

    std::printf("[VS_CPU_DIFFICULTY] failures=%d\n", g_failures);
    return 1;
}
