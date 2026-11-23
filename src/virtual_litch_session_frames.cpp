#include "virtual_litch_session_internal.hpp"

#ifdef LITCH_NATIVE

#include <algorithm>
#include <cmath>

#include "foxie_embedded/frame_hash.hpp"
#include "firmware/sleep_countdown_led_frame.hpp"
#include "firmware/startup_led_sequence.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/orientation.hpp"
#include "modules/quick_settings_orchestration.hpp"
#include "modules/quick_settings_hold_preview.hpp"
#include "modules/game_reset_hold_policy.hpp"
#include "modules/brightness_scale_policy.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/litchcraft_physical_presentation.hpp"
#include "modules/startup_choreography.hpp"
#include "modules/water_slosh_presentation.hpp"
#include "modules/white_squares_setting.hpp"
#include "modules/rgbw_auto_map.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"

namespace runtime::virtual_litch_internal {
namespace {

static_assert(hardware::LEDS_PER_SQUARE <=
                  VIRTUAL_LITCH_MAX_LEDS_PER_SQUARE,
              "VirtualLitch semantic frame needs more per-square capacity");
static_assert(hardware::MAX_PHYSICAL_LED_COUNT <=
                  VIRTUAL_LITCH_MAX_PHYSICAL_LED_COUNT,
              "VirtualLitch physical frame capacity is too small");

virtual_litch_color ToColor(const modules::Rgb color) {
    return virtual_litch_color{color.r, color.g, color.b, 0U};
}

} // namespace

CaptureLedRenderer::CaptureLedRenderer(const uint16_t ledCount,
                                       const bool rgbw)
    : m_ledCount(ledCount), m_rgbw(rgbw) {
    Clear();
}

void CaptureLedRenderer::Clear() {
    m_pixels.fill(virtual_litch_color{});
}

void CaptureLedRenderer::SetLedRgb(const uint16_t ledIndex,
                                   const uint8_t r,
                                   const uint8_t g,
                                   const uint8_t b) {
    if (ledIndex >= m_ledCount) {
        return;
    }
    if (!m_rgbw) {
        m_pixels[ledIndex] = virtual_litch_color{r, g, b, 0U};
        return;
    }
    const modules::RgbwChannels mapped = modules::AutoMapRgbToRgbw(
        modules::Rgb{r, g, b}, 32U, m_rgbwWhiteMixScale256);
    m_pixels[ledIndex] =
        virtual_litch_color{mapped.r, mapped.g, mapped.b, mapped.w};
}

void Session::BeginBootDisplay() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(m_nowMs);
    uint64_t resumeOccupancy = 0ULL;
    const bool resumableGame = m_gamePersistence &&
        m_gamePersistence->TryGetBootResumeOccupancy(resumeOccupancy);
    m_bootDisplay.Begin(modules::ReadBootDisplayChoice(m_settings),
        HardwareProfile(m_config).revision == hardware::BoardRevision::V3_3S3 &&
        m_mode != SessionMode::FactoryJourney,
        {m_gamePersistence ? m_gamePersistence->Litchcraft().ModeGeneration() : 0U,
         m_gamePersistence ? m_gamePersistence->Litchcraft().CanvasGeneration() : 0U,
         resumeOccupancy, resumableGame});
}

void Session::BeginStartupPieceAccent() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(m_nowMs);
    m_startupPieceAccent.Begin();
}

