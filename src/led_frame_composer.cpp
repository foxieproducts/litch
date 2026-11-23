#include "modules/led_frame_composer_internal.hpp"

#include "modules/status_led_frame_adapter.hpp"

namespace modules::led_frame_composer {

void ComposeStartupGrid(
    const fxe::SettingsStore& settings,
    modules::BoardLedFrame& outLedFrame) {
    runtime::InteractionModelSnapshot snapshot{};
    snapshot.render.bootFadeActive = true;
    ComposeInputs inputs;
    inputs.settings = &settings;
    inputs.interactionSnapshot = &snapshot;
    inputs.neutralStartupGrid = true;
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edge{};
    std::array<modules::Rgb, 64> grid{};
    Compose(inputs, outLedFrame, edge, grid);

}

void ApplyLitchcraftOccupiedBoardWarning(const ComposeInputs& in,
                                        const uint64_t occupancy,
                                        modules::BoardLedFrame& board) {
    if (in.interactionSnapshot == nullptr ||
        !in.interactionSnapshot->render.allPiecesFlashActive ||
        in.interactionSnapshot->render.allPiecesFlashOn) return;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((occupancy & (1ULL << square)) != 0ULL) {
            board[square] = modules::BoardSquareLeds{};
        }
    }
}

void Compose(const ComposeInputs& in,
             modules::BoardLedFrame& outLedFrame,
             std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdgeFrame,
             std::array<modules::Rgb, 64>& outGridFrame) {
    Compose(in, outLedFrame, outEdgeFrame, outGridFrame, nullptr);
}

void RestoreLitchcraftStatusWarnings(
    const ComposeInputs& in,
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& sharedStatusFrame,
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>& litchcraftStatusFrame) {
    const bool activeWarningCue =
        in.edgeCueIntent.kind == modules::EdgeLedCueKind::Unavailable ||
        in.edgeCueIntent.kind == modules::EdgeLedCueKind::OccupiedBoardBlocked;
    if (activeWarningCue) {
        litchcraftStatusFrame = sharedStatusFrame;
        return;
    }

    const bool persistenceDegraded = in.resolvedSettings != nullptr
        ? in.resolvedSettings->persistenceDegraded
        : in.settings != nullptr && in.settings->HasConfirmedStorageFailure();
    const bool runtimeDegradedUsable =
        in.runtimeHealth.state == runtime::RuntimeHealthState::DegradedUsable;
    if (!persistenceDegraded && !runtimeDegradedUsable) return;

    for (const uint8_t physicalIndex :
         modules::status_led_frame_adapter::USB_PORT_PHYSICAL_INDICES) {
        const uint8_t semanticIndex = physicalIndex;
        litchcraftStatusFrame[semanticIndex] =
            sharedStatusFrame[semanticIndex];
    }
}

