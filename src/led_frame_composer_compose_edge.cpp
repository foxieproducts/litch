#include "modules/led_frame_composer_internal.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/orientation.hpp"
#include "modules/control_led_thinking_feedback.hpp"
#include "modules/control_led_resignation_feedback.hpp"

#include "modules/edge_led_layer.hpp"
#include "modules/edge_led_renderer.hpp"
#include "modules/edge_led_hint_feedback.hpp"
#include "modules/edge_led_state_selection.hpp"
#include "modules/promotion_piece_cue.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "modules/turn_anim_style_setting.hpp"

namespace modules::led_frame_composer {
namespace internal {

bool ComposeEdgeLeds(const ComposeContext& ctx,
                     modules::BoardLedFrame& outLedFrame,
                     std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdgeFrame,
                     modules::status_led_frame_adapter::FixedPhysicalStatusOverlay*
                         outFixedPhysicalStatusOverlay) {
    const auto& in = *ctx.in;
    const auto& settings = ctx.settings;
    const auto& snapshot = ctx.Snapshot();

    modules::EdgeLedSelectionInputs edgeInputs;
    edgeInputs.inCheckmate =
        ctx.renderIntent.celebration == CelebrationIntent::Checkmate ||
        ctx.renderIntent.celebration == CelebrationIntent::ClockVictory ||
        ctx.renderIntent.celebration ==
            CelebrationIntent::VariantVictory;
    edgeInputs.inDraw =
        ctx.renderIntent.celebration == CelebrationIntent::Draw;
    const bool persistenceDegraded = settings.persistenceDegraded;
    edgeInputs.runtimeHealthPaused =
        runtime::IsRuntimeHealthPlayPaused(in.runtimeHealth);
    const bool runtimeHealthDegradedUsable =
        in.runtimeHealth.state != runtime::RuntimeHealthState::Healthy &&
        !edgeInputs.runtimeHealthPaused;
    const bool invalidBoardFault =
        ctx.freshSetupActive
            ? ctx.invalidBoard.extraMask != 0ULL
            : ctx.invalidBoard.IsActive();
    const bool calmInitialVariantValidation =
        snapshot.game.initialVariantValidationActive &&
        (ctx.humanIllegalMask | ctx.cpuIllegalMask) == 0ULL;
    edgeInputs.illegalActive = !snapshot.game.kingResignationArmed &&
        ctx.correctionFeedback == modules::CorrectionFeedback::Strong &&
        !calmInitialVariantValidation &&
        !ctx.undoPromptActive &&
        (invalidBoardFault ||
         (!ctx.freshSetupActive &&
          ((ctx.humanIllegalMask != 0ULL) ||
           (ctx.cpuIllegalMask != 0ULL))) ||
         ctx.freshSetupIllegalPickup);
    const bool freshSetupUsesTurnIndicator =
        !snapshot.interaction.kingQueenSwapPromptActive &&
        (ctx.freshSetupActive ||
         (snapshot.game.setupLifecycle ==
              runtime::SetupLifecycle::FreshSetup &&
          snapshot.board.physicalOccupiedMask ==
              snapshot.board.expectedOccupiedMask));
    // Foreground menu/text remains above the newly visible Fresh Setup Turn
    // indicator. Outside Fresh Setup, retain the existing menu fade base.
    edgeInputs.textOverlayActive =
        snapshot.render.textOverlayActive ||
        (freshSetupUsesTurnIndicator &&
         ctx.renderIntent.menuForegroundVisible &&
         !in.menuOverlayPreservesTurnIndicator);
    edgeInputs.placementModeActive =
        snapshot.interaction.kingQueenSwapPromptActive ||
        (!freshSetupUsesTurnIndicator &&
         snapshot.interaction.placementModeActive);
    const bool cpuThinkingEligible = snapshot.game.cpuThinking &&
        !ctx.invalidBoard.IsActive() &&
        snapshot.board.physicalOccupiedMask == snapshot.board.expectedOccupiedMask &&
        (ctx.humanIllegalMask | ctx.cpuIllegalMask) == 0ULL;
    const bool duckRelocationContinuesTurn =
        snapshot.game.variant == modules::GameVariant::Duck &&
        snapshot.game.duckTurnPhase ==
            modules::DuckTurnPhase::DuckRelocation &&
        snapshot.interaction.taggedState.plan.kind ==
            runtime::PhysicalTransitionKind::DuckRelocation;
    edgeInputs.interactionActive = !freshSetupUsesTurnIndicator &&
                                   !IsIdleInteraction(snapshot) &&
                                   !snapshot.interaction.undoPromptActive &&
                                   !IsCancelUndoTransitionActive(snapshot) &&
                                   !duckRelocationContinuesTurn;
    if (edgeInputs.inCheckmate) {
        const modules::SideToMove winnerSide =
            ctx.renderIntent.celebration == CelebrationIntent::ClockVictory
                ? (snapshot.game.clockWinner == modules::GameWinner::White
                       ? modules::SideToMove::White
                       : modules::SideToMove::Black)
                :
            ctx.renderIntent.celebration ==
                    CelebrationIntent::VariantVictory
                ? (snapshot.game.variantOutcome.winner ==
                           modules::GameWinner::White
                       ? modules::SideToMove::White
                       : modules::SideToMove::Black)
                : ((snapshot.game.sideToMove == modules::SideToMove::Black)
                       ? modules::SideToMove::White
                       : modules::SideToMove::Black);
        edgeInputs.activeSide = modules::led_frame_composer::OwnerForSideToMove(winnerSide, ctx.firstPlayerIsP2);
    } else {
        edgeInputs.activeSide = (ctx.edgeActiveOwner == modules::UiOwner::None)
                                    ? modules::led_frame_composer::OwnerForSideToMove(snapshot.game.sideToMove, ctx.firstPlayerIsP2)
                                    : ctx.edgeActiveOwner;
    }

    const modules::EdgeLedState edgeState = modules::SelectEdgeLedState(edgeInputs);

    modules::EdgeLedState baseEdgeState = edgeState;
    baseEdgeState.nowMs = ctx.nowMs;
    baseEdgeState.faultFlashElapsedMs = ctx.invalidBoard.elapsedMs;
    baseEdgeState.faultFlashPeriodMs = ctx.illegalPeriodMs;
    baseEdgeState.gameOverElapsedMs =
        ctx.renderIntent.celebrationElapsedMs;
    if (ctx.renderIntent.hintPromptVisible && snapshot.render.hintFeedbackTimingValid &&
        !ctx.invalidBoard.IsActive() && !snapshot.game.cpuThinking) {
        baseEdgeState.hintFeedbackOwner = ctx.activeOwner;
        baseEdgeState.hintFeedbackPending = snapshot.render.hintFeedbackPending;
        baseEdgeState.hintFeedbackHasMove = snapshot.render.hintMoveValid;
        baseEdgeState.hintFeedbackElapsedMs = snapshot.render.hintFeedbackElapsedMs;
    }
    baseEdgeState.beginnerModeP1Enabled = in.beginnerModeP1Enabled;
    baseEdgeState.beginnerModeP2Enabled = in.beginnerModeP2Enabled;
    baseEdgeState.beginnerModeStingerOwner = in.beginnerModeStingerOwner;
    baseEdgeState.beginnerModeStingerEnabling = in.beginnerModeStingerEnabling;
    baseEdgeState.beginnerModeStingerStartMs = in.beginnerModeStingerStartMs;

    baseEdgeState.turnAnimStyle = settings.turnStyle;
    baseEdgeState.minimalAnimations = ctx.minimalAnimations;

    // Menu edge overlay normally follows menu visual alpha (and menuActive implies alpha=255).
    // Hold progress may be shown on the edge without engaging the board menu
    // dim/overlay. Explicit system-hold progress remains visible during a
    // celebration so holding X can discover Quick Setup.
    const bool edgeOnlyHoldOverlay = !in.menuActive &&
                                     in.menuVisualAlpha255 == 0U &&
                                     in.menuIntent.menuActive &&
                                     in.menuIntent.menuHoldActive &&
                                     (in.menuIntent.menuOwner != modules::UiOwner::None);
    const bool holdMayReplaceCelebration =
        edgeOnlyHoldOverlay;

    const uint8_t menuAlpha = edgeOnlyHoldOverlay
                                  ? 255U
                                  : ((in.menuActive && in.menuVisualAlpha255 == 0U) ? 255U : in.menuVisualAlpha255);
    const bool menuVisible =
        (holdMayReplaceCelebration || !ctx.renderIntent.HasCelebration()) &&
        (!edgeInputs.runtimeHealthPaused ||
         in.higherPriorityStatusOverlayActive) &&
        (menuAlpha != 0U) && in.menuIntent.menuActive &&
        (in.menuIntent.menuOwner != modules::UiOwner::None);

    modules::EdgeLedState menuEdgeState = baseEdgeState;
    if (menuVisible) {
        menuEdgeState.mode = modules::EdgeUiMode::Menu;
        menuEdgeState.owner = in.menuIntent.menuOwner;
        // Input ownership includes player placement. Menu status is mapped
        // with literal board rotation only, so remove placement here to keep
        // the marker beside the physical bank that opened the menu.
        menuEdgeState.menuEdgeOwner = in.firstPlayerIsP2 && !in.litchcraftActive &&
                                             !edgeOnlyHoldOverlay
            ? modules::SwapP1P2Owner(in.menuIntent.menuOwner)
            : in.menuIntent.menuOwner;
        menuEdgeState.menuSelectionIndex = in.menuIntent.menuSelectionIndex;
        menuEdgeState.menuValue01_255 = in.menuIntent.menuValue01_255;
        menuEdgeState.menuValueIsHoldProgress = in.menuIntent.menuHoldActive;
        menuEdgeState.menuHoldFillAll = in.menuIntent.menuHoldFillAll;
    }

    modules::EdgeLedPalette palette;
    const bool isIdleTurn = (baseEdgeState.mode == modules::EdgeUiMode::IdleTurn);
    if (ctx.renderIntent.HasCelebration() || isIdleTurn) {
        palette.p1 = ctx.p1PieceRgb;
        palette.p2 = ctx.p2PieceRgb;
    } else {
        palette.p1 = ctx.p1AccentRgb;
        palette.p2 = ctx.p2AccentRgb;
    }
    const modules::EdgeLedRenderer edgeRenderer(palette);

    std::array<modules::Rgb, modules::EDGE_LED_COUNT> baseFrame;
    edgeRenderer.Render(baseEdgeState, baseFrame);
    // Fade only the ordinary Turn layer. Faults, recovery, text and menus retain
    // immediate priority; their colors and acknowledgement are never faded here.
    const auto interactionPhase = snapshot.interaction.taggedState.tag;
    const bool ordinaryPhase = IsIdleInteraction(snapshot) ||
        (interactionPhase == runtime::ChessInteractionPhase::PieceLifted &&
         !snapshot.render.cpuMoveSettlingActive);
    const bool ordinaryTurn = ordinaryPhase &&
        snapshot.game.duckTurnPhase != modules::DuckTurnPhase::DuckRelocation &&
        snapshot.game.setupLifecycle != runtime::SetupLifecycle::FreshSetup &&
        !freshSetupUsesTurnIndicator &&
        !duckRelocationContinuesTurn && !ctx.undoPromptActive &&
        !IsCancelUndoTransitionActive(snapshot) &&
        !edgeInputs.placementModeActive && !edgeInputs.textOverlayActive &&
        !edgeInputs.illegalActive && !edgeInputs.runtimeHealthPaused &&
        !ctx.renderIntent.HasCelebration();
    if (ordinaryTurn && snapshot.render.turnPresentationValid &&
        (baseEdgeState.mode == modules::EdgeUiMode::IdleTurn ||
         baseEdgeState.mode == modules::EdgeUiMode::Placement)) {
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> turnFrame{};
        modules::EdgeLedPalette turnPalette;
        turnPalette.p1 = ctx.p1PieceRgb;
        turnPalette.p2 = ctx.p2PieceRgb;
        const modules::EdgeLedRenderer turnRenderer(turnPalette);
        const uint16_t scale = snapshot.render.turnScale256;
        modules::EdgeLedState turn;
        turn.mode = modules::EdgeUiMode::IdleTurn;
        turn.activeSide = baseEdgeState.activeSide;
        turn.activePalette = baseEdgeState.activePalette;
        turn.animationTimeExplicit = true;
        // Preserve legacy spatial phase through recovery; Away owns its episode.
        turn.nowMs = baseEdgeState.turnAnimStyle == modules::TurnAnimStyle::AwayFromActiveEnd
            ? snapshot.render.turnElapsedMs : ctx.nowMs;
        turn.turnAnimStyle = baseEdgeState.turnAnimStyle;
        turn.minimalAnimations = ctx.minimalAnimations;
        turnRenderer.Render(turn, turnFrame);
        for (auto& pixel : turnFrame) {
            pixel = modules::ScaleRgbSaturatingBy256(pixel, scale);
        }
        if (baseEdgeState.mode == modules::EdgeUiMode::IdleTurn && scale < 256U) {
            // Crossfade the existing spatial owner cue into the selected motion.
            // This carries authority through waveform troughs and quantization.
            auto carrier = turn;
            carrier.minimalAnimations = true;
            std::array<modules::Rgb, modules::EDGE_LED_COUNT> carrierFrame{};
            turnRenderer.Render(carrier, carrierFrame);
            for (size_t i = 0U; i < turnFrame.size(); ++i) {
                turnFrame[i] = modules::AddRgbSaturating(turnFrame[i],
                    modules::ScaleRgbSaturatingBy256(carrierFrame[i], 256U - scale));
            }
        }
        // Compose badges/stingers once. Required Hint acknowledgement owns its
        // corners after the ordinary Turn layer, as in the existing renderer.
        auto decoration = baseEdgeState;
        decoration.mode = modules::EdgeUiMode::Placement;
        decoration.hintFeedbackOwner = modules::UiOwner::None;
        edgeRenderer.Render(decoration, baseFrame);
        for (size_t i = 0U; i < baseFrame.size(); ++i) {
            baseFrame[i] = modules::AddRgbSaturating(baseFrame[i], turnFrame[i]);
        }
        modules::ApplyHintCornerFeedback(baseEdgeState, baseFrame);
    }
    outEdgeFrame = baseFrame;

    if (menuVisible) {
        modules::EdgeLedLayer menuLayer;
        menuLayer.mode = modules::EdgeLedLayerMode::Replace;
        menuLayer.alpha255 = menuAlpha;
        edgeRenderer.Render(menuEdgeState, menuLayer.pixels);
        modules::ApplyEdgeLedLayer(menuLayer, outEdgeFrame);
    }

    // Usable degradation owns only the two physical USB-side pixels. Apply it
    // after menu/Identify composition so those warnings remain visible without
    // hiding the rest of the foreground. Higher recovery-class status retains
    // its explicit priority above health.
    const bool healthMayOwnUsb =
        (persistenceDegraded ||
         (runtimeHealthDegradedUsable &&
          (!ctx.freshSetupActive || in.litchcraftActive))) &&
        !edgeInputs.runtimeHealthPaused &&
        !in.higherPriorityStatusOverlayActive &&
        (baseEdgeState.mode == modules::EdgeUiMode::IdleTurn || menuVisible ||
         in.menuActive || in.litchcraftActive);
    if (healthMayOwnUsb) {
        modules::EdgeLedState healthState = baseEdgeState;
        healthState.mode = modules::EdgeUiMode::HealthDegradedUsable;
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> healthFrame{};
        edgeRenderer.Render(healthState, healthFrame);
        for (const uint8_t physicalIndex :
             modules::status_led_frame_adapter::USB_PORT_PHYSICAL_INDICES) {
            if (outFixedPhysicalStatusOverlay != nullptr) {
                outFixedPhysicalStatusOverlay->Set(
                    physicalIndex, healthFrame[physicalIndex]);
            } else {
                // Semantic-only compatibility callers cannot apply a physical
                // overlay. Preserve their historical warning observation.
                outEdgeFrame[physicalIndex] = healthFrame[physicalIndex];
            }
        }
    }

    const bool unavailableOverMenu =
        menuVisible &&
        (in.edgeCueIntent.kind == modules::EdgeLedCueKind::Unavailable ||
         in.edgeCueIntent.kind ==
             modules::EdgeLedCueKind::OccupiedBoardBlocked);
    const bool ordinaryCueSurface =
        !menuVisible &&
        (baseEdgeState.mode == modules::EdgeUiMode::IdleTurn ||
         (in.edgeCueIntent.kind ==
              modules::EdgeLedCueKind::OccupiedBoardBlocked &&
          baseEdgeState.mode == modules::EdgeUiMode::Placement));
    // A new Hint request is the next user action after setup. Its immediate
    // acknowledgement supersedes the earlier setup-complete sweep; menus,
    // correction and required physical actions still own their usual surfaces.
    const bool hintSupersedesSetupAcknowledgement =
        in.edgeCueIntent.kind == modules::EdgeLedCueKind::SetupComplete &&
        baseEdgeState.hintFeedbackOwner != modules::UiOwner::None;
    if (in.edgeCueIntent.IsActive() &&
        (unavailableOverMenu || ordinaryCueSurface) &&
        !hintSupersedesSetupAcknowledgement) {
        modules::EdgeLedState cueState = baseEdgeState;
        cueState.mode =
            in.edgeCueIntent.kind == modules::EdgeLedCueKind::SetupComplete
                ? modules::EdgeUiMode::SetupCompleteSweep
            : in.edgeCueIntent.kind ==
                      modules::EdgeLedCueKind::OccupiedBoardBlocked
                ? modules::EdgeUiMode::OccupiedBoardWarning
                : modules::EdgeUiMode::UnavailableNudge;
        cueState.activeSide =
            in.edgeCueIntent.owner == modules::UiOwner::None
                ? baseEdgeState.activeSide
                : in.edgeCueIntent.owner;
        cueState.effectStartMs = in.edgeCueIntent.startMs;
        modules::EdgeLedLayer cueLayer;
        cueLayer.mode =
            (in.edgeCueIntent.kind == modules::EdgeLedCueKind::SetupComplete ||
             in.edgeCueIntent.kind ==
                 modules::EdgeLedCueKind::OccupiedBoardBlocked)
                ? modules::EdgeLedLayerMode::Replace
                : modules::EdgeLedLayerMode::Overlay;
        if (in.edgeCueIntent.kind == modules::EdgeLedCueKind::SetupComplete) {
            const uint32_t elapsedMs = static_cast<uint32_t>(
                ctx.nowMs - in.edgeCueIntent.startMs);
            cueLayer.alpha255 =
                modules::EdgeLedCueController::SetupCompleteLayerAlpha255(
                    elapsedMs);
        }
        edgeRenderer.Render(cueState, cueLayer.pixels);
        modules::ApplyEdgeLedLayer(cueLayer, outEdgeFrame);
    }

    // Promotion selection borrows four side-edge pixels only while the
    // ordinary placement surface owns the edge. Human selection alone gets
    // these controls; correction, menu, pausing health, and fatal states keep
    // complete priority. Usable degradation remains supplemental and does not
    // turn the selector into a fatal state.
    const bool promotionBaseIsOtherwiseDark =
        baseEdgeState.mode == modules::EdgeUiMode::Placement ||
        (baseEdgeState.mode == modules::EdgeUiMode::Menu &&
         baseEdgeState.owner == modules::UiOwner::None);
    const bool humanPromotionMayGuide =
        IsHumanPromotionActive(snapshot.interaction) &&
        !ctx.invalidBoard.IsActive() &&
        ctx.renderIntent.centerTextOverlayVisible && !menuVisible &&
        promotionBaseIsOtherwiseDark &&
        snapshot.board.physicalOccupiedMask ==
            snapshot.interaction.taggedState.plan.afterOccupiedMask &&
        !ctx.renderIntent.HasCelebration() && !snapshot.render.textOverlayActive &&
        !edgeInputs.runtimeHealthPaused && !in.higherPriorityStatusOverlayActive &&
        (ctx.humanIllegalMask | ctx.cpuIllegalMask) == 0ULL &&
        !snapshot.system.safeModeActive && !snapshot.system.hardwareTestActive &&
        !snapshot.system.amptestActive && !snapshot.system.otaInProgress &&
        !in.litchcraftActive;
    if (humanPromotionMayGuide) {
        modules::promotion_piece_cue::ApplyHumanLeftRightGuidance(
            outEdgeFrame, ctx.promoMoverPieceRgb,
            snapshot.interaction.promotionElapsedMs);
        if (outFixedPhysicalStatusOverlay != nullptr) {
            modules::promotion_piece_cue::ComposeHumanControls(
                *outFixedPhysicalStatusOverlay, ctx.promoMoverPieceRgb,
                snapshot.interaction.promotionElapsedMs);
        }
    }

    (void)outLedFrame;
    const bool controlSurfaceAvailable = !ctx.renderIntent.HasCelebration() &&
        !ctx.renderIntent.menuForegroundVisible && !in.menuActive && !menuVisible &&
        !ctx.renderIntent.centerTextOverlayVisible && !snapshot.render.textOverlayActive &&
        !edgeInputs.runtimeHealthPaused && !in.higherPriorityStatusOverlayActive &&
        !ctx.invalidBoard.IsActive() &&
        snapshot.board.physicalOccupiedMask == snapshot.board.expectedOccupiedMask &&
        (ctx.humanIllegalMask | ctx.cpuIllegalMask) == 0ULL &&
        !edgeInputs.placementModeActive && !snapshot.system.safeModeActive &&
        !snapshot.system.hardwareTestActive && !snapshot.system.amptestActive &&
        !snapshot.system.otaInProgress && !in.litchcraftActive;
    const bool resignationSurfaceAvailable =
        snapshot.game.kingResignationOwner != UiOwner::None &&
        (snapshot.game.kingResignationArmed || !snapshot.render.cpuMoveSettlingActive) &&
        !ctx.renderIntent.HasCelebration() && !ctx.renderIntent.menuForegroundVisible &&
        !in.menuActive && !menuVisible && !ctx.renderIntent.centerTextOverlayVisible &&
        !snapshot.render.textOverlayActive && !edgeInputs.runtimeHealthPaused &&
        !in.higherPriorityStatusOverlayActive && !snapshot.system.safeModeActive &&
        !snapshot.system.hardwareTestActive && !snapshot.system.amptestActive &&
        !snapshot.system.otaInProgress && !in.litchcraftActive;
    if (outFixedPhysicalStatusOverlay != nullptr && resignationSurfaceAvailable) {
        const auto owner = snapshot.game.kingResignationOwner;
        ComposeControlLedResignationFeedback(owner, snapshot.game.kingResignationElapsedMs,
            owner == UiOwner::P2 ? ctx.p2PieceRgb : ctx.p1PieceRgb,
            *outFixedPhysicalStatusOverlay);
        return false;
    }
    const auto thinkingStyle = settings.thinkingStyle;
    const bool thinkingStyleEnabled = thinkingStyle !=
        hint_thinking_style_setting::Style::None;
    const bool cauldron = thinkingStyle == hint_thinking_style_setting::Style::CauldronBrewing;
    const bool cpuPresentation = snapshot.game.cpuThinking || snapshot.render.thinkingControlCpu;
    const bool manualBrew = cauldron && !cpuPresentation;
    const UiOwner thinkingOwner = manualBrew ? snapshot.render.hintBrewingOwner
        : snapshot.render.thinkingControlOwner;
    const uint16_t thinkingScale = manualBrew ? snapshot.render.hintBrewingScale256
        : snapshot.render.thinkingControlScale256;
    const uint32_t thinkingElapsed = manualBrew ? snapshot.render.hintBrewingElapsedMs
        : snapshot.render.thinkingControlElapsedMs;
    if (outFixedPhysicalStatusOverlay != nullptr && controlSurfaceAvailable) {
        auto& output = *outFixedPhysicalStatusOverlay;
        const UiOwner owner = thinkingOwner;
        const uint8_t ownerBit = owner == UiOwner::P2 ? 2U : 1U;
        if (thinkingScale != 0U && owner != UiOwner::None &&
            (in.paletteEditOwners & ownerBit) == 0U) {
            if (cauldron && cpuPresentation) {
                // Older V3 has no physical O. Preserve its existing CPU cue
                // in the emulated bank; fitted controls receive Cauldron below.
                status_led_frame_adapter::FixedPhysicalStatusOverlay legacy;
                ComposeControlLedThinkingFeedback({owner, thinkingElapsed,
                    hint_thinking_style_setting::Style::Beachball,
                    owner == UiOwner::P2 ? ctx.p2PieceRgb : ctx.p1PieceRgb,
                    ctx.minimalAnimations, thinkingScale}, legacy);
                output.emulatedControlMask = legacy.emulatedControlMask;
                output.emulatedControlPixels = legacy.emulatedControlPixels;
            }
            ComposeControlLedThinkingFeedback({owner, thinkingElapsed, thinkingStyle,
                owner == UiOwner::P2 ? ctx.p2PieceRgb : ctx.p1PieceRgb,
                ctx.minimalAnimations, thinkingScale}, output);
        }
        if ((in.paletteEditOwners & 1U) != 0U)
            ComposeControlLedColorPreview(UiOwner::P1, ctx.p1PieceRgb, output);
        if ((in.paletteEditOwners & 2U) != 0U)
            ComposeControlLedColorPreview(UiOwner::P2, ctx.p2PieceRgb, output);

        const UiOwner cpuOwner = settings.cpuOwner;
        const uint8_t cpuBank = cpuOwner == UiOwner::P2 ? 6U : 0U;
        const uint16_t cpuBankMask = static_cast<uint16_t>(0x3FU << cpuBank);
        if (cpuOwner != UiOwner::None && IsIdleInteraction(snapshot) &&
            !IsSettlingActive(snapshot) && !ctx.undoPromptActive &&
            !snapshot.game.initialVariantValidationActive &&
            !snapshot.render.bootFadeActive && !snapshot.render.startupPresentationActive &&
            !in.neutralStartupGrid &&
            !in.menuIntent.menuHoldActive &&
            (output.controlMask & cpuBankMask) == 0U) {
            // A seat marker, independent of turn and search lifetime. Yield the
            // whole bank to thinking/preview, including its bank-wide fade.
            constexpr animation::ValueToken CPU_BEACON{
                {4096U, animation::PlaybackMode::Loop, 0U},
                {animation::Waveform::Breathe, 48U, 128U},
            };
            const uint16_t scale = ctx.minimalAnimations ? 80U
                : animation::SampleValue({ctx.nowMs}, CPU_BEACON);
            const auto color = ScaleRgbSaturatingBy256(
                cpuOwner == UiOwner::P2 ? ctx.p2PieceRgb : ctx.p1PieceRgb, scale);
            for (const auto role : {hardware::ControlLedRole::Up, hardware::ControlLedRole::Down}) {
                const auto index = static_cast<uint8_t>(cpuBank + static_cast<uint8_t>(role));
                output.controlPixels[index] = color;
                output.controlMask |= static_cast<uint16_t>(1U << index);
            }
        }
    }
    if (outFixedPhysicalStatusOverlay != nullptr) {
        outFixedPhysicalStatusOverlay->ordinaryStatus = isIdleTurn &&
            !menuVisible && !healthMayOwnUsb && !in.higherPriorityStatusOverlayActive &&
            !in.edgeCueIntent.IsActive() &&
            baseEdgeState.hintFeedbackOwner == modules::UiOwner::None &&
            outFixedPhysicalStatusOverlay->mask == 0U;
    }
    return thinkingStyleEnabled && cpuThinkingEligible && controlSurfaceAvailable &&
        (in.paletteEditOwners & (snapshot.game.sideToMove == SideToMove::Black ? 2U : 1U)) == 0U;
}

void ApplyGlobalBrightnessBoost(modules::BoardLedFrame& outLedFrame) {
    for (auto& square : outLedFrame) {
        square.left = ScaleRgbSaturating(square.left, led_render_policy::COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256);
        square.top = ScaleRgbSaturating(square.top, led_render_policy::COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256);
        square.right = ScaleRgbSaturating(square.right, led_render_policy::COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256);
        square.bottom = ScaleRgbSaturating(square.bottom, led_render_policy::COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256);
    }
}

void ApplyGlobalBrightnessBoost(modules::GridForegroundLayer& layer) {
    layer.brightnessScale256 = led_render_policy::COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256;
}

void ApplyGlobalBrightnessBoost(modules::BoardLedFrame& outLedFrame,
                                std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdgeFrame,
                                std::array<modules::Rgb, 64>& outGridFrame) {
    ApplyGlobalBrightnessBoost(outLedFrame);
    for (size_t i = 0; i < outEdgeFrame.size(); ++i) {
        outEdgeFrame[i] = ScaleRgbSaturating(outEdgeFrame[i], led_render_policy::COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256);
    }
    for (size_t i = 0; i < outGridFrame.size(); ++i) {
        outGridFrame[i] = ScaleRgbSaturating(outGridFrame[i], led_render_policy::COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256);
    }
}

} // namespace internal
} // namespace modules::led_frame_composer