void Session::CaptureStartupFrame(const uint32_t nowMs,
                                  const uint16_t fadeScale256) {
    m_nowMs = nowMs;
    m_cpuThinkingVisible = false;
    m_sidePrioritySquareMask = 0ULL;
    m_captureRemovalMask = 0ULL;
    m_captureTapMask = 0ULL;
    m_duckReminderMask = 0ULL;
    m_fixedPhysicalStatusOverlay = {};
    modules::led_frame_composer::ComposeStartupGrid(m_settings, m_semanticLedFrame);
    if (firmware::StartupLedSequence::UsesPreServiceGrid(HardwareProfile(m_config).revision)) {
        modules::startup_choreography::ApplyGrid(m_semanticLedFrame, fadeScale256);
    } else {
        m_semanticLedFrame.fill(modules::BoardSquareLeds{});
    }
    m_statusFrame.fill(modules::RGB_BLACK);
    ++m_frameSequence;
    CaptureSemanticFrame();
    modules::PowerBudgetPolicy policy(
        modules::led_render_policy::MakeConservativePowerBudgetPolicyConfig(
            {HardwareProfile(m_config).isS3, false}));
    const uint16_t scale = modules::brightness_scale_policy::ComputeEffectiveScale256(
        modules::white_squares_setting::ReadBrightness0To8(m_settings),
        false, 0.0f, 256U,
        m_settings.ReadValue("AUTO_BRIGHTNESS", true));
    const bool rgbw = HardwareProfile(m_config).isS3 &&
        HardwareProfile(m_config).revision != hardware::BoardRevision::V3_3S3;
    auto pipelineConfig = modules::led_render_policy::MakePipelineConfig(
        rgbw, 32U);
    pipelineConfig.outputBrightnessScale256 = HardwareProfile(m_config).ledOutputScale256;
    if (HardwareProfile(m_config).revision == hardware::BoardRevision::V3_3S3) {
        pipelineConfig.channelMaxCurrentMa = 20U;
    }
    modules::BudgetAwareLedPipeline pipeline(m_renderer, policy,
                                             pipelineConfig);
    CapturePhysicalFrame(scale, &pipeline);
}

