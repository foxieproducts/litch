#include "virtual_litch_session_internal.hpp"

#ifdef LITCH_NATIVE

#include <algorithm>
#include <cmath>

#include "foxie_embedded/frame_hash.hpp"
#include "firmware/sleep_countdown_led_frame.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
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
uint16_t ScaleToU16(const float scale01) {
    const float bounded = std::max(0.0f, std::min(1.0f, scale01));
    return static_cast<uint16_t>(bounded * 65535.0f + 0.5f);
}

uint64_t HashPhysicalFrame(
    const std::array<virtual_litch_color,
                     VIRTUAL_LITCH_MAX_PHYSICAL_LED_COUNT>& pixels,
    const uint16_t ledCount,
    const uint8_t channelCount) {
    uint64_t hash = fxe::FNV1A64_OFFSET_BASIS;
    hash = fxe::Fnv1a64Update(hash, &channelCount, 1U);
    for (uint16_t index = 0U; index < ledCount; ++index) {
        const auto& pixel = pixels[index];
        const uint8_t channels[4] = {pixel.r, pixel.g, pixel.b, pixel.w};
        hash = fxe::Fnv1a64Update(hash, channels, channelCount);
    }
    return hash;
}

} // namespace

void Session::CapturePhysicalFrame(const uint16_t scaleOverride,
                                   modules::BudgetAwareLedPipeline* startupPipeline,
                                   const bool initialStoreModePending) {
    // A denied production lease preserves the last delivered physical frame
    // and cannot acknowledge a boot frame or consume its visible dwell.
    if (m_ledOutputArbiter != nullptr && !m_ledOutputArbiter->TryBeginRender()) return;
    auto& pipeline = startupPipeline != nullptr ? *startupPipeline : m_pipeline;
    virtual_litch_config outputConfig = m_config;
    if (m_mode == SessionMode::EngineBackedJourney) {
        const bool firstPlayerIsP2 =
            m_settings.ReadValue("GAME_FIRST_PLAYER",
                                 static_cast<uint32_t>(1U)) == 2U;
        const bool boardRotate =
            m_settings.ReadValue("BOARD_ROTATE", false);
        outputConfig.board_rotate = boardRotate ? 1U : 0U;
        outputConfig.square_rotate_for_output =
            modules::ComputeSquareRotate(boardRotate, firstPlayerIsP2,
                m_gamePersistence && m_gamePersistence->Litchcraft().IsCanvasVisible())
                ? 1U
                : 0U;
    }
    const bool boardRotateForOutput = outputConfig.board_rotate != 0U;
    const bool squareRotateForOutput =
        outputConfig.square_rotate_for_output != 0U;
    const bool fixedControlOverlayActive =
        m_snapshot.render.menuOverlayActive ||
        m_snapshot.render.quickSettingsOverlayActive;
    const bool sleepCountdownActive =
        (m_sleep.GetState() == firmware::SleepState::Countdown ||
         m_sleep.GetState() == firmware::SleepState::Fading ||
         m_sleep.GetState() == firmware::SleepState::Preparing) &&
        !runtime::IsRuntimeHealthPlayPaused(m_runtimeHealth.Snapshot());
    const bool controlBanksSwapped = modules::ComputeControlBankSwap(
        boardRotateForOutput,
        m_mode == SessionMode::EngineBackedJourney ? FirstPlayerIsP2()
                                                 : outputConfig.first_player_is_p2 != 0U,
        m_gamePersistence && m_gamePersistence->Litchcraft().IsCanvasVisible());
    const bool statusRotateForOutput =
        (fixedControlOverlayActive || sleepCountdownActive)
            ? boardRotateForOutput
            : controlBanksSwapped;
    const hardware::LedFrameMappingConfig mappingConfig{
        HardwareProfile(outputConfig).revision, squareRotateForOutput,
        statusRotateForOutput, m_sidePrioritySquareMask};
    // Keep a fixed control bank's menu upright for the person at that edge.
    // Gameplay orientation selects the logical owner; the owner perspective
    // then cancels that mapping for P1 or supplies P2's 180-degree counterpart.
    const bool controlOverlayRotateForOutput =
        controlBanksSwapped !=
        (m_menuOverlay.GetOwner() == modules::UiOwner::P2);
    auto physical = fixedControlOverlayActive && m_menuForeground.IsVisible()
        ? hardware::MapSemanticLedFrameWithGridForeground(
              mappingConfig, controlOverlayRotateForOutput,
              m_semanticLedFrame, m_menuForeground,
              m_statusFrame)
        : hardware::MapSemanticLedFrame(
              mappingConfig, m_semanticLedFrame, m_statusFrame);
    if (m_gamePersistence && m_gamePersistence->Litchcraft().IsCanvasVisible()) {
        modules::litchcraft_physical_presentation::ApplyCanvasScale(
            physical, HardwareProfile(outputConfig).leds.physicalBoardLedCount);
    }
    const uint8_t waterRider = modules::WaterSloshRiderSquare(m_snapshot);
    modules::ApplyWaterSloshPhysicalPresentation(
        m_waterSlosh, HardwareProfile(outputConfig).revision, physical,
        waterRider < 64U
            ? runtime::MapSquareIndex(waterRider, squareRotateForOutput)
            : 255U);
    if (!sleepCountdownActive) {
        hardware::ApplyFixedPhysicalStatusOverlay(
            HardwareProfile(outputConfig).revision,
            m_fixedPhysicalStatusOverlay, physical, controlBanksSwapped);
    }

    const uint16_t ledCount = PhysicalLedCount(m_config);
    // File-backed journeys exercise settings load and physical controls through
    // the same user-brightness policy as firmware. Fixed-scale render fixtures
    // retain their explicit scale. Ambient interpolation is covered by policy
    // tests; these journeys supply the deterministic no-sample fallback.
    const uint16_t scale = scaleOverride <= 256U ? scaleOverride : m_loadPersistedSettings
        ? modules::brightness_scale_policy::ComputeEffectiveScale256(
              modules::white_squares_setting::ReadBrightness0To8(m_settings),
              false, 0.0f, 256U,
              m_settings.ReadValue("AUTO_BRIGHTNESS", true))
        : m_config.brightness_scale_256;
    const uint16_t sleepScale = static_cast<uint16_t>(
        (static_cast<uint32_t>(scale) *
         (runtime::IsRuntimeHealthPlayPaused(m_runtimeHealth.Snapshot())
              ? 256U : m_sleep.GetFadeScale256()) + 128U) >> 8U);
    pipeline.SetBrightnessScale01(static_cast<float>(sleepScale) / 256.0f);
    // Native profiles deliberately use the existing no-sensor brightness
    // fallback. The production owner still requires an actual Hall observation.
    m_startupPieceScale256 = m_startupPieceAccent.Update(
        m_stateMachine.GetLastBoardObservationSequence() != 0U,
        m_snapshot.board.physicalOccupiedMask, true,
        m_ordinaryPieceMask != 0ULL && startupPipeline == nullptr,
        !m_bootDisplay.IsComplete());
    const bool captureAccent = m_captureRemovalMask != 0ULL && !sleepCountdownActive &&
        m_runtimeHealth.Snapshot().state == runtime::RuntimeHealthState::Healthy &&
        !m_settings.IsDisabled() && m_settings.GetLastError() == modules::SettingsStoreError::None &&
        !(m_gamePersistence && m_gamePersistence->Litchcraft().IsCanvasVisible());
    const bool duckAccent = !captureAccent && m_duckReminderMask != 0ULL && !sleepCountdownActive &&
        m_runtimeHealth.Snapshot().state == runtime::RuntimeHealthState::Healthy &&
        !m_settings.IsDisabled() && m_settings.GetLastError() == modules::SettingsStoreError::None &&
        !(m_gamePersistence && m_gamePersistence->Litchcraft().IsCanvasVisible());
    std::array<uint8_t, hardware::MAX_PHYSICAL_LED_COUNT> accentMask{};
    const auto revision = HardwareProfile(outputConfig).revision;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if (captureAccent || duckAccent || (m_ordinaryPieceMask & (1ULL << square)) == 0ULL) { continue; }
        const auto targets = hardware::MapSemanticSquareLedTargets(
            revision, square, hardware::SquareLedSlot::Piece,
            outputConfig.square_rotate_for_output != 0U);
        for (uint8_t target = 0U; target < targets.count; ++target) {
            const uint16_t index = targets.indices[target];
            physical[index] = modules::ScaleRgbSaturatingBy256(
                physical[index], m_startupPieceAccent.PieceScale256(square));
            accentMask[index] = 1U;
        }
    }
    if (captureAccent) for (uint8_t square = 0U; square < 64U; ++square) {
        if ((m_captureRemovalMask & (UINT64_C(1) << square)) == 0ULL) continue;
        for (const auto slot : {hardware::SquareLedSlot::Square0, hardware::SquareLedSlot::Square2}) {
            const auto targets = hardware::MapSemanticSquareLedTargets(revision, square, slot,
                outputConfig.square_rotate_for_output != 0U);
            for (uint8_t i = 0U; i < targets.count; ++i) accentMask[targets.indices[i]] = 1U;
        }
    }
    if (duckAccent) for (uint8_t square = 0U; square < 64U; ++square) {
        if ((m_duckReminderMask & (UINT64_C(1) << square)) == 0ULL) continue;
        for (const auto slot : {hardware::SquareLedSlot::Square0, hardware::SquareLedSlot::Piece,
                                hardware::SquareLedSlot::Square2}) {
            const auto targets = hardware::MapSemanticSquareLedTargets(revision, square, slot,
                outputConfig.square_rotate_for_output != 0U);
            for (uint8_t i = 0U; i < targets.count; ++i) accentMask[targets.indices[i]] = 1U;
        }
    }
    const bool litchcraft = m_gamePersistence &&
        m_gamePersistence->Litchcraft().IsCanvasVisible();
    const bool urgent = runtime::IsRuntimeHealthPlayPaused(m_runtimeHealth.Snapshot()) ||
        m_runtimeHealth.Snapshot().state != runtime::RuntimeHealthState::Healthy ||
        m_settings.IsDisabled() || m_settings.GetLastError() != modules::SettingsStoreError::None ||
        sleepCountdownActive || m_identifyOverlayActive || m_menuOverlay.IsActive() ||
        m_edgeCue.Current(m_nowMs).kind == modules::EdgeLedCueKind::OccupiedBoardBlocked ||
        (m_gamePersistence && (m_gamePersistence->Litchcraft().IsEntryHoldActive() ||
                              m_gamePersistence->Litchcraft().IsExitHoldActive()));
    m_bootDisplay.ObserveBoard(
        m_stateMachine.GetChessInteractionModel().board.valid && !initialStoreModePending,
        m_snapshot.board.physicalOccupiedMask,
        !m_gamePersistence || !m_gamePersistence->Litchcraft().IsActive());
    m_bootDisplay.UpdatePriority(urgent || m_snapshot.render.quickSettingsOverlayActive,
        {m_gamePersistence ? m_gamePersistence->Litchcraft().ModeGeneration() : 0U,
         m_gamePersistence ? m_gamePersistence->Litchcraft().CanvasGeneration() : 0U});
    if (m_bootDisplay.WasCancelled()) m_litchcraftCrossfade.Reset();
    if (m_bootDisplay.NeedsCrossfadeStart()) m_litchcraftCrossfade.TransitionFromPresented(litchcraft, m_nowMs);
    if (!m_bootDisplay.IsActive() || m_bootDisplay.IsArtworkCrossfading())
        m_litchcraftCrossfade.Apply(physical, litchcraft, m_nowMs, urgent);
    if (initialStoreModePending && !urgent && !m_menuForeground.IsVisible()) {
        std::fill_n(physical.begin(), HardwareProfile(outputConfig).leds.physicalBoardLedCount,
                    modules::Rgb{});
    }
    if (m_bootDisplay.UsesArtwork() && m_gamePersistence)
        m_gamePersistence->Litchcraft().Compose(m_semanticLedFrame, m_statusFrame);
    const uint16_t bootFadeScale256 = m_bootDisplay.Apply(physical, boardRotateForOutput,
        m_settings.ReadValue("MINIMAL_ANIMATIONS", false),
        m_bootDisplay.UsesArtwork() ? &m_semanticLedFrame : nullptr,
        m_fixedPhysicalStatusOverlay.ordinaryStatus);
    if (bootFadeScale256 < 256U) {
        accentMask.fill(0U);
        std::fill_n(accentMask.begin(), HardwareProfile(outputConfig).leds.physicalBoardLedCount, 1U);
    }
    const uint16_t rgbwWhiteMixScale256 = m_litchcraftCrossfade.WhiteMix(litchcraft);
    pipeline.SetRgbwWhiteMixScale256(rgbwWhiteMixScale256);
    m_renderer.SetRgbwWhiteMixScale256(rgbwWhiteMixScale256);
    pipeline.RenderFrame(physical.data(), ledCount,
                         {accentMask.data(), accentMask.size(), bootFadeScale256 < 256U ? bootFadeScale256 :
                             captureAccent ? modules::led_render_policy::CAPTURE_REMOVAL_ACCENT_SCALE256 :
                             duckAccent ? modules::led_render_policy::DUCK_REMINDER_ACCENT_SCALE256 : m_startupPieceScale256,
                          bootFadeScale256 < 256U ? uint16_t(0U) :
                             captureAccent || duckAccent ? uint16_t(256U) : uint16_t(0U)});
    if (m_ledOutputArbiter != nullptr) m_ledOutputArbiter->EndRender();
    if ((initialStoreModePending && !m_bootDisplay.IsActive()) || (m_bootDisplay.IsActive() && !m_bootDisplay.UsesArtwork() && !m_bootDisplay.IsArtworkCrossfading())) m_litchcraftCrossfade.Reset();
    else m_litchcraftCrossfade.Presented(physical);
    m_bootDisplay.Presented();
    if (m_bootDisplay.UsesArtwork() && m_gamePersistence)
        m_gamePersistence->Litchcraft().NoteBootArtworkPresented();
    const auto stats = pipeline.GetLastStats();

    auto& out = m_current.physical;
    out = virtual_litch_physical_frame{};
    out.now_ms = m_nowMs;
    out.frame_sequence = m_frameSequence;
    out.led_count = ledCount;
    out.channels_per_led =
        HardwareProfile(m_config).isS3 &&
                HardwareProfile(m_config).revision != hardware::BoardRevision::V3_3S3
            ? 4U : 3U;
    out.estimated_led_current_ma = stats.estimatedLedCurrentMa;
    out.applied_led_current_ma = stats.appliedLedCurrentMa;
    out.applied_brightness_scale_65535 =
        ScaleToU16(stats.appliedBrightnessScale01);
    out.power_limited = stats.powerLimited
                            ? 1U
                            : 0U;
    std::copy_n(m_renderer.Pixels().begin(), ledCount, out.pixels);
    out.frame_hash = HashPhysicalFrame(
        m_renderer.Pixels(), ledCount, out.channels_per_led);
}

} // namespace runtime::virtual_litch_internal
#endif
