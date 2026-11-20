#include "modules/edge_led_renderer.hpp"

#include "modules/status_led_frame_adapter.hpp"

#include "foxie_embedded/millis.hpp"
#include "modules/animation_timing.hpp"
#include "modules/edge_led_constants.hpp"
#include "modules/edge_led_hint_feedback.hpp"
#include "modules/edge_led_cue.hpp"
#include "modules/rgb.hpp"
#include "modules/turn_anim_style.hpp"
#include "modules/ui_color_wheel.hpp"
#include "modules/ui_types.hpp"

namespace modules {

namespace {

class EdgeLedRendererEngine {
  public:
    explicit EdgeLedRendererEngine(const EdgeLedPalette& palette,
                                   const EdgeLedTiming& timing)
        : m_palette(palette), m_timing(timing) {}

    void Render(const EdgeLedState& state, std::array<Rgb, EDGE_LED_COUNT>& out) const {
        out.fill(m_palette.off);

        switch (state.mode) {
            case EdgeUiMode::FatalCheckmate:
                RenderFatalCheckmate(state, out);
                break;
            case EdgeUiMode::FatalDraw:
                RenderFatalDraw(state, out);
                break;
            case EdgeUiMode::SetupCompleteSweep:
                RenderSetupCompleteSweep(state, out);
                break;
            case EdgeUiMode::UnavailableNudge:
                RenderUnavailableNudge(state, out);
                break;
            case EdgeUiMode::OccupiedBoardWarning:
                RenderOccupiedBoardWarning(state, out);
                break;
            case EdgeUiMode::Error:
                RenderError(state, out);
                break;
            case EdgeUiMode::HealthPaused:
                RenderHealthPaused(state, out);
                break;
            case EdgeUiMode::HealthDegradedUsable:
                RenderHealthDegradedUsable(state, out);
                break;
            case EdgeUiMode::Placement:
                break;
            case EdgeUiMode::Menu:
                RenderMenu(state, out);
                break;
            case EdgeUiMode::Thinking:
                // Thinking now owns the dedicated player controls.
                break;
            case EdgeUiMode::BrightnessPreview:
                RenderBrightness(state, out);
                break;
            case EdgeUiMode::IdleTurn:
                RenderTurn(state, out);
                break;
        }

        ApplyBeginnerModeOverlay(state, out);
        ApplyHintCornerFeedback(state, out);
    }

  private:
    static constexpr uint8_t HomeStart(const UiOwner who) {
        return who == UiOwner::P2 ? 0U : 16U;
    }

    // Map a 0..7 position (left->right from the physical player's perspective on that edge)
    // to the global edge LED index.
    static constexpr uint8_t HomeIndex(const UiOwner who,
                                       const uint8_t localIndex0_7) {
        return PlayerHomeLedIndex(who, localIndex0_7);
    }

    Rgb ColorForSide(const UiOwner who) const {
        return (who == UiOwner::P2) ? m_palette.p2 : m_palette.p1;
    }

    UiOwner ColorOwnerForActiveSide(const EdgeLedState& state) const {
        return (state.activePalette == UiOwner::None) ? state.activeSide : state.activePalette;
    }

    uint32_t ResolveNowMs(const EdgeLedState& state) const {
        return (!state.animationTimeExplicit && state.nowMs == 0U) ? fxe::Millis() : state.nowMs;
    }

    uint32_t ResolveFaultElapsedMs(const EdgeLedState& state) const {
        if (state.faultFlashElapsedMs == 0U && state.faultFlashPeriodMs == 1U) {
            return ResolveNowMs(state);
        }

        return state.faultFlashElapsedMs;
    }

    uint32_t ResolveFaultPeriodMs(const EdgeLedState& state) const {
        return (state.faultFlashPeriodMs <= 1U) ? m_timing.illegalFlashMs : state.faultFlashPeriodMs;
    }

