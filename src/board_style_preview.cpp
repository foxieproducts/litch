#include "modules/board_style_preview.hpp"

#include <cstring>

#include "modules/led_frame_composer_internal.hpp"

namespace modules {

void BoardStylePreview::Cancel() {
    m_frame = {};
    m_anchorPending = false;
}

void BoardStylePreview::SettingAccepted(const SettingsStore& settings,
                                      const char* const key) {
    if (key == nullptr) return;
    if (std::strcmp(key, settle_animation_setting::KEY) == 0) {
        Cancel();
        m_frame.placementStyle = settle_animation_setting::Read(settings);
        if (m_frame.placementStyle == SettleAnimationStyle::None) return;
        m_frame.kind = BoardStylePreviewKind::Placement;
    } else if (std::strcmp(key, checkmate_animation_setting::KEY) == 0) {
        Cancel();
        m_frame.kind = BoardStylePreviewKind::Checkmate;
        m_frame.checkmateStyle = checkmate_animation_setting::Read(settings);
    } else {
        return;
    }
    m_elapsed.Reset();
    m_anchorPending = true;
}

BoardStylePreviewFrame BoardStylePreview::Capture(
    const led_frame_composer::ComposeInputs& inputs) {
    if (m_frame.kind == BoardStylePreviewKind::None) return {};
    led_frame_composer::internal::ComposeContext ctx;
    if (!led_frame_composer::internal::BuildComposeContext(inputs, ctx)) {
        Cancel();
        return {};
    }
    const auto& snapshot = ctx.Snapshot();
    const uint32_t elapsedMs = m_elapsed.ElapsedMs();
    const uint32_t durationMs = BoardStylePreviewDurationMs(m_frame, ctx.minimalAnimations);
    // Required next actions and physical handling retire the preview, rather
    // than pausing it and replaying it after that owner releases the board.
    if (elapsedMs >= durationMs || !runtime::IsIdleInteraction(snapshot) ||
        snapshot.interaction.lastInteractionElapsedMs < elapsedMs ||
        snapshot.game.setupLifecycle != runtime::SetupLifecycle::Gameplay ||
        snapshot.game.kingResignationArmed || snapshot.render.textOverlayActive ||
        ctx.allPiecesFlashActive ||
        led_frame_composer::internal::PhysicalRemovalSquareMask(ctx) != 0ULL ||
        ctx.invalidBoard.IsActive() ||
        ctx.initialKingPlacementPromptActive || ctx.kingQueenSwapPromptActive ||
        ctx.undoPromptActive || inputs.menuActive || inputs.menuVisualAlpha255 != 0U ||
        inputs.paletteEditOwners != 0U || inputs.litchcraftActive ||
        inputs.neutralStartupGrid || inputs.higherPriorityStatusOverlayActive ||
        snapshot.system.safeModeActive || snapshot.system.hardwareTestActive ||
        snapshot.system.amptestActive || snapshot.system.otaInProgress ||
        runtime::IsRuntimeHealthPlayPaused(inputs.runtimeHealth)) {
        Cancel();
        return {};
    }
    if (m_anchorPending) {
        uint8_t anchor = 255U;
        if (m_frame.kind == BoardStylePreviewKind::Placement) {
            if (ctx.renderIntent.hintPromptVisible && ctx.hintMoveValid) {
                anchor = ctx.hintFromSquare;
            } else if (snapshot.game.sideToMove == SideToMove::White && snapshot.render.whiteLastMoveValid) {
                anchor = snapshot.render.whiteLastMoveToSquare;
            } else if (snapshot.game.sideToMove == SideToMove::Black && snapshot.render.blackLastMoveValid) {
                anchor = snapshot.render.blackLastMoveToSquare;
            }
        } else {
            const char king = snapshot.game.sideToMove == SideToMove::White ? 'k' : 'K';
            for (uint8_t square = 0U; square < 64U; ++square) {
                if (snapshot.board.enginePieces[square] == king) { anchor = square; break; }
            }
        }
        if (!snapshot.board.enginePiecesValid || anchor >= 64U ||
            (ctx.physicalOcc64 & (1ULL << anchor)) == 0ULL ||
            snapshot.board.enginePieces[anchor] == '\0' || snapshot.board.enginePieces[anchor] == '.') {
            Cancel();
            return {};
        }
        m_frame.anchorSquare = anchor;
        const char piece = snapshot.board.enginePieces[anchor];
        const bool pieceWhite = piece >= 'A' && piece <= 'Z';
        if (m_frame.kind == BoardStylePreviewKind::Placement &&
            pieceWhite != (snapshot.game.sideToMove == SideToMove::White)) {
            Cancel();
            return {};
        }
        m_frame.white = m_frame.kind == BoardStylePreviewKind::Placement
            ? piece >= 'A' && piece <= 'Z' : snapshot.game.sideToMove == SideToMove::White;
        m_anchorPending = false;
    }
    m_frame.elapsedMs = elapsedMs;
    return m_frame;
}

} // namespace modules