void Session::CaptureFrame() {
    m_stateMachine.RetireExpiredAllPiecesFlash(m_nowMs);
    const bool initialStoreModePending = m_gamePersistence &&
        m_gamePersistence->Litchcraft().IsInitialStoreModePending();
    m_bootDisplay.UpdatePriority(
        m_mode == SessionMode::FactoryJourney || m_hardwareTest.IsActive() ||
        m_runtimeHealth.Snapshot().state != runtime::RuntimeHealthState::Healthy,
        {m_gamePersistence ? m_gamePersistence->Litchcraft().ModeGeneration() : 0U,
         m_gamePersistence ? m_gamePersistence->Litchcraft().CanvasGeneration() : 0U});
    if (m_gamePersistence) m_bootDisplay.UpdateArtwork(m_gamePersistence->Litchcraft(), m_bootArtworkUnavailable);
    SyncOwnerPairingContext();
    if (m_gamePersistence != nullptr) {
        m_gamePersistence->PresentPendingOccupiedBoardFeedback(m_edgeCue);
    }
    m_cpuThinkingVisible = false;
    m_sidePrioritySquareMask = 0ULL;
    if (m_mode == SessionMode::FactoryJourney || m_hardwareTest.IsActive()) {
        if (m_ownerPairing) (void)m_ownerPairing->SyncPhysicalMessage(
            m_menuOverlay, m_gridFrameBeforeMenuOverlay, false, false);
        m_waterSlosh.SetAdmitted(false, m_nowMs);
        CaptureManufacturingFrame();
        return;
    }
    m_identifyOverlayActive = m_bleIdentify.Update(
        m_nowMs, m_quickSettingsOverlayActive, m_menuOverlay, m_gridFrameBeforeMenuOverlay);
    const bool ownerPairingAllowed = m_ownerPairing && !m_identifyOverlayActive &&
        !runtime::IsRuntimeHealthPlayPaused(m_runtimeHealth.Snapshot());
    const bool ownerPairingRestartMessage = m_ownerPairing &&
        m_ownerPairing->SyncPhysicalMessage(m_menuOverlay, m_gridFrameBeforeMenuOverlay,
                                            ControlBanksSwapped(), ownerPairingAllowed);
    SyncBeginnerModePolicy();
    InteractionModelSnapshotBuildInputs snapshotInputs;
    snapshotInputs.stateMachine = &m_stateMachine;
    snapshotInputs.menuOverlayActive = m_menuOverlay.IsActive();
    snapshotInputs.quickSettingsOverlayActive =
        m_quickSettingsOverlayActive;
    snapshotInputs.startupPresentationActive = initialStoreModePending ||
        !m_bootDisplay.IsComplete() || m_startupPieceAccent.IsArmed();
    if (m_mode == SessionMode::EngineBackedJourney) {
        snapshotInputs.derived =
            &m_gamePersistence->GetEngineDerivedState();
        snapshotInputs.authoritativeFen = m_gamePersistence->GetPresentationFen();
        snapshotInputs.captureFirstCapturerMask = m_gamePersistence->GetCaptureFirstCapturerMask();
        snapshotInputs.gameClock = &m_gamePersistence->GetGameClock();
        snapshotInputs.clockWinner = m_gamePersistence->GetClockTerminalWinner();
        snapshotInputs.clockTerminalStartMs =
            m_gamePersistence->GetClockTerminalStartMs();
        snapshotInputs.variant = m_gamePersistence->GetGameVariant();
        snapshotInputs.duckSquare = m_gamePersistence->GetDuckSquare();
        snapshotInputs.initialDuckCandidateSquare =
            m_gamePersistence->GetInitialDuckCandidateSquare();
        snapshotInputs.initialVariantValidationActive =
            m_gamePersistence->IsInitialVariantValidationPending();
        snapshotInputs.setupLifecycle = CurrentSetupLifecycle();
        snapshotInputs.duckTurnPhase =
            m_gamePersistence->GetDuckTurnPhase();
        snapshotInputs.variantOutcome =
            m_gamePersistence->GetVariantOutcome();
        snapshotInputs.variantTerminalStartMs = m_gamePersistence->GetVariantTerminalStartMs();
        snapshotInputs.kingResignationOwner = m_gamePersistence->GetKingResignationOwner();
        snapshotInputs.kingResignationSquare = m_gamePersistence->GetKingResignationSquare();
        snapshotInputs.kingResignationElapsedMs = m_gamePersistence->GetKingResignationElapsedMs();
        snapshotInputs.kingResignationArmed = m_gamePersistence->IsKingResignationArmed();
    } else {
        snapshotInputs.derived = &m_derived;
        snapshotInputs.setupLifecycle = m_setupLifecycle;
    }
    m_snapshot = BuildInteractionModelSnapshot(snapshotInputs);
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &m_settings;
    inputs.interactionSnapshot = &m_snapshot;
    inputs.runtimeHealth = m_runtimeHealth.Snapshot();
    inputs.litchcraftActive =
        m_gamePersistence && m_gamePersistence->Litchcraft().IsCanvasVisible();
    const bool journeyFirstPlayerIsP2 =
        m_settings.ReadValue("GAME_FIRST_PLAYER",
                             static_cast<uint32_t>(1U)) == 2U;
    inputs.firstPlayerIsP2 =
        m_mode == SessionMode::EngineBackedJourney
            ? journeyFirstPlayerIsP2
            : m_config.first_player_is_p2 != 0U;
    inputs.menuOverlay = &m_menuOverlay;
    inputs.menuActive = m_quickSettingsOverlayActive ||
        m_menuOverlay.IsActive() || m_identifyOverlayActive;
    inputs.menuOverlayPreservesTurnIndicator =
        m_menuOverlay.PreservesTurnIndicator();
    if (m_quickSettingsOverlayActive) {
        inputs.menuVisualAlpha255 = 255U;
        inputs.menuBackgroundDimScale256 = 26U;
        inputs.menuIntent.menuActive = true;
        inputs.menuIntent.menuOwner = m_quickSettingsOverlayOwner;
        inputs.menuIntent.menuSelectionIndex =
            m_quickSettingsOverlaySelectedIndex;
        if (m_quickSettingsOverlaySelectedIndex ==
            modules::quick_settings_items::TIMER) {
            const uint32_t timer = m_settings.ReadValue(
                "TIMER", static_cast<uint32_t>(0U));
            const uint32_t clamped = timer > 6U ? 6U : timer;
            inputs.menuIntent.menuValue01_255 = static_cast<uint8_t>(
                (clamped * 255U + 3U) / 6U);
        }
    }
    if (m_identifyOverlayActive) {
        inputs.menuVisualAlpha255 = 255U;
        inputs.menuBackgroundDimScale256 = 0U;
    }
    if (ownerPairingRestartMessage) {
        inputs.menuVisualAlpha255 = 255U;
        inputs.menuBackgroundDimScale256 = 26U;
    }
    if (!m_quickSettingsOverlayActive) {
        const bool directOpenAllowed = modules::IsQuickSettingsDirectEntryAllowed({
            .hardwareTestActive = m_hardwareTest.IsActive(),
            .resetHoldActive = m_gameResetHoldActive,
            .menuOverlayActive = m_menuOverlay.IsActive(),
            .textOverlayActive = m_stateMachine.IsTextOverlayActive(),
            .kingQueenSwapActive = m_stateMachine.IsKingQueenSwapPromptActive(),
            .undoPromptActive = m_stateMachine.IsUndoPromptActive(),
            .promotionActive = m_stateMachine.IsPromotionActive(),
        });
        const auto preview = modules::BuildQuickSettingsHoldPreview(
            m_quickSettingsOverlayController,
            m_gamePersistence ? &m_gamePersistence->Litchcraft() : nullptr,
            directOpenAllowed);
        if (preview.menuActive) {
            inputs.menuIntent = preview;
            inputs.menuIntent.menuSelectionIndex = m_quickSettingsOverlaySelectedIndex;
        }
    }

    if (m_gameResetHoldActive && modules::IsGameResetHoldEligible({
            .menuVisualActive = false,
            .menuOverlayActive = m_menuOverlay.IsActive(),
            .quickSettingsOverlayActive = m_quickSettingsOverlayActive,
            .textOverlayActive = m_stateMachine.IsTextOverlayActive(),
            .promotionActive = m_stateMachine.IsPromotionActive()})) {
        modules::ProjectGameResetHold(
            inputs.menuIntent, m_gameResetHoldOwner,
            m_nowMs - m_gameResetHoldStartMs, m_gameResetHoldTriggered,
            m_gameResetHoldRejected);
    }
    inputs.edgeCueIntent = m_edgeCue.Current(m_nowMs);
    inputs.paletteEditOwners = HardwareProfile(m_config).revision == hardware::BoardRevision::V3_3S3
        ? 0U : m_paletteEditPreview.VisibleOwners();
    inputs.beginnerModeP1Enabled =
        m_beginnerMode.IsEffective(modules::UiOwner::P1);
    inputs.beginnerModeP2Enabled =
        m_beginnerMode.IsEffective(modules::UiOwner::P2);
    if (m_sleep.GetState() != firmware::SleepState::Awake) m_boardStylePreview.Cancel();
    inputs.boardStylePreview = m_boardStylePreview.Capture(inputs);
    modules::led_frame_composer::Compose(
        inputs,
        m_semanticLedFrame,
        m_statusFrame,
        m_gridFrame,
        &m_gridFrameBeforeMenuOverlay,
        &m_cpuThinkingVisible,
        &m_sidePrioritySquareMask, &m_ordinaryPieceMask,
        &m_fixedPhysicalStatusOverlay, &m_menuForeground, &m_captureRemovalMask, &m_duckReminderMask,
        nullptr, &m_captureTapMask);
    if (m_gamePersistence && m_gamePersistence->Litchcraft().IsCanvasVisible()) {
        const auto sharedStatusFrame = m_statusFrame;
        m_waterSlosh.SetAdmitted(false,m_nowMs);
        m_startupPieceAccent.Cancel(); m_startupPieceScale256=256U;
        if (modules::LitchcraftCanvasOwnsFrame(
                true,
                runtime::IsRuntimeHealthPlayPaused(m_runtimeHealth.Snapshot()),
                m_identifyOverlayActive || ownerPairingRestartMessage))
            m_gamePersistence->Litchcraft().Compose(m_semanticLedFrame,m_statusFrame);
            m_menuForeground.Clear();
        modules::led_frame_composer::RestoreLitchcraftStatusWarnings(
            inputs, sharedStatusFrame, m_statusFrame);
        modules::led_frame_composer::ApplyLitchcraftOccupiedBoardWarning(
            inputs, m_gamePersistence->Litchcraft().Occupancy(), m_semanticLedFrame);
    } else {
        CaptureWaterSlosh(inputs);
        if (m_gamePersistence &&
            m_gamePersistence->Litchcraft().IsEntryHoldActive() &&
            !runtime::IsRuntimeHealthPlayPaused(m_runtimeHealth.Snapshot())) {
            m_gamePersistence->Litchcraft().ComposeEntryStatus(m_statusFrame);
        }
    }
    const firmware::SleepCountdownLedFrame sleepCountdown =
        firmware::ComposeSleepCountdownLedFrame(
            m_sleep.GetState(), m_sleep.GetRemainingCountdownMs(),
            m_sleep.GetCountdownDurationMs());
    if (sleepCountdown.active &&
        !runtime::IsRuntimeHealthPlayPaused(m_runtimeHealth.Snapshot())) {
        m_startupPieceAccent.Cancel();
        m_startupPieceScale256 = 256U;
        m_statusFrame = sleepCountdown.semanticStatus;
        m_sidePrioritySquareMask = 0ULL;
        m_fixedPhysicalStatusOverlay = {};
    }

    ComposeControlFeedback(inputs);
    if (ownerPairingAllowed) m_ownerPairing->ComposePhysicalCue(
        m_fixedPhysicalStatusOverlay, ControlBanksSwapped());
    ++m_frameSequence;
    CaptureState();
    CaptureRuntimeHealth();
    CaptureSleepLifecycle();
    CaptureSemanticFrame();
    CapturePhysicalFrame(UINT16_MAX, nullptr, initialStoreModePending);
    AppendHistory();
}