    void ApplyBeginnerModeOverlay(const EdgeLedState& state,
                                  std::array<Rgb, EDGE_LED_COUNT>& out) const {
        if (state.mode != EdgeUiMode::IdleTurn &&
            state.mode != EdgeUiMode::Placement) {
            return;
        }

        const uint32_t nowMs = ResolveNowMs(state);
        const auto ApplyBadge = [&](const UiOwner owner, const bool enabled, const uint32_t phaseOffsetMs) {
            if (!enabled) {
                return;
            }

            const uint16_t pulse = PulseScale256(nowMs + phaseOffsetMs, 1800U, 40U, 104U);
            const Rgb badge = modules::ScaleRgbSaturatingBy256(m_palette.neutral, pulse);
            for (const uint8_t index : PlayerCornerLedIndices(owner)) {
                out[index] = modules::AddRgbSaturating(out[index], badge);
            }
        };

        ApplyBadge(UiOwner::P1, state.beginnerModeP1Enabled, 0U);
        ApplyBadge(UiOwner::P2, state.beginnerModeP2Enabled, 450U);

        if (state.beginnerModeStingerOwner == UiOwner::None) {
            return;
        }

        static constexpr uint32_t STINGER_DURATION_MS = 720U;
        static constexpr uint32_t STINGER_END_FADE_MS = 220U;
        const uint32_t elapsed = (nowMs >= state.beginnerModeStingerStartMs)
                                     ? (nowMs - state.beginnerModeStingerStartMs)
                                     : 0U;
        if (elapsed >= STINGER_DURATION_MS) {
            return;
        }

        static constexpr uint16_t OUTER_LEFT_Q8 = 0U;
        static constexpr uint16_t OUTER_RIGHT_Q8 = static_cast<uint16_t>(7U << 8);
        static constexpr uint16_t CENTER_LEFT_Q8 = static_cast<uint16_t>(3U << 8);
        static constexpr uint16_t CENTER_RIGHT_Q8 = static_cast<uint16_t>(4U << 8);
        static constexpr uint16_t HEAD_RADIUS_Q8 = 384U;
        const auto LerpQ8 = [&](const uint16_t startQ8, const uint16_t endQ8) -> uint16_t {
            if (startQ8 == endQ8) {
                return startQ8;
            }

            if (startQ8 < endQ8) {
                const uint32_t spanQ8 = static_cast<uint32_t>(endQ8 - startQ8);
                return static_cast<uint16_t>(startQ8 +
                                             ((elapsed * spanQ8 + (STINGER_DURATION_MS / 2U)) /
                                              STINGER_DURATION_MS));
            }

            const uint32_t spanQ8 = static_cast<uint32_t>(startQ8 - endQ8);
            return static_cast<uint16_t>(startQ8 -
                                         ((elapsed * spanQ8 + (STINGER_DURATION_MS / 2U)) /
                                          STINGER_DURATION_MS));
        };
        const uint16_t leftHeadPosQ8 = state.beginnerModeStingerEnabling
                                           ? LerpQ8(CENTER_LEFT_Q8, OUTER_LEFT_Q8)
                                           : LerpQ8(OUTER_LEFT_Q8, CENTER_LEFT_Q8);
        const uint16_t rightHeadPosQ8 = state.beginnerModeStingerEnabling
                                            ? LerpQ8(CENTER_RIGHT_Q8, OUTER_RIGHT_Q8)
                                            : LerpQ8(OUTER_RIGHT_Q8, CENTER_RIGHT_Q8);
        const int32_t leftTrailPosQ8 = static_cast<int32_t>(leftHeadPosQ8) +
                                       (state.beginnerModeStingerEnabling ? 256 : -256);
        const int32_t rightTrailPosQ8 = static_cast<int32_t>(rightHeadPosQ8) +
                                        (state.beginnerModeStingerEnabling ? -256 : 256);

        const auto Kernel = [](const uint16_t distQ8) -> uint16_t {
            if (distQ8 >= HEAD_RADIUS_Q8) {
                return 0U;
            }

            const uint16_t remaining = static_cast<uint16_t>(HEAD_RADIUS_Q8 - distQ8);
            return static_cast<uint16_t>((static_cast<uint32_t>(remaining) * 256U + (HEAD_RADIUS_Q8 / 2U)) /
                                         HEAD_RADIUS_Q8);
        };

        const Rgb sideColor = ColorForSide(state.beginnerModeStingerOwner);
        const Rgb headColor = modules::AddRgbSaturating(sideColor, modules::ScaleRgbSaturatingBy256(m_palette.neutral, 160U));
        const Rgb trailColor = modules::ScaleRgbSaturatingBy256(headColor, 112U);
        const uint16_t separationQ8 = (rightHeadPosQ8 >= leftHeadPosQ8)
                                          ? static_cast<uint16_t>(rightHeadPosQ8 - leftHeadPosQ8)
                                          : 0U;
        static constexpr uint16_t START_SEPARATION_Q8 = static_cast<uint16_t>(7U << 8);
        static constexpr uint16_t END_SEPARATION_Q8 = 256U;
        const uint32_t remainingMs = STINGER_DURATION_MS - elapsed;
        const uint16_t endFadeScale256 = (remainingMs >= STINGER_END_FADE_MS)
                                             ? 256U
                                             : static_cast<uint16_t>((remainingMs * 256U + (STINGER_END_FADE_MS / 2U)) /
                                                                     STINGER_END_FADE_MS);
        const auto ApplyEndFade = [&](const uint16_t alpha) -> uint16_t {
            return static_cast<uint16_t>((static_cast<uint32_t>(alpha) * endFadeScale256 + 127U) / 256U);
        };
        const uint16_t centerProgress256 = (separationQ8 <= END_SEPARATION_Q8)
                                               ? 256U
                                               : static_cast<uint16_t>(((static_cast<uint32_t>(START_SEPARATION_Q8 - separationQ8) * 256U) +
                                                                        ((START_SEPARATION_Q8 - END_SEPARATION_Q8) / 2U)) /
                                                                       (START_SEPARATION_Q8 - END_SEPARATION_Q8));
        const uint16_t centerAlpha = ApplyEndFade(static_cast<uint16_t>((static_cast<uint32_t>(centerProgress256) * 176U + 127U) /
                                                                        256U));
        const Rgb centerColor = modules::AddRgbSaturating(sideColor, modules::ScaleRgbSaturatingBy256(m_palette.neutral, 192U));

        for (uint8_t localIndex = 0; localIndex < 8U; ++localIndex) {
            const uint16_t localQ8 = static_cast<uint16_t>(localIndex << 8);
            const uint16_t leftHeadDistQ8 = (localQ8 >= leftHeadPosQ8) ? static_cast<uint16_t>(localQ8 - leftHeadPosQ8)
                                                                       : static_cast<uint16_t>(leftHeadPosQ8 - localQ8);
            const uint16_t rightHeadDistQ8 = (localQ8 >= rightHeadPosQ8) ? static_cast<uint16_t>(localQ8 - rightHeadPosQ8)
                                                                         : static_cast<uint16_t>(rightHeadPosQ8 - localQ8);
            const uint16_t leftHeadAlpha = ApplyEndFade(Kernel(leftHeadDistQ8));
            const uint16_t rightHeadAlpha = ApplyEndFade(Kernel(rightHeadDistQ8));
            uint16_t leftTrailAlpha = 0U;
            uint16_t rightTrailAlpha = 0U;

            if (leftTrailPosQ8 >= 0) {
                const uint16_t leftTrailQ8 = static_cast<uint16_t>(leftTrailPosQ8);
                const uint16_t leftTrailDistQ8 = (localQ8 >= leftTrailQ8) ? static_cast<uint16_t>(localQ8 - leftTrailQ8)
                                                                          : static_cast<uint16_t>(leftTrailQ8 - localQ8);
                leftTrailAlpha = ApplyEndFade(static_cast<uint16_t>((static_cast<uint32_t>(Kernel(leftTrailDistQ8)) * 3U + 2U) / 4U));
            }

            if (rightTrailPosQ8 >= 0) {
                const uint16_t rightTrailQ8 = static_cast<uint16_t>(rightTrailPosQ8);
                const uint16_t rightTrailDistQ8 = (localQ8 >= rightTrailQ8) ? static_cast<uint16_t>(localQ8 - rightTrailQ8)
                                                                            : static_cast<uint16_t>(rightTrailQ8 - localQ8);
                rightTrailAlpha = ApplyEndFade(static_cast<uint16_t>((static_cast<uint32_t>(Kernel(rightTrailDistQ8)) * 3U + 2U) / 4U));
            }

            if (leftHeadAlpha == 0U && rightHeadAlpha == 0U && leftTrailAlpha == 0U && rightTrailAlpha == 0U) {
                continue;
            }

            const uint8_t edgeIndex = HomeIndex(state.beginnerModeStingerOwner, localIndex);
            if (leftTrailAlpha != 0U) {
                out[edgeIndex] = modules::AddRgbSaturating(out[edgeIndex],
                                                           modules::ScaleRgbSaturatingBy256(trailColor, leftTrailAlpha));
            }
            if (rightTrailAlpha != 0U) {
                out[edgeIndex] = modules::AddRgbSaturating(out[edgeIndex],
                                                           modules::ScaleRgbSaturatingBy256(trailColor, rightTrailAlpha));
            }
            if (leftHeadAlpha != 0U) {
                out[edgeIndex] = modules::AddRgbSaturating(out[edgeIndex],
                                                           modules::ScaleRgbSaturatingBy256(headColor, leftHeadAlpha));
            }
            if (rightHeadAlpha != 0U) {
                out[edgeIndex] = modules::AddRgbSaturating(out[edgeIndex],
                                                           modules::ScaleRgbSaturatingBy256(headColor, rightHeadAlpha));
            }
        }

        if (centerAlpha != 0U) {
            const uint8_t centerLeftIndex = HomeIndex(state.beginnerModeStingerOwner, 3U);
            const uint8_t centerRightIndex = HomeIndex(state.beginnerModeStingerOwner, 4U);
            const Rgb centerOverlay = modules::ScaleRgbSaturatingBy256(centerColor, centerAlpha);
            out[centerLeftIndex] = modules::AddRgbSaturating(out[centerLeftIndex], centerOverlay);
            out[centerRightIndex] = modules::AddRgbSaturating(out[centerRightIndex], centerOverlay);
        }
    }