void Compose(const ComposeInputs& in,
             modules::BoardLedFrame& outLedFrame,
             std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdgeFrame,
             std::array<modules::Rgb, 64>& outGridFrame,
             std::array<modules::Rgb, 64>* outGridFrameBeforeMenuOverlay,
             bool* outCpuThinkingVisible,
             uint64_t* outSidePrioritySquareMask,
             uint64_t* outOrdinaryPieceMask,
             modules::status_led_frame_adapter::FixedPhysicalStatusOverlay*
                 outFixedPhysicalStatusOverlay,
             modules::GridForegroundLayer* outMenuForeground,
             uint64_t* outCaptureRemovalMask,
             uint64_t* outDuckReminderMask,
             HintPresentationOutcome* outHint,
             uint64_t* outCaptureTapMask) {
    if (outHint != nullptr) *outHint = {};
    if (outOrdinaryPieceMask != nullptr) { *outOrdinaryPieceMask = 0ULL; }
    if (outCaptureRemovalMask != nullptr) { *outCaptureRemovalMask = 0ULL; }
    if (outCaptureTapMask != nullptr) { *outCaptureTapMask = 0ULL; }
    if (outDuckReminderMask != nullptr) { *outDuckReminderMask = 0ULL; }
    if (outSidePrioritySquareMask != nullptr) {
        *outSidePrioritySquareMask = 0ULL;
    }
    if (outCpuThinkingVisible != nullptr) {
        *outCpuThinkingVisible = false;
    }
    if (outFixedPhysicalStatusOverlay != nullptr) {
        *outFixedPhysicalStatusOverlay = {};
    }
    outLedFrame.fill(modules::BoardSquareLeds{});
    outEdgeFrame.fill(modules::RGB_BLACK);
    outGridFrame.fill(modules::RGB_BLACK);
    if (outGridFrameBeforeMenuOverlay != nullptr) {
        outGridFrameBeforeMenuOverlay->fill(modules::RGB_BLACK);
    }
    if (outMenuForeground != nullptr) {
        outMenuForeground->Clear();
    }

    internal::ComposeContext ctx;
    if (!internal::BuildComposeContext(in, ctx)) {
        return;
    }

    if (outHint != nullptr) outHint->retainPrior = ctx.hintMoveValid &&
        ctx.renderIntent.hintPromptVisible && ctx.hintRefinementAlpha255 == 0U;
    if (outHint != nullptr && ctx.hintMoveValid &&
        ctx.renderIntent.hintPromptVisible && ctx.hintRefinementAlpha255 != 0U) {
        // Keep the legal continuation attached to the chess move the player saw.
        // Its Duck lights first appear when the interaction enters DuckRelocation.
        outHint->tuple = {ctx.hintFromSquare, ctx.hintToSquare,
            ctx.hintDuckMoveValid ? ctx.hintDuckFromSquare : uint8_t{255U},
            ctx.hintDuckMoveValid ? ctx.hintDuckToSquare : uint8_t{255U}};
        outHint->visible = true;
    }

    if (outCaptureRemovalMask != nullptr)
        *outCaptureRemovalMask = internal::PhysicalRemovalSquareMask(ctx);

    // Reuse the resolved visible assist owner; transport must not infer meaning
    // from pixel colors or independently decide whether Fresh Setup is active.
    if (outSidePrioritySquareMask != nullptr && ctx.initialKingPlacementPromptActive) {
        for (uint8_t square = 0U; square < 64U; ++square) {
            if ((ctx.initialKingPlacementPromptMask & (1ULL << square)) != 0ULL) {
                *outSidePrioritySquareMask |=
                    1ULL << square;
            }
        }
    }
    const auto& reminderSnapshot = ctx.Snapshot();
    const auto& reminder = reminderSnapshot.interaction.taggedState;
    if (outDuckReminderMask != nullptr &&
        reminderSnapshot.game.variant == modules::GameVariant::Duck &&
        reminder.tag == runtime::ChessInteractionPhase::DuckRelocation &&
        reminder.plan.kind == runtime::PhysicalTransitionKind::DuckRelocation &&
        reminder.requiredAction == runtime::RequiredPhysicalAction::LiftPrimary &&
        reminder.plan.moveFrom < 64U &&
        reminderSnapshot.game.duckSquare == reminder.plan.moveFrom &&
        (ctx.physicalOcc64 & (UINT64_C(1) << reminder.plan.moveFrom)) != 0ULL &&
        !ctx.renderIntent.HasCelebration() && !ctx.renderIntent.menuForegroundVisible &&
        !ctx.renderIntent.centerTextOverlayVisible && !ctx.invalidBoard.IsActive() &&
        ctx.humanIllegalMask == 0ULL && ctx.cpuIllegalMask == 0ULL &&
        !ctx.in->higherPriorityStatusOverlayActive &&
        !reminderSnapshot.system.safeModeActive && !reminderSnapshot.system.hardwareTestActive &&
        !reminderSnapshot.system.amptestActive && !reminderSnapshot.system.otaInProgress &&
        !runtime::IsRuntimeHealthPlayPaused(ctx.in->runtimeHealth)) {
        *outDuckReminderMask = UINT64_C(1) << reminder.plan.moveFrom;
    }
    const uint64_t captureTaps = internal::ComposeBoard(ctx, outLedFrame, outGridFrame);
    if (outCaptureTapMask != nullptr) *outCaptureTapMask = captureTaps;
    internal::ApplyPlacementStylePreview(ctx, outLedFrame, outGridFrame);
    internal::ApplyMenuBackgroundDim(ctx, outLedFrame, outGridFrame);

    if (outGridFrameBeforeMenuOverlay != nullptr) {
        *outGridFrameBeforeMenuOverlay = outGridFrame;
    }
    internal::ApplyMenuOverlay(
        ctx, outLedFrame, outGridFrame, outMenuForeground);
    internal::ApplyCenterTextOverlay(ctx, outLedFrame, outGridFrame);
    internal::ApplyCheckmateCelebration(ctx, outLedFrame, outGridFrame);
    internal::ApplyDrawCelebration(ctx, outLedFrame, outGridFrame);
    const bool promotionVisible = internal::ApplyPromotionReveal(ctx, outLedFrame, outGridFrame);
    const bool thinkingVisible = internal::ComposeEdgeLeds(
        ctx, outLedFrame, outEdgeFrame, outFixedPhysicalStatusOverlay);
    if (outCpuThinkingVisible != nullptr) {
        *outCpuThinkingVisible = thinkingVisible;
    }
    internal::ApplyGlobalBrightnessBoost(outLedFrame, outEdgeFrame, outGridFrame);
    if (outMenuForeground != nullptr) {
        internal::ApplyGlobalBrightnessBoost(*outMenuForeground);
    }
    // Export semantic ownership, never infer it from the final RGB. A startup
    // accent cannot claim a required piece prompt even if its color is identical.
    const auto& snapshot = ctx.Snapshot();
    const bool quiet = runtime::IsIdleInteraction(snapshot) &&
        !ctx.renderIntent.menuForegroundVisible && !ctx.renderIntent.centerTextOverlayVisible &&
        !ctx.renderIntent.coachingOverlayActive && !ctx.renderIntent.HasCelebration() &&
        !ctx.invalidBoard.IsActive() && ctx.humanIllegalMask == 0ULL &&
        ctx.cpuIllegalMask == 0ULL && !(ctx.captureReturnOn && ctx.captureReturnMask != 0ULL) && !ctx.kingSafetyPromptActive &&
        !thinkingVisible && !ctx.undoPromptActive &&
        !ctx.kingQueenSwapPromptActive && !(ctx.settleOn && ctx.settleTimingValid) && !ctx.settleAfterglowActive &&
        !promotionVisible && !ctx.allPiecesFlashActive && !in.higherPriorityStatusOverlayActive &&
        !snapshot.system.safeModeActive && !snapshot.system.hardwareTestActive &&
        !snapshot.system.amptestActive && !snapshot.system.otaInProgress &&
        !runtime::IsRuntimeHealthPlayPaused(in.runtimeHealth);
    if (outOrdinaryPieceMask != nullptr && quiet) {
        uint64_t logicalMask = ctx.physicalOcc64 & ~ctx.initialKingPlacementPromptMask;
        const auto exclude = [&](const bool valid, const uint8_t square) {
            if (valid && square < 64U) { logicalMask &= ~(1ULL << square); }
        };
        exclude(ctx.p1LastMoveValid, ctx.p1LastMoveFrom);
        exclude(ctx.p1LastMoveValid, ctx.p1LastMoveTo);
        exclude(ctx.p2LastMoveValid, ctx.p2LastMoveFrom);
        exclude(ctx.p2LastMoveValid, ctx.p2LastMoveTo);
        exclude(snapshot.game.variant == modules::GameVariant::Duck, snapshot.game.duckSquare);
        for (uint8_t square = 0U; square < 64U; ++square) {
            if ((logicalMask & (1ULL << square)) != 0ULL) {
                *outOrdinaryPieceMask |= 1ULL << square;
            }
        }
    }

}

