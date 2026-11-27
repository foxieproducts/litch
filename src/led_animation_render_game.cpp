#include "firmware/led_animation_renderer.hpp"

#include <algorithm>

#include "foxie_embedded/millis.hpp"
#include "modules/game_reset_hold_policy.hpp"
#include "modules/orientation.hpp"
#include "modules/quick_settings_items.hpp"
#include "modules/water_slosh_presentation.hpp"
#include "modules/litchcraft_session.hpp"
#include "modules/white_squares_setting.hpp"

namespace firmware {

void LedAnimationRenderer::RenderGame(LedAnimationPresentation& presentation,
    GameLedPresentation& game, LedAnimationFrameResult& result) {
    if (&game != &m_gameBackground) {
        m_gameBackground = game;
        m_haveGameBackground = true;
    }
    auto& ownership = m_frameOwnership;
    ownership.foreground.Clear();
    ownership.status = {};
    ownership.prioritySquares = 0U;
    ownership.ordinaryPieces = 0U;
    ownership.captureRemoval = 0U;
    ownership.captureTaps = 0U;
    ownership.duckReminder = 0U;
    ownership.menuOwner = modules::UiOwner::None;
    ownership.menuActive = false;
    ComposeGame(presentation, game, result, ownership);
    OutputGameFrame(presentation, ownership, result);
}

void LedAnimationRenderer::ComposeGame(LedAnimationPresentation& presentation,
    GameLedPresentation& game, LedAnimationFrameResult& result, FrameOwnership& ownership) {
    {
        auto snapshot = runtime::SampleInteractionModelPresentation(
            game.interaction, result.receipt.frameStartedMs);
        m_identify.Apply(presentation.identify);
        const bool identify = m_identify.Update(result.receipt.frameStartedMs,
            presentation.quickSettingsActive, m_identifyMenu, result.menuBackground);
        const bool controlSwap = modules::ComputeControlBankSwap(presentation.boardRotate,
            presentation.firstPlayerIsP2, presentation.output.canvasVisible);
        const bool pairingAllowed = presentation.pairingAllowed && !identify;
        const bool restart = modules::BoardOwnerPairingRuntime::SyncPhysicalMessage(
            presentation.pairing, m_restartMessageVisible, presentation.menu,
            result.menuBackground, controlSwap, pairingAllowed);
        auto& menu = identify ? m_identifyMenu : presentation.menu;
        ownership.menuOwner = menu.GetOwner();
        snapshot.board.physicalOccupiedMask = presentation.physicalOccupiedMask;
        snapshot.render.menuOverlayActive = menu.IsActive();
        snapshot.render.quickSettingsOverlayActive = presentation.quickSettingsActive;
        snapshot.render.allPiecesFlashActive = presentation.allPiecesFlash.IsActive(
            result.receipt.frameStartedMs);
        snapshot.render.allPiecesFlashOn = presentation.allPiecesFlash.IsOn(
            result.receipt.frameStartedMs);
        const uint32_t elapsed = result.receipt.frameStartedMs - presentation.capturedAtMs;
        modules::led_frame_composer::ComposeInputs inputs;
        inputs.resolvedSettings = &presentation.settings;
        inputs.interactionSnapshot = &snapshot;
        inputs.runtimeHealth = presentation.health;
        inputs.firstPlayerIsP2 = presentation.firstPlayerIsP2;
        inputs.litchcraftActive = presentation.output.canvasVisible;
        inputs.menuOverlay = &menu;
        inputs.menuActive = presentation.menuControllerActive || presentation.quickSettingsActive ||
            snapshot.render.menuOverlayActive;
        ownership.menuActive = inputs.menuActive;
        inputs.menuOverlayPreservesTurnIndicator = menu.PreservesTurnIndicator();
        inputs.menuIntent = presentation.menuIntent;
        if (inputs.menuIntent.menuHoldActive && presentation.menuHoldThresholdMs != 0U) {
            const uint32_t original = uint32_t(inputs.menuIntent.menuValue01_255) *
                presentation.menuHoldThresholdMs / 255U;
            const uint32_t held = original + elapsed;
            inputs.menuIntent.menuValue01_255 = held >= presentation.menuHoldThresholdMs
                ? 255U : static_cast<uint8_t>(held * 255U / presentation.menuHoldThresholdMs);
        }
        if (presentation.resetHold.active) {
            modules::ProjectGameResetHold(inputs.menuIntent, presentation.resetHold.owner,
                result.receipt.frameStartedMs - presentation.resetHold.startedAtMs,
                presentation.resetHold.triggered, presentation.resetHold.rejected);
        }
        if (identify || restart) {
            inputs.menuVisualAlpha255 = 255U;
            inputs.menuBackgroundDimScale256 = identify ? 0U : 26U;
        }
        if (presentation.quickSettingsActive) {
            inputs.menuVisualAlpha255 = 255U;
            inputs.menuBackgroundDimScale256 =
                presentation.menuSelection == modules::quick_settings_items::WHITE_SQUARES ? 256U :
                presentation.menuSelection == modules::quick_settings_items::BRIGHTNESS ? 102U : 26U;
        }
        inputs.edgeCueIntent = presentation.edgeCue;
        inputs.paletteEditOwners = presentation.paletteEditOwners;
        inputs.beginnerModeP1Enabled = presentation.beginnerModeP1;
        inputs.beginnerModeP2Enabled = presentation.beginnerModeP2;
        inputs.beginnerModeStingerOwner = presentation.beginnerStingerOwner;
        inputs.beginnerModeStingerEnabling = presentation.beginnerStingerEnabling;
        inputs.beginnerModeStingerStartMs = presentation.beginnerStingerStartedMs;
        inputs.boardStylePreview = game.stylePreview;
        if (inputs.boardStylePreview.kind != modules::BoardStylePreviewKind::None) {
            inputs.boardStylePreview.elapsedMs += elapsed;
            if (inputs.boardStylePreview.elapsedMs >= modules::BoardStylePreviewDurationMs(
                    inputs.boardStylePreview, presentation.settings.minimalAnimations))
                inputs.boardStylePreview = {};
        }
        modules::led_frame_composer::HintPresentationOutcome hint;
        modules::led_frame_composer::Compose(inputs, m_board, m_status,
            result.grid, &result.menuBackground, nullptr, &ownership.prioritySquares,
            &ownership.ordinaryPieces, &ownership.status, &ownership.foreground,
            &ownership.captureRemoval, &ownership.duckReminder, &hint, &ownership.captureTaps);
        result.receipt.visibleHint = hint.tuple;
        result.receipt.visibleHintValid = hint.visible;
        result.retainPriorHint = hint.retainPrior;
        if (auto* artwork = std::get_if<modules::litchcraft_presentation::Inputs>(&presentation.scene);
            artwork != nullptr && artwork->active &&
            modules::LitchcraftCanvasOwnsFrame(true,
                runtime::IsRuntimeHealthPlayPaused(presentation.health), identify || restart)) {
            const auto sharedStatus = m_status;
            result.receipt.artwork = m_artwork.Compose(*artwork, m_artworkPort, m_board, m_status);
            modules::led_frame_composer::RestoreLitchcraftStatusWarnings(inputs, sharedStatus, m_status);
            modules::led_frame_composer::ApplyLitchcraftOccupiedBoardWarning(
                inputs, presentation.physicalOccupiedMask, m_board);
            ownership.foreground.Clear();
            ownership.prioritySquares = 0U;
            ownership.ordinaryPieces = 0U;
            ownership.captureRemoval = 0U;
            ownership.captureTaps = 0U;
            ownership.duckReminder = 0U;
            result.receipt.visibleHintValid = false;
            result.retainPriorHint = false;
        }
        auto controls = presentation.controlInputs;
        for (auto& bank : controls.holds)
            for (auto& hold : bank) if (hold.active) hold.elapsedMs += elapsed;
        m_controlFeedback.ApplyEvents(presentation.controlEvents);
        if (identify || restart) controls.scene = modules::ControlFeedbackScene::Suppressed;
        m_controlFeedback.Compose(controls, ownership.status);
        if (pairingAllowed) modules::BoardOwnerPairingRuntime::ComposePhysicalCue(
            presentation.pairing, ownership.status, controlSwap);
        if (presentation.startupPieceAccent && !m_startupAccentStarted) {
            m_startupAccent.Begin();
            m_startupAccentStarted = true;
        }
        const bool ambientReady = !m_profile->hasLightSensors ||
            !presentation.output.autoBrightness || presentation.output.ambientUsable;
        m_startupPieceScale256 = m_startupAccent.Update(presentation.startupBoardObserved ||
            presentation.boardObservationSequence != 0U,
            snapshot.board.physicalOccupiedMask, ambientReady, ownership.ordinaryPieces != 0U,
            !m_boot.IsComplete() || presentation.initialStoreModePending);
        if (presentation.quickSettingsActive &&
            presentation.menuSelection == modules::quick_settings_items::WHITE_SQUARES) {
            const auto glow = modules::white_squares_setting::PreviewGlow(
                result.receipt.frameStartedMs);
            for (uint8_t square = 0U; square < 64U; ++square) {
                if ((snapshot.board.physicalOccupiedMask & (UINT64_C(1) << square)) != 0U)
                    continue;
                m_board[square] = {};
                if (presentation.settings.whiteSquaresOn &&
                    ((square / 8U + square % 8U) & 1U) == 0U) {
                    m_board[square].left = glow;
                    m_board[square].right = glow;
                }
            }
            const bool squareRotate = modules::ComputeSquareRotate(presentation.boardRotate,
                presentation.firstPlayerIsP2, presentation.output.canvasVisible);
            const bool controlRotate = modules::ComputeControlBankSwap(presentation.boardRotate,
                presentation.firstPlayerIsP2, presentation.output.canvasVisible) !=
                (ownership.menuOwner == modules::UiOwner::P2);
            hardware::ClipGridForegroundToOccupiedPreview(snapshot.board.physicalOccupiedMask,
                squareRotate, controlRotate, ownership.foreground);
        }
        if (!presentation.output.canvasVisible && presentation.entryHold.active &&
            !runtime::IsRuntimeHealthPlayPaused(presentation.health))
            modules::litchcraft_presentation::ComposeEntryStatus(m_profile->isS3 ? 2U : 1U,
                result.receipt.frameStartedMs - presentation.entryHold.startedAtMs, m_status);
    }
}

} // namespace firmware

namespace firmware {
void LedAnimationRenderer::RenderArtwork(LedAnimationPresentation& presentation,
    modules::litchcraft_presentation::Inputs& artwork, LedAnimationFrameResult& result) {
    if (!m_haveGameBackground || !ArtworkCurrent(artwork)) return;
    m_artworkSceneActive = true;
    RenderGame(presentation, m_gameBackground, result);
}
} // namespace firmware