    void RenderTurn(const EdgeLedState& state, std::array<Rgb, EDGE_LED_COUNT>& out) const {
        if (state.turnAnimStyle == TurnAnimStyle::None) {
            return;
        }

        if (state.minimalAnimations) {
            const uint8_t base =
                HomeStart(state.activeSide);
            const Rgb color = modules::ScaleRgbSaturatingBy256(
                ColorForSide(ColorOwnerForActiveSide(state)), 192U);
            for (uint8_t i = 2U; i <= 5U; ++i) {
                out[base + i] = color;
            }
            return;
        }

        const uint32_t nowMs = ResolveNowMs(state);
        if (state.turnAnimStyle == TurnAnimStyle::SideMiddleBand) {
            RenderTurnSideMiddleBand(state, out);
            return;
        }
        if (state.turnAnimStyle == TurnAnimStyle::ActiveEndPingpongSides) {
            RenderTurnActiveEndPingpongSides(nowMs, state, out);
            return;
        }
        if (state.turnAnimStyle == TurnAnimStyle::BothEnds) {
            RenderTurnBothEnds(nowMs, state, out);
            return;
        }

        // Active-turn indicator (ActiveEndOnly): dark-to-light motion toward the active home edge.
        const Rgb c = ColorForSide(ColorOwnerForActiveSide(state));
        const bool activeHomeIsNorth = (HomeStart(state.activeSide) == 0U);
        const uint8_t homeBase = activeHomeIsNorth ? 0U : 16U;

        const uint32_t periodMs = m_timing.turnFrameMs * 20U;
        const uint32_t p = (periodMs == 0U) ? 1U : periodMs;
        const uint32_t t = state.turnAnimStyle == TurnAnimStyle::AwayFromActiveEnd
            ? (p - 1U - nowMs % p) : nowMs % p;
        static constexpr uint16_t FULL_RANGE_Q8 = static_cast<uint16_t>(7U << 8);
        static constexpr uint16_t HALF_RANGE_Q8 = 896U; // 3.5 * 256
        const uint16_t posFullQ8 = static_cast<uint16_t>((t * FULL_RANGE_Q8 + (p / 2U)) / p);

        const auto kernel = [](const uint16_t distQ8) -> uint16_t {
            // Tight triangular kernel: dist=0 => 256, dist=1.5 => 0.
            static constexpr uint16_t RADIUS_Q8 = 384U; // 1.5 * 256
            if (distQ8 >= RADIUS_Q8) {
                return 0;
            }
            const uint16_t remaining = static_cast<uint16_t>(RADIUS_Q8 - distQ8);
            return static_cast<uint16_t>((static_cast<uint32_t>(remaining) * 256U + (RADIUS_Q8 / 2U)) / RADIUS_Q8);
        };

        const auto ApplyPulseOnSideEdge = [&](const uint8_t base,
                                              const bool reverseIndex,
                                              const uint16_t headPosQ8,
                                              const uint16_t fadeScale256) {
            for (uint8_t i = 0; i < 8U; ++i) {
                const uint16_t iQ8 = static_cast<uint16_t>(i << 8);
                const uint16_t distQ8 = (iQ8 >= headPosQ8) ? static_cast<uint16_t>(iQ8 - headPosQ8)
                                                           : static_cast<uint16_t>(headPosQ8 - iQ8);
                const uint16_t kRaw = kernel(distQ8);
                const uint16_t k = static_cast<uint16_t>((static_cast<uint32_t>(kRaw) * fadeScale256 + 127U) / 256U);
                if (k == 0) {
                    continue;
                }
                const Rgb overlay = modules::ScaleRgbSaturatingBy256(c, k);
                const uint8_t edgeIndex = reverseIndex ? static_cast<uint8_t>(base + (7U - i))
                                                       : static_cast<uint8_t>(base + i);
                out[edgeIndex] = modules::AddRgbSaturating(out[edgeIndex], overlay);
            }
        };

        // Home edge: two pulses moving from the corners toward the center.
        const uint16_t headPosQ8 = activeHomeIsNorth ? static_cast<uint16_t>(FULL_RANGE_Q8 - posFullQ8) : posFullQ8;
        const uint16_t travelTowardHomeQ8 =
            activeHomeIsNorth ? static_cast<uint16_t>(FULL_RANGE_Q8 - headPosQ8) : headPosQ8;
        const uint32_t scaledHalf = static_cast<uint32_t>(travelTowardHomeQ8) * HALF_RANGE_Q8 + (FULL_RANGE_Q8 / 2U);
        const uint16_t cornerPosQ8 = static_cast<uint16_t>(scaledHalf / FULL_RANGE_Q8);
        const uint16_t leftPosQ8 = cornerPosQ8;
        const uint16_t rightPosQ8 = static_cast<uint16_t>(FULL_RANGE_Q8 - cornerPosQ8);
        const auto FadeScale256 = [](const uint16_t distQ8, const uint16_t fadeQ8) -> uint16_t {
            if (distQ8 >= fadeQ8) {
                return 256U;
            }
            return static_cast<uint16_t>((static_cast<uint32_t>(distQ8) * 256U + (fadeQ8 / 2U)) / fadeQ8);
        };
        static constexpr uint16_t HOME_CENTER_FADE_Q8 = 512U;
        static constexpr uint16_t HOME_CORNER_FADE_Q8 = 512U;
        const uint16_t distToCenterQ8 = (cornerPosQ8 >= HALF_RANGE_Q8)
                                            ? static_cast<uint16_t>(cornerPosQ8 - HALF_RANGE_Q8)
                                            : static_cast<uint16_t>(HALF_RANGE_Q8 - cornerPosQ8);
        const uint16_t distToCornerQ8 = (cornerPosQ8 >= (FULL_RANGE_Q8 / 2U))
                                            ? static_cast<uint16_t>(FULL_RANGE_Q8 - cornerPosQ8)
                                            : cornerPosQ8;
        const uint16_t homeCenterFade = FadeScale256(distToCenterQ8, HOME_CENTER_FADE_Q8);
        const uint16_t homeCornerFade = FadeScale256(distToCornerQ8, HOME_CORNER_FADE_Q8);
        const uint16_t homeFadeScale = static_cast<uint16_t>(
            (static_cast<uint32_t>(homeCenterFade) * homeCornerFade + 127U) / 256U);
        for (uint8_t i = 0; i < 8U; ++i) {
            const uint16_t iQ8 = static_cast<uint16_t>(i << 8);
            const uint16_t d0 = (iQ8 >= leftPosQ8) ? static_cast<uint16_t>(iQ8 - leftPosQ8)
                                                   : static_cast<uint16_t>(leftPosQ8 - iQ8);
            const uint16_t d1 = (iQ8 >= rightPosQ8) ? static_cast<uint16_t>(iQ8 - rightPosQ8)
                                                    : static_cast<uint16_t>(rightPosQ8 - iQ8);
            const uint16_t k0 = kernel(d0);
            const uint16_t k1 = kernel(d1);
            const uint16_t kRaw = (k0 > k1) ? k0 : k1;
            const uint16_t k = static_cast<uint16_t>((static_cast<uint32_t>(kRaw) * homeFadeScale + 127U) / 256U);
            if (k == 0) {
                continue;
            }
            const Rgb overlay = modules::ScaleRgbSaturatingBy256(c, k);
            const uint8_t idx = static_cast<uint8_t>(homeBase + i);
            out[idx] = modules::AddRgbSaturating(out[idx], overlay);
        }

        // Side edges: pulses moving toward the active home edge.
        // Logical position is 0 (north) .. 7 (south).
        static constexpr uint16_t SIDE_END_FADE_Q8 = 768U;
        const uint16_t distToEndQ8 = (posFullQ8 >= (FULL_RANGE_Q8 / 2U))
                                         ? static_cast<uint16_t>(FULL_RANGE_Q8 - posFullQ8)
                                         : posFullQ8;
        const uint16_t sideEndFade = FadeScale256(distToEndQ8, SIDE_END_FADE_Q8);
        ApplyPulseOnSideEdge(8U, false, headPosQ8, sideEndFade);
        ApplyPulseOnSideEdge(24U, true, headPosQ8, sideEndFade);
    }