// Existing three-slot callers are compatibility consumers, never the live output owner.
void ComposeStartupGrid(const fxe::SettingsStore& settings,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& out) {
    modules::BoardLedFrame frame{};
    ComposeStartupGrid(settings, frame);
    out = modules::ProjectLegacyBoardFrame(frame);
}

void Compose(const ComposeInputs& in,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& out,
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edge,
    std::array<modules::Rgb, 64>& grid) {
    Compose(in, out, edge, grid, nullptr);
}

void Compose(const ComposeInputs& in,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& out,
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edge,
    std::array<modules::Rgb, 64>& grid,
    std::array<modules::Rgb, 64>* beforeMenu,
    bool* thinking, uint64_t* priority, uint64_t* ordinary,
    modules::status_led_frame_adapter::FixedPhysicalStatusOverlay*
        fixedPhysicalStatusOverlay,
    modules::GridForegroundLayer* menuForeground,
    uint64_t* captureRemovalMask, uint64_t* outDuckReminderMask,
    HintPresentationOutcome* outHint) {
    modules::BoardLedFrame frame{};
    Compose(in, frame, edge, grid, beforeMenu, thinking, priority, ordinary,
            fixedPhysicalStatusOverlay, menuForeground, captureRemovalMask, outDuckReminderMask, outHint);
    out = modules::ProjectLegacyBoardFrame(frame);
}

} // namespace modules::led_frame_composer
