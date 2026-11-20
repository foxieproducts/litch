#include "modules/water_slosh_presentation.hpp"

#include "modules/status_led_frame_adapter.hpp"

#if defined(LITCH_ESP32S3)
// This runs inside the nested render call chain. A whole BoardLedFrame here
// added 768 automatic bytes even when the effect was inactive. Enforce the
// budget with the real S3 compiler, not a roomy host/native stack.
#pragma GCC diagnostic error "-Wframe-larger-than=128"
#endif

namespace modules {

#if defined(LITCH_ESP32S3)
// The existing shared blend is already linked by other presentations. Keep a
// call here instead of another three-channel copy in this 288-pixel loop.
// GCC permits this TU-local override but warns that its header was inline.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
__attribute__((noinline)) Rgb AlphaBlendRgb(Rgb base, Rgb overlay, uint8_t alpha255);
#pragma GCC diagnostic pop
#endif

uint8_t WaterSloshRiderSquare(const runtime::InteractionModelSnapshot& snapshot) {
    const auto& game = snapshot.game;
    const uint8_t square = game.variant == GameVariant::Duck ? game.duckSquare :
        game.variant == GameVariant::Undecided ? game.initialDuckCandidateSquare : 255U;
    return square < 64U && snapshot.board.physicalOccupiedMask == (1ULL << square)
        ? square : 255U;
}

bool CanPresentWaterSlosh(
    const runtime::InteractionModelSnapshot& snapshot,
    const WaterSloshPresentationContext& context) {
    const auto& interaction = snapshot.interaction;
    const auto phase = interaction.taggedState.tag;
    const uint8_t rider = WaterSloshRiderSquare(snapshot);
    const uint64_t riderMask = rider < 64U ? 1ULL << rider : 0ULL;
    // Physical emptiness is the stable pickup signal. Do not couple it to the
    // persisted setup lifecycle: restoring a standard game after reboot can
    // classify the same empty board as Gameplay and otherwise make water
    // disappear until New Game. Only the authoritative neutral Vex may ride
    // on an otherwise empty board.
    return context.boardObserved && context.awake &&
           !context.higherPriorityActive && !context.minimalAnimations &&
           context.healthState == runtime::RuntimeHealthState::Healthy &&
           (snapshot.board.physicalOccupiedMask == 0ULL || rider < 64U) &&
           (phase == runtime::ChessInteractionPhase::Idle ||
            phase == runtime::ChessInteractionPhase::Correction) &&
           (interaction.placementExtraMask & ~riderMask) == 0ULL &&
           !interaction.undoPromptActive && !interaction.kingQueenSwapPromptActive &&
           // The existing chess renderer exempts the authoritative duck from
           // raw illegal/extra feedback; preserve exactly that single bit.
           ((interaction.humanIllegalMask | interaction.cpuIllegalMask) & ~riderMask) == 0ULL &&
           // Background hint calculation is deliberately invisible and the
           // ordinary LED resolver treats it as idle. Only a requested hint
           // presentation below owns the board; CPU play remains exclusive.
           !snapshot.game.cpuThinking &&
           !snapshot.game.inCheck && !snapshot.game.inCheckmate &&
           !snapshot.game.inDraw && !snapshot.game.inStalemate &&
           snapshot.game.clockWinner == GameWinner::None &&
           !snapshot.game.variantOutcome.IsTerminal() &&
           !snapshot.game.clockRunning &&
           !snapshot.render.hintDisplayRequested &&
           !snapshot.render.textOverlayActive && !snapshot.render.menuOverlayActive &&
           !snapshot.render.quickSettingsOverlayActive && !snapshot.render.bootFadeActive &&
           !snapshot.system.safeModeActive && !snapshot.system.hardwareTestActive &&
           !snapshot.system.amptestActive && !snapshot.system.otaInProgress;
}

void ApplyWaterSloshPhysicalPresentation(
    const WaterSloshSession& water,
    const hardware::BoardRevision revision,
    hardware::PhysicalLedFrame& physicalFrame,
    const uint8_t physicalRiderSquare) {
    if (!water.IsActive() || water.Opacity255() == 0U) { return; }
    const uint8_t alpha = water.Opacity255();
    const auto topology = hardware::BoardLedTopologyFor(revision);
    for (uint8_t square = 0U; square < 64U; ++square) {
        const bool v3 = hardware::HasFourLedSquares(
            hardware::ResolveBoardRevision(revision));
        const uint8_t count = v3 ? 4U : 3U;
        for (uint8_t slot = 0U; slot < count; ++slot) {
            const uint16_t targetIndex =
                static_cast<uint16_t>(square * count + slot);
            Rgb overlay{};
            const bool piece = v3 ? (slot == 1U || slot == 3U) : slot == 1U;
            if (v3) {
                overlay = water.Surface().ComposePixel(square * 4U + slot);
            } else if (piece) {
                const BoardSquareLeds surface{
                    water.Surface().ComposePixel(square * 4U),
                    water.Surface().ComposePixel(square * 4U + 1U),
                    water.Surface().ComposePixel(square * 4U + 2U),
                    water.Surface().ComposePixel(square * 4U + 3U)};
                overlay = RoundedPieceAverage(surface);
            } else {
                overlay = water.Surface().ComposePixel(
                    square * 4U + (slot == 0U ? 0U : 2U));
            }
            Rgb& target = physicalFrame[targetIndex];
            if (square == physicalRiderSquare && piece) {
            // Preserve the ordinary yellow piece pair above the surface; its
            // brightness follows local optical height, never a separate bob timer.
                target = AlphaBlendRgb(target, ScaleRgbSaturatingBy256(target,
                water.Surface().RiderScale256(physicalRiderSquare)), alpha);
            } else {
                target = AlphaBlendRgb(target, overlay, alpha);
            }
        }
    }
    for (uint8_t status = 0U; status < EDGE_LED_COUNT; ++status) {
        const Rgb overlay = water.Surface().ComposePixel(256U + status);
        if (topology.physicalStatusLedCount != 0U) {
            const uint8_t physicalStatus = revision == hardware::BoardRevision::V3_3S3
                ? hardware::V3_3PhysicalIndexForSemanticStatus(status)
                : status;
            Rgb& target = physicalFrame[topology.physicalBoardLedCount + physicalStatus];
            target = AlphaBlendRgb(target, overlay, alpha);
        } else {
            const uint16_t targetIndex = status_led_frame_adapter::
                VirtualLedIndexForPhysicalStatusIndex(status);
            physicalFrame[targetIndex] = status_led_frame_adapter::BlendVirtualStatus(
                physicalFrame[targetIndex], ScaleRgbSaturatingBy256(overlay,
                    (uint16_t(alpha) * 256U + 127U) / 255U));
        }
    }
}

} // namespace modules
