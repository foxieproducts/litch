#include "modules/litchcraft_presentation.hpp"

#include <algorithm>
#include <tuple>

#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_animation.hpp"
#include "modules/litchcraft_session.hpp"
#include "modules/ui_color_wheel.hpp"

#if defined(LITCH_NATIVE)
namespace {
void (*litchcraftComposeSquareCheckpoint)(uint8_t) = nullptr;
}
// Native tests stop a real Compose between squares while its worker publishes.
// The setter and checkpoint compile out of firmware targets.
extern "C" void LitchcraftSetComposeSquareCheckpointForTest(
    void (*checkpoint)(uint8_t)) {
    litchcraftComposeSquareCheckpoint = checkpoint;
}
#endif

namespace modules::litchcraft_presentation {
namespace {
bool Same(const Rgb a, const Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
constexpr std::array<uint8_t, 8> COLOR_PREVIEW_LEDS{{0U, 7U, 8U, 15U, 16U, 23U, 24U, 31U}};
constexpr std::array<uint8_t, 4> ERASE_MARKER_A{{0U, 31U, 15U, 16U}};
constexpr std::array<uint8_t, 4> ERASE_MARKER_B{{7U, 8U, 23U, 24U}};
constexpr uint16_t COLOR_PREVIEW_SCALE256 = 48U;
constexpr Rgb ERASE_CUE_COLOR{18U, 18U, 18U};
constexpr Rgb SHAKE_ERASE_DIM{4U, 12U, 18U};
} // namespace

bool SameContext(const Context& a, const Context& b) {
    return std::tie(a.session, a.modeGeneration, a.documentEpoch,
        a.contentRevision, a.intentEpoch, a.active, a.transferSuspended, a.transferEpoch) ==
        std::tie(b.session, b.modeGeneration, b.documentEpoch,
            b.contentRevision, b.intentEpoch, b.active, b.transferSuspended, b.transferEpoch);
}

bool SamePlayback(const Playback& a, const Playback& b) {
    return std::tie(a.elapsedMs, a.effectElapsedMs, a.clockMs, a.frameStartedAtMs,
        a.pausedAtMs, a.corruptStartedAtMs, a.selectedFrame, a.bootFlags,
        a.bootResumeFrame, a.playing, a.frameStarted, a.corruptDwell,
        a.awaitingInitialFrame, a.awaitingPlaybackFrame, a.awaitingRecoveryFrame, a.suspended) ==
        std::tie(b.elapsedMs, b.effectElapsedMs, b.clockMs, b.frameStartedAtMs,
            b.pausedAtMs, b.corruptStartedAtMs, b.selectedFrame, b.bootFlags,
            b.bootResumeFrame, b.playing, b.frameStarted, b.corruptDwell,
            b.awaitingInitialFrame, b.awaitingPlaybackFrame, b.awaitingRecoveryFrame, b.suspended);
}

bool SameVisuals(const Visuals& a, const Visuals& b) {
    if (!Same(a.color, b.color)) return false;
    for (size_t i = 0U; i < a.quickPalette.size(); ++i)
        if (!Same(a.quickPalette[i], b.quickPalette[i])) return false;
    for (size_t i = 0U; i < a.pickBefore.size(); ++i)
        if (!Same(a.pickBefore[i], b.pickBefore[i])) return false;
    return std::tie(a.blockedOccupancy, a.quickPaletteSquares, a.sparkleSeed,
        a.playbackStatusChangedAtMs, a.quickPaletteTouchedMs, a.quickPaletteFadeAtMs,
        a.pickStartedMs, a.eraseSelectedMs, a.shakeEraseAtMs, a.entryHoldStartedMs,
        a.playbackStatusFrom256, a.source, a.hardware, a.pickSquare,
        a.quickPaletteIndex, a.active, a.erasing, a.eraseCueStarted,
        a.quickPaletteVisible, a.quickPaletteFading, a.pickPending,
        a.shakeEraseActive, a.exitHoldActive) ==
        std::tie(b.blockedOccupancy, b.quickPaletteSquares, b.sparkleSeed,
            b.playbackStatusChangedAtMs, b.quickPaletteTouchedMs, b.quickPaletteFadeAtMs,
            b.pickStartedMs, b.eraseSelectedMs, b.shakeEraseAtMs, b.entryHoldStartedMs,
            b.playbackStatusFrom256, b.source, b.hardware, b.pickSquare,
            b.quickPaletteIndex, b.active, b.erasing, b.eraseCueStarted,
            b.quickPaletteVisible, b.quickPaletteFading, b.pickPending,
            b.shakeEraseActive, b.exitHoldActive);
}

void ComposeEntryStatus(const uint8_t hardware, const uint32_t elapsed,
    std::array<Rgb, EDGE_LED_COUNT>& status) {
    if (hardware == 1U) return;
    status.fill(RGB_BLACK);
    const size_t count = elapsed >= LitchcraftSession::ENTRY_HOLD_MS ? status.size() :
        1U + elapsed * (status.size() - 1U) / LitchcraftSession::ENTRY_HOLD_MS;
    for (size_t index = 0U; index < count; ++index)
        status[index] = ScaleRgbSaturatingBy256(
            ui_color_wheel::WheelToRgb(static_cast<uint8_t>(index * 8U)), 96U);
}

uint8_t ColorPickBlendAlpha(const uint32_t startedAtMs, const uint32_t nowMs) {
    const uint32_t held = nowMs - startedAtMs;
    return held <= LitchcraftSession::PICK_ARM_MS ? 0U : static_cast<uint8_t>(
        std::min(held - LitchcraftSession::PICK_ARM_MS,
            LitchcraftSession::PICK_PROGRESS_MS) * 255U / LitchcraftSession::PICK_PROGRESS_MS);
}

uint16_t PlaybackStatusScale256(const bool playing, const uint16_t from256,
    const uint32_t elapsedMs) {
    const uint32_t duration = playing ? 500U : 250U;
    const int32_t target = playing ? 0 : 256;
    return elapsedMs >= duration ? static_cast<uint16_t>(target) :
        static_cast<uint16_t>(from256 + (target - from256) *
            static_cast<int32_t>(elapsedMs) / static_cast<int32_t>(duration));
}

Feedback Renderer::Compose(Inputs& working, const StreamPort& port,
    BoardLedFrame& board, std::array<Rgb, EDGE_LED_COUNT>& status) {
    const uint32_t now = fxe::Millis();
    if (!m_initialized || m_source != working.source ||
        m_context.session != working.context.session ||
        m_context.modeGeneration != working.context.modeGeneration ||
        m_context.documentEpoch != working.context.documentEpoch ||
        m_context.intentEpoch != working.context.intentEpoch) {
        m_adoptInput = true;
        m_bootTransition = BootTransition::None;
        m_initialized = true;
    }
    m_context = working.context;
    m_source = working.source;
    Feedback feedback{};
    feedback.context = working.context;
    feedback.token.contextEpoch = working.presentationEpoch;
    feedback.playback = m_adoptInput ? working.playback : m_playback;
    feedback.bootTransition = m_bootTransition;
    if (working.source == Source::Stream && port.stream != nullptr &&
        port.stream->DocumentEpoch() == working.context.documentEpoch) {
        // Boot is sampled before native presentation-priority selection. Its
        // independent owner instead samples and confirms the actual Show here.
        if (port.confirmedOutput || (feedback.playback.bootFlags & 1U) == 0U) {
            const auto transition = AdvanceStreamed(feedback.playback,
                working.context, port, now, &working.frame, &feedback.token);
            if (transition != BootTransition::None) feedback.bootTransition = transition;
        }
        if (port.confirmedOutput) {
            if (feedback.token.lease == 0U) {
                feedback.token.frameIndex = port.stream->DisplayedIndex();
                (void)port.stream->PreparePresentation(working.frame,
                    working.presentationEpoch, feedback.token);
            }
            feedback.ready = feedback.token.lease != 0U;
            if (feedback.ready && feedback.token.frameIndex != feedback.playback.selectedFrame) {
                // An explicit seek is still paging. Show confirmed A without
                // acknowledging B or consuming its fresh control origin.
                feedback.playback = m_playback;
                AdvanceClock(feedback.playback, now, working.context.transferSuspended);
                feedback.intentApplied = false;
                feedback.bootTransition = BootTransition::None;
            }
        } else {
            uint16_t index = 0U;
            feedback.ready = port.stream->CopyDisplayed(working.frame, index) &&
                (!feedback.playback.awaitingInitialFrame ||
                    index == port.stream->RequestedIndex());
        }
    } else if (working.source == Source::Static || working.source == Source::Legacy) {
        AdvanceClock(feedback.playback, now, working.context.transferSuspended);
        feedback.ready = true;
    }
    for (uint8_t square = 0U; square < 64U; ++square) {
        if (feedback.ready) {
            std::array<litchcraft_frame_store::Emitter, 4U> stored{};
            std::copy_n(working.frame.emitters.data() + square * 4U,
                stored.size(), stored.data());
            const auto colors = litchcraft_animation::ComposeStoredSquare(stored,
                feedback.playback.effectElapsedMs, working.sparkleSeed,
                feedback.playback.selectedFrame, static_cast<uint16_t>(square * 4U));
            board[square] = {colors[0], colors[1], colors[2], colors[3]};
        } else {
            board[square] = {RGB_BLACK, RGB_BLACK, RGB_BLACK, RGB_BLACK};
        }
#if defined(LITCH_NATIVE)
        if (working.source == Source::Stream && litchcraftComposeSquareCheckpoint != nullptr)
            litchcraftComposeSquareCheckpoint(square);
#endif
        if ((working.blockedOccupancy & (UINT64_C(1) << square)) != 0U)
            ApplyToBoardSquare(board[square], BoardLedSlots::All, {72U, 12U, 0U});
    }
    if (working.shakeEraseActive) {
        const uint32_t elapsed = now - working.shakeEraseAtMs;
        if (elapsed < LitchcraftSession::SHAKE_ERASE_ANIMATION_MS) {
            const uint8_t phase = static_cast<uint8_t>(
                std::min<uint32_t>(3U, elapsed * 4U /
                                           LitchcraftSession::SHAKE_ERASE_ANIMATION_MS));
            for (uint8_t square = 0U; square < 64U; ++square) {
                const uint8_t row = square / 8U;
                const uint8_t column = square % 8U;
                const uint8_t ring = std::min(
                    {row, column, static_cast<uint8_t>(7U - row),
                     static_cast<uint8_t>(7U - column)});
                Rgb color = RGB_BLACK;
                if (ring == phase) {
                    color = ScaleRgbSaturatingBy256(
                        ui_color_wheel::WheelToRgb(
                            static_cast<uint8_t>(square * 11U + phase * 47U)),
                        112U);
                } else if (ring > phase) {
                    color = SHAKE_ERASE_DIM;
                }
                ApplyToBoardSquare(board[square], BoardLedSlots::All, color);
            }
        }
    }
    const uint32_t fadeElapsed = now - working.quickPaletteFadeAtMs;
    if (working.quickPaletteVisible ||
        (working.quickPaletteFading && fadeElapsed < LitchcraftSession::QUICK_PALETTE_FADE_MS)) {
        const uint32_t elapsed = now - working.quickPaletteTouchedMs;
        const uint32_t pulseElapsed = std::min<uint32_t>(elapsed, 250U);
        for (uint8_t slot = 0U; slot < 8U; ++slot) {
            const uint8_t index = slot;
            const uint16_t scale = index == working.quickPaletteIndex
                ? static_cast<uint16_t>(256U - (80U * pulseElapsed) / 250U)
                : 96U;
            const Rgb preview = ScaleRgbSaturatingBy256(
                working.quickPalette[index], scale);
            auto& square = board[working.quickPaletteSquares[slot]];
            const uint8_t alpha = working.quickPaletteVisible ? 255U :
                static_cast<uint8_t>(255U - fadeElapsed * 255U / LitchcraftSession::QUICK_PALETTE_FADE_MS);
            for (Rgb* emitter : {&square.left, &square.top, &square.right, &square.bottom}) {
                modules::animation::ApplyBlend(*emitter, preview, alpha,
                    modules::animation::BlendMode::AlphaBlend);
            }
        }
    }
    if (working.pickPending && working.source != Source::WaitingForStore) {
        const uint8_t alpha = ColorPickBlendAlpha(working.pickStartedMs, now);
        auto& square = board[working.pickSquare];
        square.left = animation::BlendRgb(square.left, working.pickBefore[0], alpha);
        square.top = animation::BlendRgb(square.top, working.pickBefore[1], alpha);
        square.right = animation::BlendRgb(square.right, working.pickBefore[2], alpha);
        square.bottom = animation::BlendRgb(square.bottom, working.pickBefore[3], alpha);
    }
    status.fill(RGB_BLACK);
    // No ordinary status on V1: its virtual status LEDs overlap artwork.
    if (working.hardware != 1U && working.erasing) {
        const uint32_t elapsed = working.eraseCueStarted
            ? (static_cast<int32_t>(now - working.eraseSelectedMs) >= 0
                   ? now - working.eraseSelectedMs
                   : 0U)
            : LitchcraftSession::ERASE_WIPE_MS;
        if (elapsed < LitchcraftSession::ERASE_WIPE_MS) {
            constexpr size_t WIPE_STEPS = 4U;
            const size_t step = std::min<size_t>(
                (elapsed * WIPE_STEPS) / LitchcraftSession::ERASE_WIPE_MS,
                WIPE_STEPS - 1U);
            for (size_t side = 0U; side < 4U; ++side) {
                const size_t first = side * 8U + step;
                status[first] = ERASE_CUE_COLOR;
                status[side * 8U + 7U - step] = ERASE_CUE_COLOR;
            }
        } else {
            const bool secondPhase =
                ((elapsed - LitchcraftSession::ERASE_WIPE_MS) / LitchcraftSession::ERASE_MARKER_PHASE_MS) % 2U != 0U;
            const auto& markers = secondPhase ? ERASE_MARKER_B : ERASE_MARKER_A;
            for (const uint8_t led : markers) {
                status[led] = ERASE_CUE_COLOR;
            }
        }
    } else if (working.hardware != 1U) {
        Rgb preview = ScaleRgbSaturatingBy256(
            working.color, COLOR_PREVIEW_SCALE256);
        // Black/off is a valid sampled color but cannot identify an otherwise
        // blank board as an active Litchcraft canvas. Keep the tool value exact
        // and use a dim neutral corner marker only for its non-emitting preview.
        if (Same(preview, RGB_BLACK)) {
            preview = {12U, 12U, 12U};
        }
        for (const uint8_t led : COLOR_PREVIEW_LEDS) {
            status[led] = preview;
        }
    }
    if (!working.erasing) {
        const uint16_t playbackScale = PlaybackStatusScale256(feedback.playback.playing,
            working.playbackStatusFrom256, now - working.playbackStatusChangedAtMs);
        for (auto& color : status) {
            color = ScaleRgbSaturatingBy256(color, playbackScale);
        }
    }
    if (working.source == Source::Stream && port.stream != nullptr &&
        port.stream->RequestedFrameCorrupt()) {
        status.fill({48U, 42U, 0U});
    }
    if (working.exitHoldActive)
        ComposeEntryStatus(working.hardware, now - working.entryHoldStartedMs, status);
    return feedback;
}

bool Renderer::NotePresented(Feedback& feedback, const StreamPort& port) {
    if (!feedback.ready || !SameContext(feedback.context, m_context) ||
        m_sequence == UINT32_MAX) return false;
    if (port.confirmedOutput && m_source == Source::Stream &&
        (port.stream == nullptr || !port.stream->ConfirmPresented(feedback.token)))
        return false;
    if (port.confirmedOutput && (feedback.playback.bootFlags & 9U) == 9U &&
        feedback.playback.frameStarted && !feedback.playback.awaitingInitialFrame &&
        feedback.bootTransition == BootTransition::None) {
        feedback.playback.frameStartedAtMs = fxe::Millis();
        feedback.playback.bootFlags &= static_cast<uint8_t>(~8U);
    }
    feedback.sequence = ++m_sequence;
    m_playback = feedback.playback;
    m_adoptInput = !feedback.intentApplied;
    m_bootTransition = feedback.bootTransition;
    if (feedback.bootTransition != BootTransition::None) {
        m_playback.bootFlags = 2U;
        m_playback.playing = false;
    }
    return true;
}

void Renderer::Cancel(const Feedback& feedback, const StreamPort& port) {
    if (port.confirmedOutput && port.stream != nullptr && feedback.token.lease != 0U)
        (void)port.stream->CancelPresentation(feedback.token);
}

void Renderer::Reset(const StreamPort& port) {
    if (port.confirmedOutput && port.stream != nullptr)
        port.stream->RetirePresentationContext();
    m_initialized = false;
}

} // namespace modules::litchcraft_presentation
