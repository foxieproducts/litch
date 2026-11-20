#pragma once

#include <cstdint>

#include "modules/engine.hpp"

namespace modules::king_safety {

// Immutable position evidence; these queries never enter the search engine.
struct Lift {
    const char* fen;
    uint8_t fromSquare;
    uint8_t kingSquare;
    SideToMove moverSide;
};

bool TryFindSelfCheckAttacker(const Lift& lift, uint8_t& outAttackerSquare);
// Exact attackers of the current king, with all ray blockers retained.
uint64_t CheckingPiecesMask(const char* fen, uint8_t kingSquare, SideToMove side);
bool TryFindPinningPiece(const Lift& lift, uint8_t& outAttackerSquare);

} // namespace modules::king_safety
