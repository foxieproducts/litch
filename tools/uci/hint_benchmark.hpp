#pragma once
#include <cstdint>
#include <vector>
#include "modules/litch_max_engine.hpp"
// Host-only adapter: observes the production hint mailbox without stopping search.
void RunHintBenchmark(modules::LitchMaxEngine& engine,
                      std::vector<uint32_t> waitsMs, uint32_t seed);
void RunHintNodeBenchmark(modules::LitchMaxEngine& engine,
                          std::vector<uint32_t> nodeBudgets, uint32_t seed);
// Stop after the first qualifying Full root, preserving production Hint policy.
void RunHintFirstBenchmark(modules::LitchMaxEngine& engine,
                           uint32_t maxNodes, uint32_t seed);
