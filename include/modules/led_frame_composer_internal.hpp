#pragma once

#include <array>
#include <cstdint>

#include "modules/animation_timing.hpp"
#include "modules/board_led_frame.hpp"
#include "modules/checkmate_animation_setting.hpp"
#include "modules/correction_feedback.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/persisted_side.hpp"
#include "modules/settle_animation_setting.hpp"

namespace modules::led_frame_composer {

// Existing helpers defined in:
// - src/led_frame_composer_helpers_castling.cpp
// - src/led_frame_composer_helpers_checkmate.cpp
bool TryInferTransitionPieceSide(
    const runtime::InteractionModelSnapshot& snapshot,
    uint64_t physicalOcc64,
    uint8_t logicalSq,
    modules::LitchMaxEngine::PieceSide& outSide);
modules::Rgb ScaleRgbSaturating(modules::Rgb c, uint16_t scale256);
bool TryComputeCheckmateContributorsMask(
    const runtime::InteractionModelSnapshot& snapshot,
    uint64_t& outContributorsMask64);

namespace internal {

bool TryGetPlacedRelocationDestinationSide(
    const runtime::InteractionModelSnapshot& snapshot,
    uint64_t physicalOcc64,
    uint8_t logicalSquare,
    modules::LitchMaxEngine::PieceSide& outSide);

enum class CelebrationIntent : uint8_t {
    None = 0,
    Checkmate = 1,
    Draw = 2,
    VariantVictory = 3,
    ClockVictory = 4,
};

struct RenderIntentInputs {
    const runtime::InteractionModelSnapshot* snapshot = nullptr;
    bool menuRequested = false;
    bool menuVisualActive = false;
    bool hintMoveValid = false;
};

struct RenderIntent {
    bool menuForegroundVisible = false;
    bool menuBackgroundDimVisible = false;
    bool centerTextOverlayRequested = false;
    bool centerTextOverlayVisible = false;
    bool hintPromptRequested = false;
    bool hintPromptVisible = false;
    bool coachingOverlayActive = false;
    CelebrationIntent celebration = CelebrationIntent::None;
    uint32_t celebrationElapsedMs = 0U;

    bool HasCelebration() const { return celebration != CelebrationIntent::None; }
};

RenderIntent BuildRenderIntent(const RenderIntentInputs& inputs);

// Baseline eligibility shared with physical X admission. BuildComposeContext
// additionally resolves correction and system-status precedence before the
// compositor exports visible king-assist ownership.
bool CanInitialKingPlacementAssistOwnBoard(
    const fxe::SettingsStore& settings,
    const runtime::InteractionModelSnapshot& snapshot,
    const RenderIntent& renderIntent);

struct InvalidBoardState {
    uint64_t missingMask = 0ULL;
    uint64_t extraMask = 0ULL;
    uint64_t contextMask = 0ULL;
    uint32_t elapsedMs = 0U;

    uint64_t Mask() const { return missingMask | extraMask | contextMask; }
    bool IsActive() const { return Mask() != 0ULL; }
};

struct ComposeContext {
    uint32_t nowMs = 0;

    ComposeSettings settings{};
    bool minimumBrightness = false;
    const ComposeInputs* in = nullptr;
    // Valid only for this synchronous composition. The input owner retains
    // the immutable value; no live game/settings pointer enters presentation.
    const runtime::InteractionModelSnapshot* snapshot = nullptr;
    const runtime::InteractionModelSnapshot& Snapshot() const { return *snapshot; }
    RenderIntent renderIntent{};
    modules::BoardStylePreviewFrame boardStylePreview{};

    bool firstPlayerIsP2 = false;

    bool freshSetupActive = false;
    bool freshSetupIllegalPickup = false;
    bool initialKingPlacementPromptActive = false;
    uint64_t initialKingPlacementPromptMask = 0ULL;
    uint32_t initialKingPlacementPromptElapsedMs = 0U;

    // GAME_FIRST_PLAYER compatibility storage: 1 => P1 at the normal physical
    // end, 2 => P1 at the opposite physical end. P1 always owns White; the
    // p1/p2 color slots below select the reference-end palettes for that
    // placement. Literal board rotation then moves those colors with the UI.

