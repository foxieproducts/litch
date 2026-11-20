#pragma once

#include <cstdint>

#include "modules/engine.hpp"
#include "modules/promotion_ux.hpp"

namespace modules {

inline char PromotionPieceToUciChar(const PromotionPiece piece) {
    // UCI promotion suffix: q,r,b,n
    switch (piece) {
        case PromotionPiece::Queen:
            return 'q';
        case PromotionPiece::Rook:
            return 'r';
        case PromotionPiece::Bishop:
            return 'b';
        case PromotionPiece::Knight:
            return 'n';
    }

    return 'q';
}

inline bool IsPromotionSuffixChar(const char c) {
    return c == 'q' || c == 'r' || c == 'b' || c == 'n' || c == 'Q' || c == 'R' || c == 'B' || c == 'N';
}

inline char NormalizePromotionSuffixLower(const char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

inline char PromotionFenPieceChar(const SideToMove sideToMove, const char promotionSuffixLower) {
    switch (promotionSuffixLower) {
        case 'q':
            return (sideToMove == SideToMove::White) ? 'Q' : 'q';
        case 'r':
            return (sideToMove == SideToMove::White) ? 'R' : 'r';
        case 'b':
            return (sideToMove == SideToMove::White) ? 'B' : 'b';
        case 'n':
            return (sideToMove == SideToMove::White) ? 'N' : 'n';
        default:
            return (sideToMove == SideToMove::White) ? 'Q' : 'q';
    }
}

} // namespace modules
