#pragma once

#include <cstdint>

#include "modules/engine.hpp"

namespace modules {


enum class CoachPriority : uint8_t {
    General = 0,
    Positional,
    Tactical,
    Urgent,
};

// V1 names only facts the compact engine result can prove. More specific
// tactical and strategic topics must not be added without stronger evidence.
enum class CoachTopic : uint8_t {
    None = 0,
    EscapeCheck,
    MateInOne,
    Promotion,
    Capture,
    GiveCheck,
    Castle,
    GeneralMove,
};

enum class CoachPieceType : uint8_t {
    None = 0,
    Pawn,
    Knight,
    Bishop,
    Rook,
    Queen,
    King,
};

enum CoachMoveFact : uint8_t {
    CoachMoveCaptures = 1U << 0U,
    CoachMoveGivesCheck = 1U << 1U,
    CoachMoveGivesMate = 1U << 2U,
    CoachMoveCastles = 1U << 3U,
    CoachMovePromotes = 1U << 4U,
};

// Language-neutral MCU record. The app owns wording and progressive reveal;
// no prose, candidate arrays, or dynamic storage lives on the board.
struct CoachInsight {
    CoachTopic topic = CoachTopic::None;
    CoachPriority priority = CoachPriority::General;
    SideToMove moverSide = SideToMove::White;
    uint8_t moveFacts = 0U;
    uint8_t fromSquare = 64U;
    uint8_t toSquare = 64U;
    uint8_t captureSquare = 64U;
    CoachPieceType movingPiece = CoachPieceType::None;
    CoachPieceType capturedPiece = CoachPieceType::None;
    CoachPieceType promotionPiece = CoachPieceType::None;
};

static_assert(sizeof(CoachInsight) <= 16U,
              "coach insight must stay a lightweight fixed-size record");

// Builds one truthful observation from a completed Classic result. The result
// must carry an engine-verified outcome for candidate zero. Alternatives are
// intentionally ignored because their alpha-beta bounds are not comparable.
bool TryBuildCoachInsight(const char* fen,
                          SideToMove moverSide,
                          bool startsInCheck,
                          const ClassicThinkResult& result,
                          CoachInsight& outInsight);

} // namespace modules