void Session::CaptureRuntimeHealth() {
    const runtime::RuntimeHealthSnapshot snapshot =
        m_runtimeHealth.Snapshot();
    auto& out = m_runtimeHealthCapture;
    out = virtual_litch_runtime_health{};
    out.struct_size = sizeof(out);
    out.abi_version = VIRTUAL_LITCH_RUNTIME_HEALTH_ABI_VERSION;
    out.now_ms = m_nowMs;
    out.frame_sequence = m_frameSequence;
    out.revision = snapshot.revision;
    out.causes = snapshot.causes;
    out.state = static_cast<uint8_t>(snapshot.state);
    out.board_input_allowed = snapshot.advice.boardInputAllowed ? 1U : 0U;
    out.move_acceptance_allowed =
        snapshot.advice.moveAcceptanceAllowed ? 1U : 0U;
    out.clocks_allowed = snapshot.advice.clocksAllowed ? 1U : 0U;
}

void Session::CaptureSleepLifecycle() {
    auto& out = m_sleepLifecycle;
    out = virtual_litch_sleep_lifecycle{};
    out.struct_size = sizeof(out);
    out.abi_version = VIRTUAL_LITCH_SLEEP_LIFECYCLE_ABI_VERSION;
    out.now_ms = m_nowMs;
    out.frame_sequence = m_frameSequence;
    out.remaining_countdown_ms = m_sleep.GetRemainingCountdownMs();
    out.transition_revision = m_sleep.GetRevision();
    out.state = static_cast<uint8_t>(m_sleep.GetState());
    out.origin = static_cast<uint8_t>(m_sleep.GetOrigin());
    out.cancel_cause = static_cast<uint8_t>(m_sleep.GetCancelCause());
    out.persistence_preflight_complete =
        m_sleep.IsPersistencePreflightComplete() ? 1U : 0U;
    out.wake_source_preflight_complete =
        m_sleep.IsWakeSourcePreflightComplete() ? 1U : 0U;
    out.sleep_authorization_pending =
        m_sleep.HasPendingSleepAuthorization() ? 1U : 0U;
}

