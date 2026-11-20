#pragma once

#include <cstdint>

#include "modules/engine.hpp"

namespace modules {

enum class HintMoveSelectionSource : uint8_t {
    EngineBest,
    LegalFallback,
};

struct HintMoveSelection {
    uint8_t fromSquare = 0U;
    uint8_t toSquare = 0U;
    HintMoveSelectionSource source = HintMoveSelectionSource::EngineBest;
};

// Selects a legal hint from the legality snapshot captured before search.
// A valid engine result is authoritative. When it is unusable, the first
// deterministic legal move in that snapshot is used.
bool TrySelectHintMove(const UciMove* engineBest,
                       const uint64_t* legalDestByFromSquare,
                       uint64_t legalComputedFromSquaresMask64,
                       HintMoveSelection& outSelection);

} // namespace modules