    static constexpr uint16_t PulseScale256(const uint32_t nowMs,
                                            const uint32_t periodMs,
                                            const uint16_t minScale256,
                                            const uint16_t maxScale256) {
        const uint32_t p = (periodMs == 0U) ? 1U : periodMs;
        const uint32_t t = nowMs % p;
        const uint32_t half = p / 2U;
        const uint32_t up = (half == 0U) ? 0U : ((t <= half) ? t : (p - t));
        const uint32_t span = (maxScale256 > minScale256) ? (maxScale256 - minScale256) : 0U;
        const uint16_t add = (half == 0U)
                                 ? static_cast<uint16_t>(span)
                                 : static_cast<uint16_t>((up * span) / half);
        return static_cast<uint16_t>(minScale256 + add);
    }

    void RenderTurnActiveEndPingpongSides(const uint32_t nowMs,
                                          const EdgeLedState& state,
                                          std::array<Rgb, EDGE_LED_COUNT>& out) const {
        // A continuously sampled comet on the active home edge. The complete
        // out-and-back gesture lasts about 2.2 seconds at the default timing,
        // keeping the turn indicator calm and distinct from thinking/status UI.
        const uint8_t base = HomeStart(state.activeSide);

        const uint32_t stepMs = (m_timing.turnFrameMs == 0U) ? 1U : m_timing.turnFrameMs;
        const uint32_t periodMs = stepMs * 28U;
        const uint32_t halfPeriodMs = periodMs / 2U;
        // Start within the path rather than dwelling visually at an endpoint
        // when play first becomes ready.
        const uint32_t phaseOffsetMs = (stepMs * 4U) % periodMs;
        const uint32_t t =
            ((nowMs % periodMs) + phaseOffsetMs) % periodMs;
        static constexpr uint16_t FULL_RANGE_Q8 = static_cast<uint16_t>(7U << 8U);
        const uint16_t headPosQ8 = static_cast<uint16_t>(
            (t <= halfPeriodMs)
                ? ((t * FULL_RANGE_Q8 + halfPeriodMs / 2U) / halfPeriodMs)
                : (((periodMs - t) * FULL_RANGE_Q8 + halfPeriodMs / 2U) /
                   halfPeriodMs));

        const Rgb c = ColorForSide(ColorOwnerForActiveSide(state));
        static constexpr uint16_t KERNEL_RADIUS_Q8 = 384U; // 1.5 LEDs.
        static constexpr uint16_t MAX_SCALE_256 = 224U;
        for (uint8_t i = 0U; i < 8U; ++i) {
            const uint16_t ledPosQ8 = static_cast<uint16_t>(i) << 8U;
            const uint16_t distanceQ8 =
                (ledPosQ8 > headPosQ8)
                    ? static_cast<uint16_t>(ledPosQ8 - headPosQ8)
                    : static_cast<uint16_t>(headPosQ8 - ledPosQ8);
            if (distanceQ8 >= KERNEL_RADIUS_Q8) {
                continue;
            }
            const uint16_t kernelScale = static_cast<uint16_t>(
                ((KERNEL_RADIUS_Q8 - distanceQ8) * 256U +
                 KERNEL_RADIUS_Q8 / 2U) /
                KERNEL_RADIUS_Q8);
            const uint16_t scale = static_cast<uint16_t>(
                (static_cast<uint32_t>(kernelScale) * MAX_SCALE_256 + 128U) /
                256U);
            out[static_cast<uint8_t>(base + i)] =
                modules::ScaleRgbSaturatingBy256(c, scale);
        }
    }

