#include "modules/chess_strings.hpp"

#include <litch_max/chess_notation.hpp>

namespace modules {

const char* DescribeFenPieceType(const char pieceChar) {
    switch (pieceChar) {
        case 'P':
        case 'p':
            return "Pawn";
        case 'N':
        case 'n':
            return "Knight";
        case 'B':
        case 'b':
            return "Bishop";
        case 'R':
        case 'r':
            return "Rook";
        case 'Q':
        case 'q':
            return "Queen";
        case 'K':
        case 'k':
            return "King";
        default:
            return "-";
    }
}

const char* DescribeFenPieceSide(const char pieceChar) {
    if (pieceChar >= 'A' && pieceChar <= 'Z') {
        return "W";
    }
    if (pieceChar >= 'a' && pieceChar <= 'z') {
        return "B";
    }
    return "-";
}

void FormatSquareOrUnknown(const uint8_t square, char out[3]) {
    out[0] = '?';
    out[1] = '?';
    out[2] = '\0';
    (void)litch_max::TryFormatUciSquare(square, out, 3);
}

bool SafeGetFenToBuffer(IEngine& engine, char* outFen, const size_t outSize) {
    if (outFen == nullptr || outSize == 0U) {
        return false;
    }

    outFen[0] = '\0';
    return engine.GetFen(outFen, outSize);
}

} // namespace modules