void Session::CaptureState() {
    auto& out = m_current.state;
    out = virtual_litch_state{};
    out.now_ms = m_nowMs;
    out.frame_sequence = m_frameSequence;
    out.board_observation_sequence = m_boardObservationSequence;
    out.pending_action_token =
        m_snapshot.interaction.taggedState.pendingActionToken;
    out.feedback_epoch =
        m_stateMachine.GetChessInteractionModel().feedbackEpoch;
    out.physical_occupied_mask = m_snapshot.board.physicalOccupiedMask;
    out.expected_occupied_mask = m_snapshot.board.expectedOccupiedMask;
    out.valid_destination_mask =
        m_snapshot.interaction.validDestinationMask;
    out.castling_king_destination_mask =
        m_snapshot.interaction.castlingKingDestinationMask;
    out.castling_rook_source_mask =
        m_snapshot.interaction.castlingRookSourceMask;
    out.correction_mask =
        m_snapshot.interaction.taggedState.correctionMask;
    out.human_illegal_mask = m_snapshot.interaction.humanIllegalMask;
    out.cpu_illegal_mask = m_snapshot.interaction.cpuIllegalMask;
    out.interaction_phase = static_cast<uint8_t>(
        m_snapshot.interaction.taggedState.tag);
    out.required_action = static_cast<uint8_t>(
        m_snapshot.interaction.taggedState.requiredAction);
    out.selected_square = m_snapshot.interaction.taggedState.selectedSquare;
    out.side_to_move_black =
        m_snapshot.game.sideToMove == modules::SideToMove::Black ? 1U : 0U;
}