    void RenderTurnBothEnds(const uint32_t nowMs,
                            const EdgeLedState& state,
                            std::array<Rgb, EDGE_LED_COUNT>& out) const {
        // Show the IdleTurn home-edge pulse pattern on both home edges (no side-edge motion).
        const Rgb c = ColorForSide(ColorOwnerForActiveSide(state));
        const uint32_t periodMs = m_timing.turnFrameMs * 20U;
        const uint32_t p = (periodMs == 0U) ? 1U : periodMs;
        const uint32_t t = nowMs % p;
        static constexpr uint16_t FULL_RANGE_Q8 = static_cast<uint16_t>(7U << 8);
        static constexpr uint16_t HALF_RANGE_Q8 = 896U; // 3.5 * 256
        const uint16_t posFullQ8 = static_cast<uint16_t>((t * FULL_RANGE_Q8 + (p / 2U)) / p);

        const auto kernel = [](const uint16_t distQ8) -> uint16_t {
            static constexpr uint16_t RADIUS_Q8 = 384U; // 1.5 * 256
            if (distQ8 >= RADIUS_Q8) {
                return 0;
            }
            const uint16_t remaining = static_cast<uint16_t>(RADIUS_Q8 - distQ8);
            return static_cast<uint16_t>((static_cast<uint32_t>(remaining) * 256U + (RADIUS_Q8 / 2U)) / RADIUS_Q8);
        };

        const auto FadeScale256 = [](const uint16_t distQ8, const uint16_t fadeQ8) -> uint16_t {
            if (distQ8 >= fadeQ8) {
                return 256U;
            }
            return static_cast<uint16_t>((static_cast<uint32_t>(distQ8) * 256U + (fadeQ8 / 2U)) / fadeQ8);
        };

        const auto RenderHome = [&](const uint8_t homeBase, const bool homeIsNorth) {
            const uint16_t headPosQ8 = homeIsNorth ? static_cast<uint16_t>(FULL_RANGE_Q8 - posFullQ8) : posFullQ8;
            const uint16_t travelTowardHomeQ8 =
                homeIsNorth ? static_cast<uint16_t>(FULL_RANGE_Q8 - headPosQ8) : headPosQ8;
            const uint32_t scaledHalf =
                static_cast<uint32_t>(travelTowardHomeQ8) * HALF_RANGE_Q8 + (FULL_RANGE_Q8 / 2U);
            const uint16_t cornerPosQ8 = static_cast<uint16_t>(scaledHalf / FULL_RANGE_Q8);
            const uint16_t leftPosQ8 = cornerPosQ8;
            const uint16_t rightPosQ8 = static_cast<uint16_t>(FULL_RANGE_Q8 - cornerPosQ8);

            static constexpr uint16_t HOME_CENTER_FADE_Q8 = 512U;
            static constexpr uint16_t HOME_CORNER_FADE_Q8 = 512U;
            const uint16_t distToCenterQ8 = (cornerPosQ8 >= HALF_RANGE_Q8)
                                                ? static_cast<uint16_t>(cornerPosQ8 - HALF_RANGE_Q8)
                                                : static_cast<uint16_t>(HALF_RANGE_Q8 - cornerPosQ8);
            const uint16_t distToCornerQ8 = (cornerPosQ8 >= (FULL_RANGE_Q8 / 2U))
                                                ? static_cast<uint16_t>(FULL_RANGE_Q8 - cornerPosQ8)
                                                : cornerPosQ8;
            const uint16_t homeCenterFade = FadeScale256(distToCenterQ8, HOME_CENTER_FADE_Q8);
            const uint16_t homeCornerFade = FadeScale256(distToCornerQ8, HOME_CORNER_FADE_Q8);
            const uint16_t homeFadeScale = static_cast<uint16_t>(
                (static_cast<uint32_t>(homeCenterFade) * homeCornerFade + 127U) / 256U);

            for (uint8_t i = 0; i < 8U; ++i) {
                const uint16_t iQ8 = static_cast<uint16_t>(i << 8);
                const uint16_t d0 = (iQ8 >= leftPosQ8) ? static_cast<uint16_t>(iQ8 - leftPosQ8)
                                                       : static_cast<uint16_t>(leftPosQ8 - iQ8);
                const uint16_t d1 = (iQ8 >= rightPosQ8) ? static_cast<uint16_t>(iQ8 - rightPosQ8)
                                                        : static_cast<uint16_t>(rightPosQ8 - iQ8);
                const uint16_t k0 = kernel(d0);
                const uint16_t k1 = kernel(d1);
                const uint16_t kRaw = (k0 > k1) ? k0 : k1;
                const uint16_t k =
                    static_cast<uint16_t>((static_cast<uint32_t>(kRaw) * homeFadeScale + 127U) / 256U);
                if (k == 0U) {
                    continue;
                }

                const Rgb overlay = modules::ScaleRgbSaturatingBy256(c, k);
                const uint8_t idx = static_cast<uint8_t>(homeBase + i);
                out[idx] = modules::AddRgbSaturating(out[idx], overlay);
            }
        };

        RenderHome(0U, true);
        RenderHome(16U, false);
    }

