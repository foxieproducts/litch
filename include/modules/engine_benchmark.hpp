#pragma once
#include "modules/engine.hpp"
#include "mcu-max-position.h"
namespace modules {
inline constexpr uint8_t ENGINE_BENCHMARK_CASE_COUNT = 6U;
struct EngineBenchmarkCase {
    const char* name;
    const char* fen; // Host import provenance; never parsed on target.
    uint16_t reversibleHalfmoves;
    mcumax_position root;
};
const EngineBenchmarkCase* GetEngineBenchmarkCase(uint8_t caseId);
struct EngineBenchmarkStatus {
    ThinkDiagnostics diagnostics{};
    UciMove finalMove{};
    bool finalMoveValid = false;
    uint8_t caseId = 0U;
    uint8_t level = 0U;
    bool cancelRequested = false;
};
} // namespace modules
