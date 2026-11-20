#include "modules/led_frame_composer_internal.hpp"

namespace modules::led_frame_composer {
namespace internal {
namespace {

constexpr uint16_t MISSING_SETUP_SQUARE_SCALE256 = 64U;
constexpr uint32_t KING_ASSIST_CADENCE_MS = 1600U;
constexpr uint32_t KING_ASSIST_FLASH_WINDOW_MS = 1200U;
constexpr uint32_t KING_ASSIST_FLASH_STEP_MS = 200U;
constexpr uint16_t KING_ASSIST_FLASH_SCALE256 = 512U;
// Double unsaturated flash channels and lower the rest bracket for contrast.
// Saturating RGB scaling and the normal output/current limits still apply.
constexpr uint16_t KING_ASSIST_REST_SCALE256 = 72U;
constexpr uint16_t KING_ASSIST_CENTER_SCALE256 = 96U;

modules::LitchMaxEngine::PieceSide SideForFenPiece(const char piece) {
    if (piece >= 'A' && piece <= 'Z') {
        return modules::LitchMaxEngine::PieceSide::White;
    }
    if (piece >= 'a' && piece <= 'z') {
        return modules::LitchMaxEngine::PieceSide::Black;
    }
    return modules::LitchMaxEngine::PieceSide::None;
}

modules::UiOwner ExpectedOwnerAt(
    const ComposeContext& ctx,
    const BoardComposeRuntime& runtime,
    const uint8_t logicalSq) {
    const auto& snapshot = ctx.Snapshot();
    if (snapshot.board.enginePiecesValid) {
        const modules::LitchMaxEngine::PieceSide side =
            SideForFenPiece(snapshot.board.enginePieces[logicalSq]);
        const modules::UiOwner owner = OwnerForPieceSide(
            side, ctx.firstPlayerIsP2);
        if (owner != modules::UiOwner::None) {
            return owner;
        }
    }

    const uint64_t squareMask = 1ULL << logicalSq;
    if ((ctx.expectedMoverOcc64 & squareMask) != 0ULL) {
        return runtime.expectedMoverOwner;
    }
    if ((ctx.expectedOpponentOcc64 & squareMask) != 0ULL) {
        return runtime.expectedOpponentOwner;
    }
    return modules::UiOwner::None;
}

void ApplyExtraSetupSquare(
    const ComposeContext& ctx,
    const BoardComposeRuntime& runtime,
    const bool occupied,
    const bool isDuckPiece,
    const uint8_t logicalSq,
    modules::Rgb& ioFinalSquare,
    modules::Rgb& ioPieceRgb,
    BoardSquarePromptState& ioPromptState) {
    if (!occupied || isDuckPiece) {
        return;
    }

    ApplyCorrectionSquarePrompt(ctx, runtime, occupied, logicalSq,
                                ioFinalSquare, ioPieceRgb, ioPromptState);
}

bool IsKingAssistFlashOn(const ComposeContext& ctx) {
    const uint32_t localMs =
        ctx.initialKingPlacementPromptElapsedMs % KING_ASSIST_CADENCE_MS;
    return localMs < KING_ASSIST_FLASH_WINDOW_MS &&
           ((localMs / KING_ASSIST_FLASH_STEP_MS) % 2U) == 0U;
}

void ApplyInitialKingPlacementAssist(
    const ComposeContext& ctx,
    const BoardComposeRuntime& runtime,
    const bool occupied,
    const uint8_t logicalSq,
    modules::Rgb& ioFinalSquare,
    modules::Rgb& ioPieceRgb) {
    if (!ctx.initialKingPlacementPromptActive ||
        (ctx.initialKingPlacementPromptMask & (1ULL << logicalSq)) == 0ULL) {
        return;
    }

    const modules::UiOwner owner = ExpectedOwnerAt(ctx, runtime, logicalSq);
    if (owner == modules::UiOwner::None) {
        return;
    }

    const modules::Rgb ownerRgb = PieceRgbForOwner(ctx, owner);
    ioFinalSquare = ScaleRgbSaturating(
        ownerRgb,
        IsKingAssistFlashOn(ctx)
            ? KING_ASSIST_FLASH_SCALE256
            : KING_ASSIST_REST_SCALE256);
    if (occupied) {
        // Keep the center steady while the two side LEDs carry the cadence.
        // Ready does not have the ordinary FreshSetup wash, so set the same
        // restrained center explicitly in both states.
        ioPieceRgb = ScaleRgbSaturating(
            ownerRgb, KING_ASSIST_CENTER_SCALE256);
    }
}

} // namespace

void ApplyFreshSetupPresentation(
    const ComposeContext& ctx,
    const BoardComposeRuntime& runtime,
    const bool forceOff,
    const bool occupied,
    const bool isDuckPiece,
    const uint8_t logicalSq,
    modules::Rgb& ioFinalSquare,
    modules::Rgb& ioPieceRgb,
    BoardSquarePromptState& ioPromptState) {
    if (forceOff || logicalSq >= 64U) {
        return;
    }

    // Fresh Setup's calm missing-piece guidance must not hide an actual
    // wrong-side interaction. The ordinary correction renderer below owns
    // that state so the player sees the same unmistakable feedback as play.
    if (ctx.freshSetupIllegalPickup) {
        return;
    }

    if (ctx.freshSetupActive) {
        const uint64_t squareMask = 1ULL << logicalSq;
        const bool expected = (ctx.expectedOcc64 & squareMask) != 0ULL;
        // Square color guides placement; the center remains detection-only.
        // Both home sides remain useful while leftover pieces need removal.
        if (expected && !occupied) {
            const modules::UiOwner owner = ExpectedOwnerAt(ctx, runtime, logicalSq);
            ioFinalSquare = ScaleRgbSaturating(PieceRgbForOwner(ctx, owner),
                                               MISSING_SETUP_SQUARE_SCALE256);
        } else if ((ctx.invalidBoard.extraMask & squareMask) != 0ULL) {
            ApplyExtraSetupSquare(
                ctx,
                runtime,
                occupied,
                isDuckPiece,
                logicalSq,
                ioFinalSquare,
                ioPieceRgb,
                ioPromptState);
        }
    }

    // Optional teaching must not compete with the required first step. Once
    // leftover pieces are gone, the optional king bracket may return.
    if (ctx.invalidBoard.extraMask == 0ULL) {
        ApplyInitialKingPlacementAssist(ctx,
                                        runtime,
                                        occupied,
                                        logicalSq,
                                        ioFinalSquare,
                                        ioPieceRgb);
    }
}

} // namespace internal
} // namespace modules::led_frame_composer
