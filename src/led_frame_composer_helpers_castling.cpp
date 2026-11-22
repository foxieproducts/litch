#include "modules/led_frame_composer_internal.hpp"

namespace modules::led_frame_composer {

namespace {

modules::LitchMaxEngine::PieceSide SideForFenPiece(const char piece) {
    if (piece >= 'A' && piece <= 'Z') {
        return modules::LitchMaxEngine::PieceSide::White;
    }
    if (piece >= 'a' && piece <= 'z') {
        return modules::LitchMaxEngine::PieceSide::Black;
    }
    return modules::LitchMaxEngine::PieceSide::None;
}

} // namespace

bool TryInferTransitionPieceSide(
    const runtime::InteractionModelSnapshot& snapshot,
    const uint64_t physicalOcc64,
    const uint8_t logicalSq,
    modules::LitchMaxEngine::PieceSide& outSide) {
    outSide = modules::LitchMaxEngine::PieceSide::None;
    const auto isPhysicalOccupied = [&](const uint8_t sq) -> bool {
        return (sq < 64U) && (((physicalOcc64 >> sq) & 1ULL) != 0ULL);
    };
    if (!isPhysicalOccupied(logicalSq) ||
        !snapshot.board.enginePiecesValid ||
        !runtime::HasActivePhysicalPlan(snapshot.interaction)) {
        return false;
    }

    const runtime::PhysicalTransitionPlan& plan =
        snapshot.interaction.taggedState.plan;
    for (uint8_t i = 0U; i < plan.relocationCount; ++i) {
        const runtime::PieceRelocation& relocation = plan.relocations[i];
        uint8_t modeledSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
        if (logicalSq == relocation.to &&
            !isPhysicalOccupied(relocation.from)) {
            modeledSquare = relocation.from;
        } else if (logicalSq == relocation.from &&
                   !isPhysicalOccupied(relocation.to)) {
            modeledSquare = relocation.to;
        }
        if (modeledSquare < 64U) {
            outSide = SideForFenPiece(
                snapshot.board.enginePieces[modeledSquare]);
            if (outSide != modules::LitchMaxEngine::PieceSide::None) {
                return true;
            }
        }
    }
    return false;
}

modules::Rgb ScaleRgbSaturating(const modules::Rgb c, const uint16_t scale256) {
    return modules::ScaleRgbSaturatingBy256(c, scale256);
}

} // namespace modules::led_frame_composer
