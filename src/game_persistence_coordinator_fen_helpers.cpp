#include "modules/game_persistence_coordinator.hpp"

#include <litch_max/chess_notation.hpp>
#include "modules/draw_rules.hpp"
#include <litch_max/duck_chess_rules.hpp>
#include <litch_max/chess_castling_patterns.hpp>

namespace modules {
namespace {

SideToMove SideToMoveFromFen(const char* fen,
                             const SideToMove fallback) {
    if (fen == nullptr) {
        return fallback;
    }
    const char* cursor = fen;
    while (*cursor != '\0' && *cursor != ' ') {
        ++cursor;
    }
    while (*cursor == ' ') {
        ++cursor;
    }
    if (*cursor == 'w') {
        return SideToMove::White;
    }
    if (*cursor == 'b') {
        return SideToMove::Black;
    }
    return fallback;
}

} // namespace

bool GamePersistenceCoordinator::IsCurrentEngineFenEquivalent(
    const FenString& nowFen) const {
    if (m_cachedFen.Equals(nowFen.View())) {
        return true;
    }
    if (!IsDuckGame()) {
        return false;
    }

    fxe::FixedString<96> cachedPosition;
    fxe::FixedString<96> enginePosition;
    return TryBuildFenPositionKey4(
               m_cachedFen.CStr(), cachedPosition) &&
           TryBuildFenPositionKey4(nowFen.CStr(), enginePosition) &&
           cachedPosition.Equals(enginePosition.View());
}

bool GamePersistenceCoordinator::TryGetFenPieceAtSquare(const char* fen,
                                                        const uint8_t square,
                                                        char& outPieceChar) {
    outPieceChar = '\0';
    if (fen == nullptr || fen[0] == '\0') {
        return false;
    }
    if (square >= 64) {
        return false;
    }

    // Parse piece-placement field only; mapping is 0=a8..63=h1.
    int rank = 0;
    int file = 0;
    for (const char* p = fen; *p != '\0' && *p != ' '; ++p) {
        const char c = *p;
        if (c == '/') {
            if (file != 8) {
                return false;
            }
            ++rank;
            file = 0;
            continue;
        }
        if (c >= '1' && c <= '8') {
            const int n = static_cast<int>(c - '0');
            file += n;
            if (file > 8) {
                return false;
            }
            continue;
        }

        const bool isPiece = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
        if (!isPiece) {
            return false;
        }

        if (rank < 0 || rank >= 8 || file < 0 || file >= 8) {
            return false;
        }

        const uint8_t sq = static_cast<uint8_t>(rank * 8 + file);
        if (sq == square) {
            outPieceChar = c;
            return true;
        }
        ++file;
    }

    return false;
}

bool GamePersistenceCoordinator::FenToOccupancyMask(const char* fen, uint64_t& outOcc64) {
    outOcc64 = 0;

    if (fen == nullptr) {
        return false;
    }

    uint8_t rank = 0;
    uint8_t file = 0;

    for (const char* p = fen; *p != '\0'; ++p) {
        const char c = *p;
        if (c == ' ') {
            break;
        }

        if (c == '/') {
            if (file != 8) {
                return false;
            }
            ++rank;
            file = 0;
            continue;
        }

        if (c >= '1' && c <= '8') {
            const uint8_t n = static_cast<uint8_t>(c - '0');
            file = static_cast<uint8_t>(file + n);
            if (file > 8) {
                return false;
            }
            continue;
        }

        const bool isPiece = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!isPiece) {
            return false;
        }

        if (rank >= 8 || file >= 8) {
            return false;
        }

        const uint8_t sq = static_cast<uint8_t>((rank * 8) + file);
        outOcc64 |= (1ULL << sq);
        ++file;
    }

    return (rank == 7) && (file == 8);
}

uint8_t GamePersistenceCoordinator::MaskToSquareListAscending(const uint64_t mask64,
                                                              uint8_t* outSquares,
                                                              const uint8_t outCapacity) {
    if (outSquares == nullptr || outCapacity == 0) {
        return 0;
    }

    uint8_t count = 0;
    for (uint8_t i = 0; i < 64; ++i) {
        if ((mask64 & (1ULL << i)) == 0) {
            continue;
        }
        if (count >= outCapacity) {
            break;
        }
        outSquares[count] = i;
        ++count;
    }

    return count;
}

bool GamePersistenceCoordinator::MakeUciMove(const uint8_t from,
                                             const uint8_t to,
                                             const char promotionSuffix,
                                             UciMove& outUci) {
    return litch_max::TryFormatUciMove(from, to, promotionSuffix, outUci);
}