    void RenderTurnSideMiddleBand(const EdgeLedState& state,
                                  std::array<Rgb, EDGE_LED_COUNT>& out) const {
        // Side-edge band: light only the 4 middle LEDs on each side edge.
        // Side edges: East (8..15) and West (24..31).
        const Rgb c = ColorForSide(ColorOwnerForActiveSide(state));

        static constexpr uint8_t EAST_BASE = 8U;
        static constexpr uint8_t WEST_BASE = 24U;
        for (uint8_t i = 2U; i <= 5U; ++i) {
            out[static_cast<uint8_t>(EAST_BASE + i)] = c;
            out[static_cast<uint8_t>(WEST_BASE + i)] = c;
        }
    }

    void RenderFatalCheckmate(const EdgeLedState& state,
                              std::array<Rgb, EDGE_LED_COUNT>& out) const {
        // The winner owns the result edge. Brighter sparks expand from a
        // changing launch position; the other three edges remain dark.
        const UiOwner edgeOwner = state.activeSide;
        const uint8_t winnerBase = HomeStart(edgeOwner);
        const Rgb winnerColor = ColorForSide(ColorOwnerForActiveSide(state));
        const uint32_t periodMs =
            modules::animation::LED_FRAME_TOKENS.checkmateMotion.periodMs;
        const uint32_t burst = state.gameOverElapsedMs / periodMs;
        const uint32_t ageMs = state.gameOverElapsedMs % periodMs;
        const uint8_t origin = static_cast<uint8_t>(1U + (burst * 5U) % 6U);
        const auto motion = modules::animation::SamplePhase(
            {state.gameOverElapsedMs},
            modules::animation::LED_FRAME_TOKENS.checkmateMotion);
        const uint16_t travelQ8 = static_cast<uint16_t>(
            static_cast<uint32_t>(motion.progress255) * (7U << 8U) / 255U);
        constexpr uint16_t SPARK_RADIUS_Q8 = 384U;
        const Rgb sparkColor = modules::AlphaBlendRgb(winnerColor, m_palette.neutral, 96U);
        for (uint8_t i = 0U; i < 8U; ++i) {
            Rgb color = modules::ScaleRgbSaturatingBy256(
                winnerColor, state.minimalAnimations ? 208U : 96U);
            if (!state.minimalAnimations) {
                const uint16_t distanceQ8 = static_cast<uint16_t>(
                    (i > origin ? i - origin : origin - i) << 8U);
                const uint16_t sparkDistanceQ8 = distanceQ8 > travelQ8
                    ? distanceQ8 - travelQ8 : travelQ8 - distanceQ8;
                if (sparkDistanceQ8 < SPARK_RADIUS_Q8) {
                    const uint16_t scale = static_cast<uint16_t>(
                        (SPARK_RADIUS_Q8 - sparkDistanceQ8) * 256U *
                        (periodMs - ageMs) / (SPARK_RADIUS_Q8 * periodMs));
                    color = modules::AddRgbSaturating(
                        color, modules::ScaleRgbSaturatingBy256(sparkColor, scale));
                }
            }
            out[winnerBase + i] = color;
        }
    }

    void RenderFatalDraw(const EdgeLedState& state,
                         std::array<Rgb, EDGE_LED_COUNT>& out) const {
        const uint8_t tri = state.minimalAnimations
                                ? 128U
                                : modules::animation::TriangleWave255(
                                      {state.gameOverElapsedMs}, 2600U);
        const uint16_t homeScale256 = state.minimalAnimations
                                          ? 160U
                                          : static_cast<uint16_t>(
                                                96U +
                                                (static_cast<uint16_t>(tri) *
                                                 80U) /
                                                    255U);
        const Rgb p1Color = ColorForSide(UiOwner::P1);
        const Rgb p2Color = ColorForSide(UiOwner::P2);

        // Draw/stalemate: neutral ring with both players still present.
        for (uint8_t i = 0; i < EDGE_LED_COUNT; ++i) {
            out[i] = modules::ScaleRgbSaturatingBy256(m_palette.neutral, 72U);
        }

        const uint8_t p2Base = HomeStart(UiOwner::P2);
        const uint8_t p1Base = HomeStart(UiOwner::P1);
        for (uint8_t i = 0; i < 8U; ++i) {
            out[p2Base + i] = modules::AlphaBlendRgb(out[p2Base + i], p2Color, static_cast<uint8_t>(homeScale256));
            out[p1Base + i] = modules::AlphaBlendRgb(out[p1Base + i], p1Color, static_cast<uint8_t>(homeScale256));
        }
    }

    static constexpr uint8_t RingDistance(const uint8_t left,
                                          const uint8_t right) {
        const uint8_t direct = left > right
                                   ? static_cast<uint8_t>(left - right)
                                   : static_cast<uint8_t>(right - left);
        const uint8_t wrapped = static_cast<uint8_t>(EDGE_LED_COUNT - direct);
        return direct < wrapped ? direct : wrapped;
    }

    static constexpr uint8_t DistanceFromHomeCenter(
        const uint8_t index,
        const UiOwner owner) {
        const uint8_t base = HomeStart(owner);
        const uint8_t left = RingDistance(index, static_cast<uint8_t>(base + 3U));
        const uint8_t right = RingDistance(index, static_cast<uint8_t>(base + 4U));
        return left < right ? left : right;
    }