    modules::Rgb boostedLightSquareRgb = modules::RGB_BLACK;
    modules::Rgb boostedDarkSquareRgb = modules::RGB_BLACK;

    uint8_t p1Wheel = 0;
    uint8_t p2Wheel = 0;
    modules::Rgb p1PieceRgb = modules::RGB_WHITE;
    modules::Rgb p2PieceRgb = modules::RGB_WHITE;

    // Accent/highlight variants derived from the active physical-end wheel.
    // Used by edge UI so the turn indicator reads like a UI highlight, not a piece color.
    modules::Rgb p1AccentRgb = modules::RGB_WHITE;
    modules::Rgb p2AccentRgb = modules::RGB_WHITE;

    modules::CheckmateAnimationStyle checkmateAnimationStyle =
        modules::CheckmateAnimationStyle::FireLob;
    bool minimalAnimations = false;
    modules::SettleAnimationStyle settleAnimationStyle =
        modules::settle_animation_setting::DEFAULT_STYLE;

    modules::UiOwner edgeActiveOwner = modules::UiOwner::None;
    modules::UiOwner activeOwner = modules::UiOwner::None;
    uint8_t activeWheel = 0;
    modules::Rgb activePromptRgb = modules::RGB_WHITE;
    modules::Rgb availablePromptRgb = modules::RGB_WHITE;

    uint64_t validDestMask = 0;
    uint64_t castlingKingDestinationMask = 0;
    uint64_t castlingRookSourceMask = 0;
    uint8_t moveFromSquare = 255;
    uint8_t moveToSquare = 255;

    uint8_t hintFromSquare = 0;
    uint8_t hintToSquare = 0;
    bool hintMoveValid = false;
    uint8_t hintCaptureSquare = 255U;
    uint8_t hintRefinementAlpha255 = 255U;
    uint8_t hintDuckFromSquare = 0;
    uint8_t hintDuckToSquare = 0;
    bool hintDuckMoveValid = false;
    bool coachingOverlayActive = false;

    bool settleOn = false;
    uint32_t settleElapsedMs = 0;
    uint32_t settleDurationMs = 0;
    bool settleTimingValid = false;
    uint32_t settleVisualElapsedMs = 0;
    uint32_t settlePresentationDurationMs = 0;
    uint8_t settleVariant = 0;
    modules::Rgb settleMoverPieceRgb = modules::RGB_WHITE;

    bool settleAfterglowActive = false;
    uint8_t settleAfterglowSquare = 255;
    uint32_t settleAfterglowElapsedMs = 0;
    modules::Rgb settleAfterglowRgb = modules::RGB_WHITE;

    bool p1LastMoveValid = false;
    uint8_t p1LastMoveFrom = 255;
    uint8_t p1LastMoveTo = 255;
    bool p2LastMoveValid = false;
    uint8_t p2LastMoveFrom = 255;
    uint8_t p2LastMoveTo = 255;
    bool lastMoveMostRecentKnown = false;
    bool lastMoveMostRecentIsP1 = false;

    uint8_t undoFromSquare = 0;
    uint8_t undoToSquare = 0;
    bool undoPromptActive = false;
    modules::PersistedSide undoMoverSidePersist = modules::PersistedSide::Unknown;
    bool undoMoverSideKnown = false;
    uint8_t undoRestoreSquare = 0;
    char undoRestorePieceFen = '\0';
    bool undoRestoreValid = false;
    char undoMoverPieceFen = '\0';
    bool undoMoverPieceFenKnown = false;
    modules::Rgb undoMoverPieceRgb = modules::RGB_WHITE;
    modules::Rgb undoRestorePieceRgb = modules::RGB_WHITE;
    modules::Rgb undoMoverPromptRgb = modules::RGB_WHITE;
    modules::Rgb undoRestorePromptRgb = modules::RGB_WHITE;
    bool undoMoverReturned = false;
    bool undoRestorePhase = false;
    bool undoMoverPhase = false;