void GamePersistenceCoordinator::ApplyEngineFenToStateMachineExpected(const char* fen) {
    uint64_t whiteOcc64 = 0;
    uint64_t blackOcc64 = 0;
    uint64_t kingOcc64 = 0;
    uint64_t rookOcc64 = 0;
    uint64_t pawnOcc64 = 0;

    // Parse piece-placement field only; mapping is 0=a8..63=h1.
    // Uppercase = white, lowercase = black.
    if (fen == nullptr) {
        return;
    }

    uint8_t rank = 0;
    uint8_t file = 0;
    for (const char* p = fen; *p != '\0'; ++p) {
        const char c = *p;
        if (c == ' ') {
            break;
        }

        if (c == '/') {
            if (file != 8) {
                return;
            }
            ++rank;
            file = 0;
            continue;
        }

        if (c >= '1' && c <= '8') {
            const uint8_t n = static_cast<uint8_t>(c - '0');
            file = static_cast<uint8_t>(file + n);
            if (file > 8) {
                return;
            }
            continue;
        }

        const bool isUpper = (c >= 'A' && c <= 'Z');
        const bool isLower = (c >= 'a' && c <= 'z');
        if (!isUpper && !isLower) {
            return;
        }
        if (rank >= 8 || file >= 8) {
            return;
        }

        const uint8_t sq = static_cast<uint8_t>((rank * 8) + file);
        const uint64_t mask = (1ULL << sq);
        if (isUpper) {
            whiteOcc64 |= mask;
        } else {
            blackOcc64 |= mask;
        }

        if (c == 'K' || c == 'k') {
            kingOcc64 |= mask;
        } else if (c == 'R' || c == 'r') {
            rookOcc64 |= mask;
        }
        if (c == 'P' || c == 'p') {
            pawnOcc64 |= mask;
        }
        ++file;
    }

    if (!((rank == 7) && (file == 8))) {
        return;
    }

    const SideToMove stm = SideToMoveFromFen(
        fen, m_engine.GetSideToMove());
    const uint64_t mover = (stm == SideToMove::White) ? whiteOcc64 : blackOcc64;
    const uint64_t opponent = (stm == SideToMove::White) ? blackOcc64 : whiteOcc64;

    // Legal capture targets: opponent-occupied squares that are capturable by
    // at least one mover-side piece.
    uint8_t moverSquares[64] = {};
    const uint8_t moverSquareCount = MaskToSquareListAscending(mover, moverSquares, sizeof(moverSquares));
    runtime::AppEvent position;
    position.type = runtime::AppEventType::PositionAcknowledged;
    // One producer-owned table lives through synchronous acknowledgement and
    // tracing. Ordinary AppEvents and their queue never copy these rows.
    runtime::CaptureLegality captureLegality;
    uint64_t legalCaptureTargetsMask64 = 0;
    bool legalCaptureTargetsValid = true;
    uint64_t legalCastlingKingTargetsMask64 = 0;
    bool legalCastlingTargetsValid = false;
    for (uint8_t i = 0; i < moverSquareCount; ++i) {
        const uint8_t fromSquare = moverSquares[i];
        uint64_t destMask64 = 0;
        const bool legalOk = IsDuckGame()
            ? litch_max::GetDuckLegalDestinationsMask(
                  fen, m_duckSquare, fromSquare, destMask64)
            : m_engine.GetLegalDestinationsMask(
                  fromSquare, destMask64);
        if ((kingOcc64 & (1ULL << fromSquare)) != 0ULL) {
            legalCastlingTargetsValid = legalOk;
            if (legalOk) {
                for (const auto& castle : litch_max::CHESS_CASTLING_PATTERNS) {
                    if (castle.kingFrom == fromSquare) {
                        legalCastlingKingTargetsMask64 |=
                            destMask64 & (1ULL << castle.kingTo);
                    }
                }
            }
        }
        if (!legalOk) {
            legalCaptureTargetsValid = false;
            legalCaptureTargetsMask64 = 0;
            continue;
        }
        captureLegality.knownSources |= 1ULL << fromSquare;
        captureLegality.landingsBySource[fromSquare] = destMask64;
        if (legalCaptureTargetsValid) {
            legalCaptureTargetsMask64 |= (destMask64 & opponent);
        }
    }
    // Chess/FEN occupancy stays duck-free. The interaction model owns the
    // expected physical projection, so its occupied mask includes the tracked
    // neutral duck while all side and piece masks remain chess-only.
    const uint64_t duckMask = IsDuckGame() && m_duckSquare < 64U
                                  ? (1ULL << m_duckSquare)
                                  : 0ULL;
    position.occupiedMask = whiteOcc64 | blackOcc64 | duckMask;
    position.sideToMoveValid = true;
    position.sideToMoveBlack = stm == SideToMove::Black;
    position.moverMask = mover;
    position.opponentMask = opponent;
    position.kingMask = kingOcc64;
    position.rookMask = rookOcc64;
    position.pawnMask = pawnOcc64;
    position.legalCaptureTargetsValid = legalCaptureTargetsValid;
    position.legalCaptureTargetsMask = legalCaptureTargetsMask64;
    position.legalCastlingTargetsValid = legalCastlingTargetsValid;
    position.legalCastlingKingTargetsMask = legalCastlingKingTargetsMask64;

    // En-passant capture-first allowance: if an en-passant target exists and at
    // least one mover pawn can legally capture to it, allow lifting the captured
    // pawn square while Idle (target-first).
    //
    // FEN fields: <placement> <stm> <castling> <ep> ...
    uint8_t epTargetSquare = 0;
    bool epTargetValid = false;
    {
        const char* p = fen;
        for (uint8_t field = 0; field < 3; ++field) {
            while (*p != '\0' && *p != ' ') {
                ++p;
            }
            while (*p == ' ') {
                ++p;
            }
            if (*p == '\0') {
                break;
            }
        }

        if (*p != '\0') {
            if (*p == '-') {
                epTargetValid = false;
            } else {
                epTargetValid = litch_max::TryParseUciSquare(p, epTargetSquare);
            }
        }
    }

    bool epCaptureFirstValid = false;
    uint8_t epCaptureSquare = 0;
    if (epTargetValid && epTargetSquare < 64U) {
        // Captured pawn square is behind the ep target in the mover's direction.
        if (stm == SideToMove::White) {
            if (epTargetSquare <= 55U) {
                epCaptureSquare = static_cast<uint8_t>(epTargetSquare + 8U);
                epCaptureFirstValid = true;
            }
        } else {
            if (epTargetSquare >= 8U) {
                epCaptureSquare = static_cast<uint8_t>(epTargetSquare - 8U);
                epCaptureFirstValid = true;
            }
        }

        // Require: ep target is empty, capture square contains the opponent pawn,
        // and at least one mover pawn has the ep target in its legal mask.
        if (epCaptureFirstValid) {
            char tmp = '\0';
            if (TryGetFenPieceAtSquare(fen, epTargetSquare, tmp)) {
                epCaptureFirstValid = false;
            }
        }

        if (epCaptureFirstValid) {
            char capturedPiece = '\0';
            const char expectedCapturedPawn = (stm == SideToMove::White) ? 'p' : 'P';
            if (!TryGetFenPieceAtSquare(fen, epCaptureSquare, capturedPiece) || capturedPiece != expectedCapturedPawn) {
                epCaptureFirstValid = false;
            }
        }

        if (epCaptureFirstValid) {
            const uint8_t epFile = static_cast<uint8_t>(epTargetSquare % 8U);

            // Candidate capturing pawn squares (diagonally behind the ep target).
            // White captures "up" (toward a8): pawns are one rank below ep target.
            // Black captures "down" (toward h1): pawns are one rank above ep target.
            uint8_t fromSquares[2] = {255, 255};
            uint8_t fromCount = 0;
            if (stm == SideToMove::White) {
                if (epFile > 0U && epTargetSquare <= 56U) {
                    fromSquares[fromCount++] = static_cast<uint8_t>(epTargetSquare + 7U);
                }
                if (epFile < 7U && epTargetSquare <= 54U) {
                    fromSquares[fromCount++] = static_cast<uint8_t>(epTargetSquare + 9U);
                }
            } else {
                if (epFile > 0U && epTargetSquare >= 9U) {
                    fromSquares[fromCount++] = static_cast<uint8_t>(epTargetSquare - 9U);
                }
                if (epFile < 7U && epTargetSquare >= 7U) {
                    fromSquares[fromCount++] = static_cast<uint8_t>(epTargetSquare - 7U);
                }
            }

            const char expectedMoverPawn = (stm == SideToMove::White) ? 'P' : 'p';

            bool anyLegal = false;
            for (uint8_t i = 0; i < fromCount; ++i) {
                const uint8_t fromSq = fromSquares[i];
                if (fromSq >= 64U) {
                    continue;
                }

                char moverPiece = '\0';
                if (!TryGetFenPieceAtSquare(fen, fromSq, moverPiece) || moverPiece != expectedMoverPawn) {
                    continue;
                }

                const uint64_t destMask64 = captureLegality.landingsBySource[fromSq];
                if (((destMask64 >> epTargetSquare) & 1ULL) != 0ULL) {
                    anyLegal = true;
                    break;
                }
            }

            if (!anyLegal) {
                epCaptureFirstValid = false;
            }
        }
    }

    position.enPassantTargetSquare = epCaptureFirstValid
        ? epTargetSquare : runtime::CHESS_INTERACTION_INVALID_SQUARE;
    position.enPassantCaptureSquare = epCaptureFirstValid
        ? epCaptureSquare : runtime::CHESS_INTERACTION_INVALID_SQUARE;
    const uint64_t epLanding = epCaptureFirstValid ? 1ULL << epTargetSquare : 0ULL;
    for (uint8_t i = 0U; i < moverSquareCount; ++i) {
        const uint8_t fromSquare = moverSquares[i];
        captureLegality.landingsBySource[fromSquare] &=
            opponent | ((pawnOcc64 & (1ULL << fromSquare)) != 0ULL ? epLanding : 0ULL);
    }
    DispatchInteractionPositionAcknowledged(position, captureLegality);
}

} // namespace modules