    void RenderSetupCompleteSweep(
        const EdgeLedState& state,
        std::array<Rgb, EDGE_LED_COUNT>& out) const {
        const uint32_t nowMs = ResolveNowMs(state);
        const uint32_t elapsed = nowMs >= state.effectStartMs
                                     ? nowMs - state.effectStartMs
                                     : 0U;
        static constexpr uint32_t DURATION_MS =
            EdgeLedCueController::SETUP_COMPLETE_DURATION_MS;
        static constexpr uint32_t HALF_DURATION_MS = DURATION_MS / 2U;
        const uint32_t travelElapsed =
            elapsed < HALF_DURATION_MS
                ? elapsed
                : (elapsed < DURATION_MS ? DURATION_MS - elapsed : 0U);
        static constexpr uint32_t UNIT_Q16 = 65535U;
        static constexpr uint16_t MAX_RADIUS_Q8 =
            static_cast<uint16_t>(7U << 8U);
        const uint32_t progressQ16 =
            (travelElapsed * UNIT_Q16 + (HALF_DURATION_MS / 2U)) /
            HALF_DURATION_MS;
        const uint64_t progressSquared =
            static_cast<uint64_t>(progressQ16) * progressQ16;
        const uint32_t easedQ16 = static_cast<uint32_t>(
            (progressSquared * (3U * UNIT_Q16 - 2U * progressQ16) +
             (static_cast<uint64_t>(UNIT_Q16) * UNIT_Q16 / 2U)) /
            (static_cast<uint64_t>(UNIT_Q16) * UNIT_Q16));
        const uint16_t radiusQ8 = static_cast<uint16_t>(
            (static_cast<uint32_t>(easedQ16) * MAX_RADIUS_Q8 +
             (UNIT_Q16 / 2U)) /
            UNIT_Q16);

        // Start at both player-edge centers, meet at the side-edge centers,
        // then reverse toward the players before normal Turn feedback resumes.
        // The result reads as one complete board becoming ready, without
        // drawing a board-square icon or using generic green success light.
        for (uint8_t index = 0U; index < EDGE_LED_COUNT; ++index) {
            const uint8_t p1Distance = DistanceFromHomeCenter(
                index, UiOwner::P1);
            const uint8_t p2Distance = DistanceFromHomeCenter(
                index, UiOwner::P2);
            const uint8_t nearest = p1Distance < p2Distance
                                        ? p1Distance
                                        : p2Distance;
            uint16_t alpha256 = 0U;
            if (nearest == 0U) {
                alpha256 = 256U;
            } else {
                const uint16_t thresholdQ8 =
                    static_cast<uint16_t>((nearest - 1U) << 8U);
                if (radiusQ8 > thresholdQ8) {
                    const uint16_t coverage = static_cast<uint16_t>(
                        radiusQ8 - thresholdQ8);
                    alpha256 = coverage > 256U ? 256U : coverage;
                }
            }
            if (alpha256 == 0U) {
                continue;
            }
            const Rgb color =
                p1Distance < p2Distance
                    ? m_palette.p1
                    : (p2Distance < p1Distance
                           ? m_palette.p2
                           : AlphaBlendRgb(
                                 m_palette.p1, m_palette.p2, 128U));
            out[index] =
                modules::ScaleRgbSaturatingBy256(color, alpha256);
        }
    }

    void RenderUnavailableNudge(
        const EdgeLedState& state,
        std::array<Rgb, EDGE_LED_COUNT>& out) const {
        static constexpr uint32_t RISE_MS = 120U;
        static constexpr uint32_t DURATION_MS =
            EdgeLedCueController::UNAVAILABLE_DURATION_MS;
        const uint32_t elapsed =
            static_cast<uint32_t>(ResolveNowMs(state) - state.effectStartMs);
        if (elapsed >= DURATION_MS) {
            return;
        }

        const uint16_t scale256 = elapsed < RISE_MS
                                      ? static_cast<uint16_t>(
                                            64U + (elapsed * 96U) / RISE_MS)
                                      : static_cast<uint16_t>(
                                            ((DURATION_MS - elapsed) * 160U) /
                                            (DURATION_MS - RISE_MS));
        const Rgb outer = ScaleRgbSaturatingBy256(
            m_palette.neutral, scale256);
        const Rgb inner = ScaleRgbSaturatingBy256(
            m_palette.neutral,
            static_cast<uint16_t>(scale256 / 2U));
        const UiOwner owner = state.activeSide == UiOwner::None
                                  ? UiOwner::P1
                                  : state.activeSide;
        out[HomeIndex(owner, 0U)] = outer;
        out[HomeIndex(owner, 7U)] = outer;
        out[HomeIndex(owner, 1U)] = inner;
        out[HomeIndex(owner, 6U)] = inner;
    }

    void RenderOccupiedBoardWarning(
        const EdgeLedState& state,
        std::array<Rgb, EDGE_LED_COUNT>& out) const {
        static constexpr Rgb AMBER{255U, 132U, 0U};
        static constexpr uint32_t PULSE_MS = 200U;
        const uint32_t elapsed = static_cast<uint32_t>(
            ResolveNowMs(state) - state.effectStartMs);
        if (elapsed >=
            EdgeLedCueController::OCCUPIED_BOARD_BLOCKED_DURATION_MS) {
            return;
        }
        const bool bright = ((elapsed / PULSE_MS) % 2U) == 0U;
        const uint16_t scale256 = bright ? 176U : 72U;
        const Rgb color = ScaleRgbSaturatingBy256(AMBER, scale256);
        for (size_t index = 0U; index < out.size(); ++index) {
            out[index] = color;
        }
    }

    void RenderError(const EdgeLedState& state, std::array<Rgb, EDGE_LED_COUNT>& out) const {
        const uint32_t halfCycleMs = ResolveFaultPeriodMs(state);
        const animation::AnimationContext anim{ResolveFaultElapsedMs(state) + halfCycleMs};
        animation::ValueToken token;
        token.phase.periodMs = halfCycleMs * 2U;
        token.phase.playback = animation::PlaybackMode::Loop;
        token.curve.waveform = animation::Waveform::Triangle;
        token.curve.minValue = 0U;
        token.curve.maxValue = 255U;
        const uint8_t alpha = animation::SampleValue(anim, token);
        if (alpha == 0U) {
            return;
        }

        for (uint8_t i = 0; i < EDGE_LED_COUNT; ++i) {
            out[i] = animation::BlendRgb(m_palette.off, m_palette.error, alpha);
        }
    }

    void RenderHealthPaused(
        const EdgeLedState& state,
        std::array<Rgb, EDGE_LED_COUNT>& out) const {
        const uint32_t nowMs = ResolveNowMs(state);
        const uint16_t leading = PulseScale256(nowMs, 1600U, 80U, 176U);
        const uint16_t trailing = PulseScale256(
            nowMs + 800U, 1600U, 80U, 176U);
        const Rgb leadingColor =
            modules::ScaleRgbSaturatingBy256(m_palette.prompt, leading);
        const Rgb trailingColor =
            modules::ScaleRgbSaturatingBy256(m_palette.prompt, trailing);

        // Eight fixed two-pixel dashes remain spatially distinct through the
        // least-salient cadence phase; the alternating breath supplies the
        // measured warning channel without resembling a flashing fatal error.
        for (uint8_t i = 0U; i < EDGE_LED_COUNT; i += 4U) {
            out[i] = leadingColor;
            out[static_cast<uint8_t>(i + 1U)] = trailingColor;
        }
    }