    bool kingQueenSwapPromptActive = false;
    uint64_t kingQueenSwapPromptMask = 0;
    uint64_t kingQueenSwapPromptFlashMask = 0;

    uint8_t promoFromSquare = 0;
    uint8_t promoToSquare = 0;
    bool promoMoverIsBlack = false;
    bool promoMoveValid = false;
    modules::Rgb promoMoverPieceRgb = modules::RGB_WHITE;

    uint8_t pendingCaptureSquare = 0;
    bool pendingCaptureSquareValid = false;

    bool cpuTargetOccValid = false;
    uint64_t physicalOcc64 = 0;
    uint64_t expectedOcc64 = 0;

    uint64_t expectedMoverOcc64 = 0;
    uint64_t expectedOpponentOcc64 = 0;

    uint64_t partialCastlingTargetOcc64 = 0;
    bool partialCastlingTargetOccValid = false;

    uint64_t humanIllegalMask = 0;
    uint64_t cpuIllegalMask = 0;
    uint64_t cpuTentativeCorrectionMask = 0;
    bool illegalOn = false;
    uint32_t illegalElapsedMs = 0;
    uint32_t illegalPeriodMs = 1;
    bool kingSafetyPromptActive = false;
    uint8_t kingSafetyKingSquare = 255;
    uint8_t kingSafetyAttackerSquare = 255;
    uint64_t captureReturnMask = 0;
    bool captureReturnOn = false;
    uint32_t captureReturnElapsedMs = 0;

    bool allPiecesFlashActive = false;
    bool allPiecesFlashOn = false;

    // Classified once per frame, then shared by board and edge renderers.
    InvalidBoardState invalidBoard{};
    modules::CorrectionFeedback correctionFeedback = modules::CorrectionFeedback::Strong;
};

struct CheckmateBoardAnimationInputs {
    modules::CheckmateAnimationStyle style =
        modules::CheckmateAnimationStyle::FireLob;
    uint32_t elapsedMs = 0U;
    uint32_t seed = 1U;
    uint64_t contributorPhysicalMask = 0ULL;
    uint64_t winnerPhysicalMask = 0ULL;
    uint8_t losingKingPhysicalSquare = 255U;
    modules::Rgb winnerRgb = modules::RGB_WHITE;
    bool duckKingCaptured = false;
    uint8_t captureFromSquare = 255U;
    uint8_t captureToSquare = 255U;
};

struct BoardComposeRuntime {
    modules::UiOwner expectedMoverOwner = modules::UiOwner::None;
    modules::UiOwner expectedOpponentOwner = modules::UiOwner::None;

    modules::animation::AnimationContext globalAnim{};
    modules::animation::AnimationContext undoAnim{};
    modules::animation::AnimationContext illegalAnim{};

    modules::animation::ValueToken faultBlinkToken{};
    modules::animation::ValueToken lastMovePulseToken{};
    modules::animation::ValueToken dangerPulseToken{};

    bool moverPieceBlinkOn = false;
    bool undoMoverPieceBlinkOn = false;
    uint8_t mismatchRainbowBaseWheel = 0;

    bool undoMoveValid = false;
    bool undoMoverExpectedPresent = false;
    bool undoMoverExpectedMissing = false;
    uint64_t undoMoverInitialMask = 0ULL;
    uint64_t undoRestoreInitialMask = 0ULL;
    uint64_t undoCandidateBlinkMask = 0ULL;
    modules::Rgb undoCandidateBlinkRgb = modules::RGB_WHITE;
    bool undoCandidateBlinkActive = false;

