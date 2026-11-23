#include "firmware/led_animation_renderer.hpp"

#include <algorithm>

#include "firmware/sleep_countdown_led_frame.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/orientation.hpp"
#include "modules/water_slosh_presentation.hpp"

namespace firmware {

void LedAnimationRenderer::OutputGameFrame(LedAnimationPresentation& presentation,
    FrameOwnership& ownership, LedAnimationFrameResult& result) {
    const uint32_t nowMs = result.receipt.frameStartedMs;
    const bool paused = runtime::IsRuntimeHealthPlayPaused(presentation.health);
    const bool degraded = presentation.health.state != runtime::RuntimeHealthState::Healthy ||
        presentation.settings.persistenceDegraded;
    const uint32_t elapsed = nowMs - presentation.capturedAtMs;
    const uint32_t remaining = presentation.sleepRemainingMs > elapsed
        ? presentation.sleepRemainingMs - elapsed : 0U;
    const auto countdown = ComposeSleepCountdownLedFrame(presentation.sleepState,
        remaining, presentation.sleepDurationMs);
    auto semanticStatus = m_status;
    if (countdown.active && !paused) semanticStatus = countdown.semanticStatus;
    const bool canvas = presentation.output.canvasVisible;
    const bool squareRotate = modules::ComputeSquareRotate(presentation.boardRotate,
        presentation.firstPlayerIsP2, canvas);
    const bool controlSwap = modules::ComputeControlBankSwap(presentation.boardRotate,
        presentation.firstPlayerIsP2, canvas);
    const bool statusRotate = countdown.active && !paused ? presentation.boardRotate
        : ownership.menuActive ? presentation.boardRotate : controlSwap;
    const bool controlRotate = controlSwap != (presentation.menu.GetOwner() == modules::UiOwner::P2);
    const hardware::LedFrameMappingConfig mapping{m_profile->revision, squareRotate,
        statusRotate, countdown.active && !paused ? 0U : ownership.prioritySquares};
    auto physical = ownership.menuActive && ownership.foreground.IsVisible()
        ? hardware::MapSemanticLedFrameWithGridForeground(mapping, controlRotate,
            m_board, ownership.foreground, semanticStatus)
        : hardware::MapSemanticLedFrame(mapping, m_board, semanticStatus);
    if (canvas) modules::litchcraft_physical_presentation::ApplyCanvasScale(
        physical, m_profile->leds.physicalBoardLedCount);
    presentation.water.Update(nowMs);
    modules::ApplyWaterSloshPhysicalPresentation(presentation.water,
        m_profile->revision, physical, presentation.waterRiderSquare < 64U
            ? runtime::MapSquareIndex(presentation.waterRiderSquare, squareRotate) : 255U);
    if (!countdown.active || paused)
        hardware::ApplyFixedPhysicalStatusOverlay(m_profile->revision,
            ownership.status, physical, squareRotate);

    if (countdown.active) {
        m_startupAccent.Cancel();
        m_startupPieceScale256 = 256U;
    }
    const bool capture = ownership.captureRemoval != 0U && !countdown.active && !degraded && !canvas;
    const bool duck = !capture && ownership.duckReminder != 0U && !countdown.active && !degraded && !canvas;
    std::array<uint8_t, hardware::MAX_PHYSICAL_LED_COUNT> accentMask{};
    for (uint8_t square = 0U; square < 64U; ++square) {
        if (capture || duck || (ownership.ordinaryPieces & (UINT64_C(1) << square)) == 0U) continue;
        const auto targets = hardware::MapSemanticSquareLedTargets(m_profile->revision,
            square, hardware::SquareLedSlot::Piece, squareRotate);
        for (uint8_t target = 0U; target < targets.count; ++target) {
            const uint16_t index = targets.indices[target];
            physical[index] = modules::ScaleRgbSaturatingBy256(physical[index],
                m_startupAccent.PieceScale256(square));
            accentMask[index] = 1U;
        }
    }
    if (capture || duck) {
        const uint64_t mask = capture ? ownership.captureTaps : ownership.duckReminder;
        for (uint8_t square = 0U; square < 64U; ++square) {
            if ((mask & (UINT64_C(1) << square)) == 0U) continue;
            for (const auto slot : {hardware::SquareLedSlot::Square0,
                    hardware::SquareLedSlot::Piece, hardware::SquareLedSlot::Square2}) {
                if (capture && slot == hardware::SquareLedSlot::Piece) continue;
                const auto targets = hardware::MapSemanticSquareLedTargets(m_profile->revision,
                    square, slot, squareRotate);
                for (uint8_t target = 0U; target < targets.count; ++target)
                    accentMask[targets.indices[target]] = 1U;
            }
        }
    }
    modules::LedFrameAccent accent{accentMask.data(), accentMask.size(),
        capture ? modules::led_render_policy::CaptureTapScale(ownership.captureTaps) :
        duck ? modules::led_render_policy::DUCK_REMINDER_ACCENT_SCALE256 :
        m_startupPieceScale256, capture || duck ? uint16_t{256U} : uint16_t{0U}};
    const auto* artworkInput = std::get_if<modules::litchcraft_presentation::Inputs>(&presentation.scene);
    const bool urgent = degraded || countdown.active || ownership.menuActive ||
        presentation.entryHold.active || (artworkInput != nullptr && artworkInput->exitHoldActive) ||
        presentation.edgeCue.kind == modules::EdgeLedCueKind::OccupiedBoardBlocked;
    m_boot.ObserveBoard(presentation.startupBoardObserved && !presentation.initialStoreModePending,
        presentation.physicalOccupiedMask, artworkInput == nullptr || !artworkInput->active);
    m_boot.UpdatePriority(urgent || ownership.foreground.IsVisible(),
        {presentation.artworkModeGeneration, presentation.artworkCanvasGeneration});
    if (m_boot.WasCancelled()) m_crossfade.Reset();
    if (auto* artwork = std::get_if<modules::litchcraft_presentation::Inputs>(&presentation.scene);
        artwork != nullptr && !artwork->active && m_boot.UsesArtwork()) {
        result.receipt.artwork = m_artwork.Compose(*artwork, m_artworkPort, m_board, m_status);
        const auto transition = result.receipt.artwork.bootTransition;
        const bool playing = (result.receipt.artwork.playback.bootFlags & 1U) != 0U;
        m_boot.ObserveArtwork(false,
            transition == modules::litchcraft_presentation::BootTransition::Finished,
            playing, presentation.artworkUnavailable);
    }
    if (m_boot.NeedsCrossfadeStart()) m_crossfade.TransitionFromPresented(canvas, nowMs);
    if (!m_boot.IsActive() || m_boot.IsArtworkCrossfading())
        m_crossfade.Apply(physical, canvas, nowMs, urgent);
    if (presentation.initialStoreModePending && !urgent && !ownership.foreground.IsVisible())
        std::fill_n(physical.begin(), m_profile->leds.physicalBoardLedCount, modules::Rgb{});
    const uint16_t bootScale = m_boot.Apply(physical, presentation.boardRotate,
        presentation.settings.minimalAnimations, m_boot.UsesArtwork() ? &m_board : nullptr,
        ownership.status.ordinaryStatus);
    if (bootScale < 256U) {
        accentMask.fill(0U);
        std::fill_n(accentMask.begin(), m_profile->leds.physicalBoardLedCount, 1U);
        accent.scale256 = bootScale;
        accent.ceilingScale256 = 0U;
    }
    const uint16_t requestedScale = Brightness(presentation.output, nowMs);
    uint16_t sleepScale = presentation.sleepScale256;
    if (presentation.sleepState == SleepState::Countdown ||
        presentation.sleepState == SleepState::Fading) {
        const uint32_t age = nowMs - presentation.sleepStartedMs;
        const uint32_t fadeAge = age > presentation.sleepDurationMs
            ? age - presentation.sleepDurationMs : 0U;
        sleepScale = fadeAge >= SleepTransition::FADE_MS ? 0U :
            static_cast<uint16_t>((SleepTransition::FADE_MS - fadeAge) * 256U /
                SleepTransition::FADE_MS);
    }
    const uint16_t scale = static_cast<uint16_t>((uint32_t(requestedScale) *
        (paused ? 256U : sleepScale) + 128U) >> 8U);
    if (const auto* artwork = std::get_if<modules::litchcraft_presentation::Inputs>(&presentation.scene);
        artwork != nullptr && !ArtworkCurrent(*artwork)) return;
    result.attempted = true;
    result.receipt.completed = Output(physical, presentation.output, scale, accent);
    result.receipt.brightnessScale256 = scale;
    if (m_boot.IsActive() || m_boot.IsPending()) {
        result.receipt.visibleHintValid = false;
        result.retainPriorHint = false;
    }
    if (result.receipt.completed) {
        if ((presentation.initialStoreModePending && !m_boot.IsActive()) ||
            (m_boot.IsActive() && !m_boot.UsesArtwork() && !m_boot.IsArtworkCrossfading()))
            m_crossfade.Reset();
        else m_crossfade.Presented(physical);
        m_boot.Presented();
    }
}

} // namespace firmware
