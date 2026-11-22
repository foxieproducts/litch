#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/chess_strings.hpp"
#include "modules/game_persistence_coordinator_update_internal.hpp"

namespace modules {

void GamePersistenceCoordinator::LogMoveSummary(
    const char* actor,
    const char* fenBefore,
    const uint8_t fromSquare,
    const uint8_t toSquare) const {
    char fromText[3] = {0};
    char toText[3] = {0};
    FormatSquareOrUnknown(fromSquare, fromText);
    FormatSquareOrUnknown(toSquare, toText);

    char movedPiece = '\0';
    (void)game_persistence_update_internal::TryGetFenPieceAtSquare(
        fenBefore, fromSquare, movedPiece);
    uint8_t captureSquare = toSquare;
    (void)game_persistence_update_internal::TryComputeCpuCaptureSquare(
        fenBefore, fromSquare, toSquare, captureSquare);
    char capturedPiece = '\0';
    (void)game_persistence_update_internal::TryGetFenPieceAtSquare(
        fenBefore, captureSquare, capturedPiece);

    char captureText[3] = {0};
    FormatSquareOrUnknown(captureSquare, captureText);
    logger::Trace("move: %s %s%s %s->%s cap=%s%s@%s",
                 actor,
                 DescribeFenPieceSide(movedPiece),
                 DescribeFenPieceType(movedPiece),
                 fromText,
                 toText,
                 DescribeFenPieceSide(capturedPiece),
                 DescribeFenPieceType(capturedPiece),
                 captureText);
}

} // namespace modules
