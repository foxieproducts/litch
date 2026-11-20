#pragma once

#include <cstdint>

#include <litch_max/chess_notation.hpp>
#include "modules/engine.hpp"

namespace modules::game_persistence_vs_cpu_internal {

bool IsParsedMoveLegalInCurrentPosition(IEngine& engine,
                                        uint8_t fromSquare,
                                        uint8_t toSquare);

// Retain the best completed legal result, or the ranked legal Seed if no
// completed analysis is available. No level deliberately substitutes a move.
bool TryChooseClassicCpuMove(IEngine& engine, const UciMove& searched,
                             litch_max::ParsedUciMove& outParsed, UciMove& outUci);

} // namespace modules::game_persistence_vs_cpu_internal