    void RenderHealthDegradedUsable(
        const EdgeLedState& state,
        std::array<Rgb, EDGE_LED_COUNT>& out) const {
        constexpr uint32_t PERIOD_MS = 5000U;
        constexpr uint32_t ACTIVE_MS = 1000U;
        constexpr uint32_t STEP_MS = 250U;
        const uint32_t phase = ResolveNowMs(state) % PERIOD_MS;
        if (phase >= ACTIVE_MS) {
            return;
        }

        const uint8_t step = static_cast<uint8_t>(phase / STEP_MS);
        const uint8_t physicalIndex =
            status_led_frame_adapter::USB_PORT_PHYSICAL_INDICES[step % 2U];
        out[physicalIndex] = step % 2U == 0U ? RGB_YELLOW : RGB_ORANGE;
    }

    void RenderMenu(const EdgeLedState& state,
                    std::array<Rgb, EDGE_LED_COUNT>& out) const {
        if (state.owner == UiOwner::None) {
            return;
        }
        const UiOwner edgeOwner = (state.menuEdgeOwner != UiOwner::None) ? state.menuEdgeOwner : state.owner;

        // For destructive actions (e.g. new game), show an explicit progress fill
        // across all edge LEDs to reduce accidental activation.
        if (state.menuValueIsHoldProgress && state.menuHoldFillAll) {
            const uint16_t scaled = static_cast<uint16_t>(state.menuValue01_255) * EDGE_LED_COUNT;
            const uint8_t fill = static_cast<uint8_t>((scaled + 127U) / 255U);
            const uint8_t clampedFill = (fill > EDGE_LED_COUNT) ? EDGE_LED_COUNT : fill;
            for (uint8_t i = 0; i < clampedFill; ++i) {
                out[i] = m_palette.confirm;
            }

            // Fill-all progress is a dedicated display mode.
            return;
        }

        const Rgb c = ColorForSide(state.owner);
        for (uint8_t i = 0; i < 8; ++i) {
            const uint8_t idx = HomeIndex(edgeOwner, i);
            out[idx] = {static_cast<uint8_t>(c.r / 4), static_cast<uint8_t>(c.g / 4),
                        static_cast<uint8_t>(c.b / 4)};
        }

        // Non-destructive holds preview their eventual mode on the owner's
        // home edge. Immediate dim light acknowledges the press; the bright
        // player-color fill makes the one-second hold discoverable without
        // implying a destructive confirmation or depending on the app.
        if (state.menuValueIsHoldProgress) {
            const uint16_t scaled =
                static_cast<uint16_t>(state.menuValue01_255) * 8U;
            const uint8_t fill = static_cast<uint8_t>((scaled + 254U) / 255U);
            const uint8_t clampedFill = (fill > 8U) ? 8U : fill;
            for (uint8_t i = 0; i < clampedFill; ++i) {
                out[HomeIndex(edgeOwner, i)] = c;
            }
            return;
        }

        // Selected option indicator:
        // - Option indices 0..7 map to the owner's bottom edge LEDs left->right.
        // - Higher indices continue up the owner's right edge, bottom->top.
        const uint8_t optionIndex = state.menuSelectionIndex;
        uint8_t selLed = HomeIndex(edgeOwner, 0);

        if (optionIndex < 8) {
            // Map option 0..7 to left..right on the edge where the menu is displayed.
            selLed = HomeIndex(edgeOwner, optionIndex);
        } else {
            // Right edge (from the owner's perspective):
            // - P1: East edge (8..15), start at bottom-right corner => 15 and move up => 14..8.
            // - P2: West edge (24..31), start at bottom-right corner => 31 and move up => 30..24.
            const uint8_t offset = static_cast<uint8_t>(optionIndex - 8);
            const uint8_t clampedOffset = (offset > 7U) ? 7U : offset;
            const uint8_t rightStart = (edgeOwner == UiOwner::P2) ? 31U : 15U;
            selLed = static_cast<uint8_t>(rightStart - clampedOffset);
        }

        if (selLed < EDGE_LED_COUNT) {
            out[selLed] = m_palette.neutral;
        }
    }

    void RenderBrightness(const EdgeLedState& state,
                          std::array<Rgb, EDGE_LED_COUNT>& out) const {
        if (state.owner == UiOwner::None) {
            return;
        }

        // Spec parity: brightness preview uses the East edge (8..15) regardless
        // of which player adjusted the setting.
        static constexpr uint8_t EAST_EDGE_BASE = 8;
        static constexpr uint8_t EAST_EDGE_COUNT = 8;

        const uint8_t fill = state.value01_8 > 8 ? 8 : state.value01_8;
        for (uint8_t i = 0; i < fill; ++i) {
            out[EAST_EDGE_BASE + i] = m_palette.neutral;
        }

        // Small accent on the top LED, using the owner's side color.
        if (fill != 0) {
            const Rgb c = ColorForSide(state.owner);
            out[EAST_EDGE_BASE + (EAST_EDGE_COUNT - 1)] = {static_cast<uint8_t>(c.r / 2),
                                                           static_cast<uint8_t>(c.g / 2),
                                                           static_cast<uint8_t>(c.b / 2)};
        }
    }

    const EdgeLedPalette& m_palette;
    const EdgeLedTiming& m_timing;
};

} // namespace

EdgeLedRenderer::EdgeLedRenderer(const EdgeLedPalette& palette,
                                 const EdgeLedTiming& timing)
    : m_palette(palette), m_timing(timing) {}

void EdgeLedRenderer::Render(
    const EdgeLedState& state,
    std::array<Rgb, EDGE_LED_COUNT>& out) const {
    EdgeLedRendererEngine(m_palette, m_timing).Render(state, out);
}

} // namespace modules