void Session::CaptureSemanticFrame() {
    auto& out = m_current.semantic;
    out = virtual_litch_semantic_frame{};
    out.now_ms = m_nowMs;
    out.frame_sequence = m_frameSequence;
    out.leds_per_square = hardware::LEDS_PER_SQUARE;
    const auto legacy = modules::ProjectLegacyBoardFrame(m_semanticLedFrame);
    for (uint8_t square = 0U; square < 64U; ++square) {
        for (uint8_t slot = 0U; slot < hardware::LEDS_PER_SQUARE; ++slot) {
            const uint16_t index = static_cast<uint16_t>(
                square * hardware::LEDS_PER_SQUARE + slot);
            out.board[square][slot] = ToColor(legacy[index]);
        }
    }
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        out.status[index] = ToColor(m_statusFrame[index]);
    }
    out.board_hash = fxe::HashRgbFrame(
        legacy.data(), hardware::BOARD_LED_COUNT);
    out.status_hash =
        fxe::HashRgbFrame(m_statusFrame.data(), m_statusFrame.size());
    out.combined_hash =
        fxe::HashCombineU64(out.board_hash, out.status_hash);
}

void Session::AppendHistory() {
    if (m_history.empty()) {
        return;
    }
    m_history[m_historyWriteIndex] = m_current;
    m_historyWriteIndex = (m_historyWriteIndex + 1U) % m_history.size();
    if (m_historyCount < m_history.size()) {
        ++m_historyCount;
    }
}

} // namespace runtime::virtual_litch_internal

#endif
