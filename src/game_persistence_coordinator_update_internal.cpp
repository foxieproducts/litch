#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "modules/chess_strings.hpp"
#include "modules/game_persistence_coordinator_update_internal.hpp"

#include "modules/promotion_helpers.hpp"
#include "modules/persisted_side.hpp"

namespace modules::game_persistence_update_internal {

uint8_t FileFromSquare(const uint8_t sq) {
    return static_cast<uint8_t>(sq % 8U);
}

uint8_t RankFromTopFromSquare(const uint8_t sq) {
    return static_cast<uint8_t>(sq / 8U);
}

bool TryGetFenPieceAtSquare(const char* fen, const uint8_t square, char& outPieceChar) {
    return modules::TryGetFenPieceAtSquare(fen, square, outPieceChar);
}

bool TryComputeCpuCaptureSquare(const char* currentFen,
                                const uint8_t fromSquare,
                                const uint8_t toSquare,
                                uint8_t& outCaptureSquare) {
    if (fromSquare >= 64 || toSquare >= 64) {
        return false;
    }

    // Default: normal capture (or no-capture) uses the destination square.
    outCaptureSquare = toSquare;

    char fromPiece = '\0';
    char toPiece = '\0';
    if (!TryGetFenPieceAtSquare(currentFen, fromSquare, fromPiece)) {
        return false;
    }

    const bool toOccupied = TryGetFenPieceAtSquare(currentFen, toSquare, toPiece);
    if (toOccupied) {
        // Normal capture or non-capture into occupied square is handled by toSquare.
        return true;
    }

    const bool isWhitePawn = (fromPiece == 'P');
    const bool isBlackPawn = (fromPiece == 'p');
    if (!isWhitePawn && !isBlackPawn) {
        return true;
    }

    const int fromFile = static_cast<int>(FileFromSquare(fromSquare));
    const int fromRank = static_cast<int>(RankFromTopFromSquare(fromSquare));
    const int toFile = static_cast<int>(FileFromSquare(toSquare));
    const int toRank = static_cast<int>(RankFromTopFromSquare(toSquare));

    const int df = toFile - fromFile;
    const int dr = toRank - fromRank;

    // En-passant: pawn moves diagonally onto an empty square; captured pawn is
    // on the origin rank and destination file.
    const bool isDiagonal = (df == 1) || (df == -1);
    if (!isDiagonal) {
        return true;
    }

    const bool isWhiteEp = isWhitePawn && (dr == -1);
    const bool isBlackEp = isBlackPawn && (dr == 1);
    if (!isWhiteEp && !isBlackEp) {
        return true;
    }

    const uint8_t capSq = static_cast<uint8_t>(fromRank * 8 + toFile);
    if (capSq >= 64) {
        return false;
    }

    outCaptureSquare = capSq;
    return true;
}

bool TryComputeCpuCaptureSquareAndIsCapture(const char* currentFen,
                                            const uint8_t fromSquare,
                                            const uint8_t toSquare,
                                            uint8_t& outCaptureSquare,
                                            bool& outIsCapture) {
    outCaptureSquare = toSquare;
    outIsCapture = false;

    if (fromSquare >= 64 || toSquare >= 64) {
        return false;
    }

    char fromPiece = '\0';
    if (!TryGetFenPieceAtSquare(currentFen, fromSquare, fromPiece)) {
        return false;
    }

    char toPiece = '\0';
    const bool toOccupied = TryGetFenPieceAtSquare(currentFen, toSquare, toPiece);
    if (toOccupied) {
        outCaptureSquare = toSquare;
        outIsCapture = true;
        return true;
    }

    const bool isWhitePawn = (fromPiece == 'P');
    const bool isBlackPawn = (fromPiece == 'p');
    if (!isWhitePawn && !isBlackPawn) {
        return true;
    }

    const int fromFile = static_cast<int>(FileFromSquare(fromSquare));
    const int fromRank = static_cast<int>(RankFromTopFromSquare(fromSquare));
    const int toFile = static_cast<int>(FileFromSquare(toSquare));
    const int toRank = static_cast<int>(RankFromTopFromSquare(toSquare));

    const int df = toFile - fromFile;
    const int dr = toRank - fromRank;

    // En-passant: pawn moves diagonally onto an empty square; captured pawn is
    // on the origin rank and destination file.
    const bool isDiagonal = (df == 1) || (df == -1);
    if (!isDiagonal) {
        return true;
    }

    const bool isWhiteEp = isWhitePawn && (dr == -1);
    const bool isBlackEp = isBlackPawn && (dr == 1);
    if (!isWhiteEp && !isBlackEp) {
        return true;
    }

    const uint8_t capSq = static_cast<uint8_t>(fromRank * 8 + toFile);
    if (capSq >= 64) {
        return false;
    }

    outCaptureSquare = capSq;
    outIsCapture = true;
    return true;
}

uint8_t TryGetPromotionSuffixFromUci(const char* uci) {
    if (uci == nullptr) {
        return 0;
    }
    // UCI promotion: e7e8q (len=5)
    const size_t n = std::strlen(uci);
    if (n < 5) {
        return 0;
    }
    const char c = NormalizePromotionSuffixLower(uci[4]);
    return IsPromotionSuffixChar(c) ? static_cast<uint8_t>(c) : 0U;
}

void BuildUndoPersistFieldsFromFen(const char* fenBefore,
                                   const SideToMove sideToMove,
                                   const uint8_t fromSquare,
                                   const uint8_t toSquare,
                                   const bool captureSquareValid,
                                   const uint8_t captureSquare,
                                   const uint8_t promotedPiece,
                                   uint8_t* const outFields) {
    if (outFields == nullptr) {
        return;
    }
    outFields[0] = PersistedSideToU8(
        PersistedSideFromSideToMove(sideToMove));
    char movedPiece = '\0';
    (void)TryGetFenPieceAtSquare(fenBefore, fromSquare, movedPiece);
    outFields[1] = static_cast<uint8_t>(movedPiece);

    uint8_t capturedSquare = toSquare;
    bool isCapture = false;
    if (captureSquareValid && captureSquare < 64U) {
        capturedSquare = captureSquare;
        isCapture = true;
    } else {
        (void)TryComputeCpuCaptureSquareAndIsCapture(
            fenBefore,
            fromSquare,
            toSquare,
            capturedSquare,
            isCapture);
    }
    char capturedPiece = '\0';
    if (isCapture) {
        (void)TryGetFenPieceAtSquare(
            fenBefore, capturedSquare, capturedPiece);
    }
    outFields[2] = static_cast<uint8_t>(capturedPiece);
    outFields[3] = capturedPiece == '\0' ? 255U : capturedSquare;
    outFields[4] = promotedPiece;
    outFields[5] = fromSquare;
    outFields[6] = toSquare;
}

} // namespace modules::game_persistence_update_internal
