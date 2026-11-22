#include "runtime/game_state_machine.hpp"

#include "runtime/game_state_machine_castling.hpp"

namespace runtime {

bool TryMatchCastlingPattern(const uint8_t from,
                             const uint8_t to,
                             litch_max::CastlingPattern& out,
                             bool& outIsKingMove) {
    for (const auto& pattern : litch_max::CHESS_CASTLING_PATTERNS) {
        if (from == pattern.kingFrom && to == pattern.kingTo) {
            out = pattern;
            outIsKingMove = true;
            return true;
        }
        if (from == pattern.rookFrom && to == pattern.rookTo) {
            out = pattern;
            outIsKingMove = false;
            return true;
        }
    }

    return false;
}

bool TryGetCastlingRookMoveForKingMove(const uint8_t from,
                                       const uint8_t to,
                                       uint8_t& outRookFrom,
                                       uint8_t& outRookTo) {
    litch_max::CastlingPattern pattern;
    bool isKingMove = false;
    if (!TryMatchCastlingPattern(from, to, pattern, isKingMove) || !isKingMove) {
        return false;
    }

    outRookFrom = pattern.rookFrom;
    outRookTo = pattern.rookTo;
    return true;
}

} // namespace runtime