    bool undoCastlingRookMoveValid = false;
    uint8_t undoCastlingRookFrom = 0;
    uint8_t undoCastlingRookTo = 0;
};

struct BoardSquarePromptState {
    bool useMismatchRainbow = false;
    modules::BoardSquareLeds mismatchRainbowLeds{};
    uint16_t pieceFinalScale256 = 256U;
};

uint16_t UndoCandidatePiecePulseScale256(
    const BoardComposeRuntime& runtime);
void ApplyKingQueenSwapPairShuttle(const ComposeContext& ctx,
                                   uint8_t logicalSq,
                                   bool occupied,
                                   modules::BoardSquareLeds& leds);
void ApplyDirectedMoveLanding(const ComposeContext& ctx,
                              modules::BoardLedFrame& board);
void ApplyHintMovePrompt(const ComposeContext& ctx,
                         const BoardComposeRuntime& runtime,
                         uint8_t logicalSq,
                         modules::Rgb& ioFinalSquare,
                         BoardSquarePromptState& ioPromptState);

void ApplyHeldHintMovePrompt(const ComposeContext& ctx,
                            const BoardComposeRuntime& runtime,
                            uint8_t logicalSq,
                            modules::Rgb& ioFinalSquare);
void ApplyFreshSetupPresentation(
    const ComposeContext& ctx,
    const BoardComposeRuntime& runtime,
    bool forceOff,
    bool occupied,
    bool isDuckPiece,
    uint8_t logicalSq,
    modules::Rgb& ioFinalSquare,
    modules::Rgb& ioPieceRgb,
    BoardSquarePromptState& ioPromptState);

inline modules::UiOwner OwnerForPieceSide(
    const modules::LitchMaxEngine::PieceSide pieceSide,
    const bool firstPlayerIsP2) {
    (void)firstPlayerIsP2;
    switch (pieceSide) {
        case modules::LitchMaxEngine::PieceSide::White:
            return modules::UiOwner::P1;
        case modules::LitchMaxEngine::PieceSide::Black:
            return modules::UiOwner::P2;
        case modules::LitchMaxEngine::PieceSide::None:
        default:
            return modules::UiOwner::None;
    }
}

inline modules::UiOwner OwnerForPersistedSide(
    const modules::PersistedSide side,
    const bool firstPlayerIsP2) {
    (void)firstPlayerIsP2;
    switch (side) {
        case modules::PersistedSide::White:
            return modules::UiOwner::P1;
        case modules::PersistedSide::Black:
            return modules::UiOwner::P2;
        case modules::PersistedSide::Unknown:
        default:
            return modules::UiOwner::None;
    }
}

inline modules::Rgb PieceRgbForOwner(const ComposeContext& ctx,
                                     const modules::UiOwner owner) {
    if (owner == modules::UiOwner::None) {
        return modules::RGB_BLACK;
    }
    return (owner == modules::UiOwner::P2) ? ctx.p2PieceRgb : ctx.p1PieceRgb;
}

inline modules::Rgb PieceRgbForPieceSide(const ComposeContext& ctx,
                                         const modules::LitchMaxEngine::PieceSide pieceSide) {
    const modules::UiOwner owner = OwnerForPieceSide(pieceSide, ctx.firstPlayerIsP2);
    if (owner == modules::UiOwner::None) {
        return modules::RGB_BLACK;
    }
    return PieceRgbForOwner(ctx, owner);
}

bool BuildComposeContext(const ComposeInputs& in, ComposeContext& outCtx);

InvalidBoardState ComputeInvalidBoardState(const ComposeContext& ctx);
void ConfigureCorrectionFeedback(ComposeContext& ctx);
void ApplyCorrectionSquarePrompt(const ComposeContext& ctx,
                                 const BoardComposeRuntime& runtime,
                                 bool occupied, uint8_t logicalSq,
                                 modules::Rgb& ioFinalSquare,
                                 modules::Rgb& ioPieceRgb,
                                 BoardSquarePromptState& ioPromptState);

BoardComposeRuntime BuildBoardComposeRuntime(const ComposeContext& ctx);

modules::BoardSquareLeds BuildMismatchRainbowLeds(const BoardComposeRuntime& runtime,
                                                  uint8_t logicalSq);

void ApplyThreatOverlays(const ComposeContext& ctx,
                         const BoardComposeRuntime& runtime,
                         bool forceOff,
                         uint8_t logicalSq,
                         modules::Rgb& ioFinalSquare,
                         modules::Rgb& ioPieceRgb);

void ApplyLastMovePulse(const ComposeContext& ctx,
                        const BoardComposeRuntime& runtime,
                        bool forceOff,
                        bool occupied,
                        uint8_t logicalSq,
                        modules::Rgb& ioFinalSquare,
                        modules::Rgb& ioPieceRgb);

void ApplyPlacementEmitterEffects(const ComposeContext& ctx, modules::BoardLedFrame& board);
bool ApplyPromotionReveal(const ComposeContext& ctx, modules::BoardLedFrame& board,
                          std::array<modules::Rgb, 64>& grid);

void ApplyPlacementStylePreview(const ComposeContext& ctx,
                                modules::BoardLedFrame& board,
                                std::array<modules::Rgb, 64>& grid);

void ApplyPlacementEffects(const ComposeContext& ctx,
                           bool forceOff,
                           uint8_t logicalSq,
                           modules::Rgb& ioFinalSquare);

void ApplySquareStatePrompts(const ComposeContext& ctx,
                             const BoardComposeRuntime& runtime,
                             bool forceOff,
                             bool occupied,
                             uint8_t logicalSq,
                             modules::Rgb& ioFinalSquare,
                             modules::Rgb& ioPieceRgb,
                             BoardSquarePromptState& ioPromptState);

void ApplyDirectedMovePrompt(const ComposeContext& ctx,
                             const BoardComposeRuntime& runtime,
                             bool forceOff,
                             bool occupied,
                             uint8_t logicalSq,
                             modules::Rgb& ioFinalSquare,
                             modules::Rgb& ioPieceRgb);

bool IsHintCaptureRemovalSquare(const ComposeContext& ctx, uint8_t logicalSq);

uint64_t PhysicalRemovalSquareMask(const ComposeContext& ctx);
uint64_t ApplyPhysicalRemovalBrackets(const ComposeContext& ctx, modules::BoardLedFrame& board);

uint64_t ComposeBoard(const ComposeContext& ctx,
                      modules::BoardLedFrame& outLedFrame,
                      std::array<modules::Rgb, 64>& outGridFrame);

void ApplyMenuBackgroundDim(const ComposeContext& ctx,
                            modules::BoardLedFrame& outLedFrame,
                            std::array<modules::Rgb, 64>& outGridFrame);

void ApplyMenuOverlay(const ComposeContext& ctx,
                      modules::BoardLedFrame& outLedFrame,
                      std::array<modules::Rgb, 64>& outGridFrame,
                      modules::GridForegroundLayer* outMenuForeground);

void ApplyCenterTextOverlay(const ComposeContext& ctx,
                            modules::BoardLedFrame& outLedFrame,
                            std::array<modules::Rgb, 64>& outGridFrame);

void ApplyCheckmateCelebration(const ComposeContext& ctx,
                               modules::BoardLedFrame& outLedFrame,
                               std::array<modules::Rgb, 64>& outGridFrame);

void ApplyCheckmateBoardAnimation(
    const CheckmateBoardAnimationInputs& inputs,
    modules::BoardLedFrame& outLedFrame,
    std::array<modules::Rgb, 64>& outGridFrame);

void ApplyFullCheckmateFireworks(
    const CheckmateBoardAnimationInputs& inputs,
    modules::BoardLedFrame& outLedFrame,
    std::array<modules::Rgb, 64>& outGridFrame);

void ApplyDrawCelebration(const ComposeContext& ctx,
                          modules::BoardLedFrame& outLedFrame,
                          std::array<modules::Rgb, 64>& outGridFrame);

bool ComposeEdgeLeds(const ComposeContext& ctx,
                     modules::BoardLedFrame& outLedFrame,
                     std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdgeFrame,
                     modules::status_led_frame_adapter::FixedPhysicalStatusOverlay*
                         outFixedPhysicalStatusOverlay);

void ApplyGlobalBrightnessBoost(modules::BoardLedFrame& outLedFrame,
                                std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdgeFrame,
                                std::array<modules::Rgb, 64>& outGridFrame);

void ApplyGlobalBrightnessBoost(modules::BoardLedFrame& outLedFrame);
void ApplyGlobalBrightnessBoost(modules::GridForegroundLayer& layer);

} // namespace internal
} // namespace modules::led_frame_composer
