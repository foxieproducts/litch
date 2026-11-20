#include "modules/draw_rules.hpp"

#include <cctype>
#include <cstddef>
#include <cstring>

#include "modules/chess_strings.hpp"
#include <litch_max/classic_chess_search.hpp>

namespace modules {

namespace {

enum class SquareColor : uint8_t {
    Light = 0,
    Dark = 1,
};

struct MaterialSummary {
    uint8_t whiteKnights = 0;
    uint8_t blackKnights = 0;
    uint8_t bishops = 0;
    uint8_t bishopLightSquares = 0;
    uint8_t bishopDarkSquares = 0;
    bool hasPawn = false;
    bool hasRook = false;
    bool hasQueen = false;
    bool valid = false;
};

SquareColor GetSquareColor(const uint8_t rank, const uint8_t file) {
    return (((rank + file) & 1U) == 0U) ? SquareColor::Light : SquareColor::Dark;
}

bool TrySummarizeMaterial(const char* fen, MaterialSummary& outSummary) {
    outSummary = MaterialSummary{};
    if (fen == nullptr || fen[0] == '\0') {
        return false;
    }

    uint8_t rank = 0;
    uint8_t file = 0;
    for (const char* p = fen; *p != '\0' && *p != ' '; ++p) {
        const char c = *p;
        if (c == '/') {
            if (file != 8U || rank >= 7U) {
                return false;
            }
            ++rank;
            file = 0;
            continue;
        }

        if (c >= '1' && c <= '8') {
            file = static_cast<uint8_t>(file + static_cast<uint8_t>(c - '0'));
            if (file > 8U) {
                return false;
            }
            continue;
        }

        const bool isPiece = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
        if (!isPiece || rank >= 8U || file >= 8U) {
            return false;
        }

        switch (c) {
            case 'P':
            case 'p':
                outSummary.hasPawn = true;
                break;
            case 'R':
            case 'r':
                outSummary.hasRook = true;
                break;
            case 'Q':
            case 'q':
                outSummary.hasQueen = true;
                break;
            case 'N':
                ++outSummary.whiteKnights;
                break;
            case 'n':
                ++outSummary.blackKnights;
                break;
            case 'B':
            case 'b':
                ++outSummary.bishops;
                if (GetSquareColor(rank, file) == SquareColor::Light) {
                    ++outSummary.bishopLightSquares;
                } else {
                    ++outSummary.bishopDarkSquares;
                }
                break;
            case 'K':
            case 'k':
                break;
            default:
                return false;
        }

        ++file;
        if (file > 8U) {
            return false;
        }
    }

    outSummary.valid = true;
    return true;
}

} // namespace

bool TryBuildFenPositionKey4(const char* fen, fxe::FixedString<96>& outKey) {
    outKey.Clear();

    if (fen == nullptr || fen[0] == '\0') {
        return false;
    }

    char buf[96] = {};
    size_t w = 0;

    // Copy <placement> <side> <castling> <ep>
    // i.e. stop before the 4th space character.
    uint8_t spaces = 0;
    for (const char* p = fen; *p != '\0'; ++p) {
        const char c = *p;
        if (c == ' ') {
            if (spaces == 3U) {
                break;
            }
            ++spaces;
        }

        if (w + 1U >= sizeof(buf)) {
            return false;
        }
        buf[w] = c;
        ++w;
    }

    if (spaces < 3U) {
        return false;
    }

    buf[w] = '\0';
    return outKey.Assign(buf) && !outKey.Empty();
}

uint8_t CountThreefoldRepetition(const char* currentFen,
                                 const char* const* historyFens,
                                 const uint8_t historyCount) {
    if (currentFen == nullptr || currentFen[0] == '\0') {
        return 0;
    }
    if (historyFens == nullptr || historyCount == 0U) {
        return 0;
    }

    uint8_t count = 0;
    for (uint8_t i = 0; i < historyCount; ++i) {
        const char* fen = historyFens[i];
        if (fen == nullptr || fen[0] == '\0') {
            continue;
        }

        if (SameClassicRepetitionPosition(currentFen, fen)) {
            ++count;
        }
    }

    return count;
}

bool SameClassicRepetitionPosition(const char* leftFen, const char* rightFen) {
    fxe::FixedString<96> left, right;
    if (!TryBuildFenPositionKey4(leftFen, left) ||
        !TryBuildFenPositionKey4(rightFen, right)) return false;
    if (left.Equals(right.View())) return true;
    const char* leftEp = std::strrchr(left.CStr(), ' ');
    const char* rightEp = std::strrchr(right.CStr(), ' ');
    if (leftEp == nullptr || rightEp == nullptr) return false;
    const size_t prefix = static_cast<size_t>(leftEp - left.CStr());
    if (prefix != static_cast<size_t>(rightEp - right.CStr()) ||
        std::memcmp(left.CStr(), right.CStr(), prefix) != 0) return false;
    // Board, side and castling are already exact. PositionKey differs only in
    // its final effective-EP byte here; this is not general hash adjudication.
    uint64_t leftKey = 0U, rightKey = 0U;
    return litch_max::classic_chess::TryPositionKey(leftFen, leftKey) &&
           litch_max::classic_chess::TryPositionKey(rightFen, rightKey) && leftKey == rightKey;
}

uint16_t NextHalfmoveClockPlies(const char* preMoveFen,
                                const uint16_t priorHalfmoveClockPlies,
                                const uint8_t fromSquare,
                                const uint8_t toSquare,
                                const bool captureSquareValid,
                                const uint8_t captureSquare) {
    char movedPiece = '\0';
    if (!TryGetFenPieceAtSquare(preMoveFen, fromSquare, movedPiece)) {
        // Defensive: if we can't determine the moved piece, avoid falsely
        // advancing toward a 50-move draw.
        return 0;
    }

    const char movedLower = static_cast<char>(std::tolower(static_cast<unsigned char>(movedPiece)));
    const bool isPawnMove = (movedLower == 'p');
    if (isPawnMove) {
        return 0;
    }

    if (captureSquareValid) {
        return 0;
    }

    char capturedPiece = '\0';
    const bool isCapture = TryGetFenPieceAtSquare(preMoveFen, toSquare, capturedPiece);
    if (isCapture) {
        return 0;
    }

    (void)captureSquare;

    const uint32_t next = static_cast<uint32_t>(priorHalfmoveClockPlies) + 1U;
    return (next > 0xFFFFU) ? 0xFFFFU : static_cast<uint16_t>(next);
}

bool IsInsufficientMaterialDraw(const char* fen) {
    MaterialSummary summary;
    if (!TrySummarizeMaterial(fen, summary) || !summary.valid) {
        return false;
    }

    if (summary.hasPawn || summary.hasRook || summary.hasQueen) {
        return false;
    }

    const uint8_t totalKnights = static_cast<uint8_t>(summary.whiteKnights + summary.blackKnights);
    const uint8_t totalMinors = static_cast<uint8_t>(summary.bishops + totalKnights);

    if (totalMinors == 0U) {
        return true;
    }

    if (totalMinors == 1U) {
        return true;
    }

    if (totalKnights == 0U && summary.bishops != 0U) {
        const bool bishopsAllLight = (summary.bishopDarkSquares == 0U);
        const bool bishopsAllDark = (summary.bishopLightSquares == 0U);
        return bishopsAllLight || bishopsAllDark;
    }

    return false;
}

} // namespace modules
