#pragma once

#include <cstdint>

#include <litch_max/chess_castling_patterns.hpp>

namespace runtime {

bool TryMatchCastlingPattern(uint8_t from, uint8_t to, litch_max::CastlingPattern& out, bool& outIsKingMove);
bool TryGetCastlingRookMoveForKingMove(uint8_t from, uint8_t to, uint8_t& outRookFrom, uint8_t& outRookTo);

} // namespace runtime
