#include "modules/litchcraft_presentation.hpp"

namespace modules::litchcraft_presentation {

void AdvanceClock(Playback& playback, const uint32_t now, const bool suspended) {
    const uint32_t elapsed = now - playback.clockMs;
    if (suspended || playback.suspended) {
        // Shift every authored anchor; paused wall time never becomes catch-up.
        if (playback.frameStarted) playback.frameStartedAtMs += elapsed;
        if (!playback.playing) playback.pausedAtMs += elapsed;
        if (playback.corruptDwell) playback.corruptStartedAtMs += elapsed;
    } else {
        playback.effectElapsedMs += elapsed;
        if (playback.playing) playback.elapsedMs += elapsed;
    }
    playback.clockMs = now;
    playback.suspended = suspended;
}

BootTransition AdvanceStreamed(Playback& playback, const Context& context,
    const StreamPort& port, const uint32_t now,
    litchcraft_frame_store::Frame* const workspace,
    litchcraft_frame_stream::FrameStream::PresentationToken* const token) {
    if (port.stream == nullptr || port.stream->FrameCount() == 0U ||
        port.stream->DocumentEpoch() != context.documentEpoch)
        return BootTransition::None;
    auto& stream = *port.stream;
    const uint32_t presentationEpoch = token != nullptr ? token->contextEpoch : 0U;
    const auto wake = [&]() {
        if (port.wake != nullptr) port.wake(port.context, playback.playing,
            context.session, context.active, (playback.bootFlags & 1U) != 0U);
    };
    const auto presentRequested = [&]() {
        if (!port.confirmedOutput) return stream.TryPresentRequested();
        if (workspace == nullptr || token == nullptr) return false;
        auto candidate = *token;
        candidate.frameIndex = stream.RequestedIndex();
        if (!stream.PreparePresentation(*workspace, presentationEpoch, candidate))
            return false;
        *token = candidate;
        return true;
    };
    const auto copyDisplayed = [&](uint16_t& index, uint16_t& duration) {
        if (port.confirmedOutput && token != nullptr && workspace != nullptr &&
            token->lease != 0U) {
            index = token->frameIndex;
            duration = workspace->durationMs;
            return true;
        }
        std::array<litchcraft_frame_store::Emitter, 4U> square{};
        return stream.CopyDisplayedSquare(0U, square, index, duration);
    };
    // Pin confirmed A before any successor demand. Main control admission
    // cannot race the task's reads or retarget an old document between them.
    if (port.confirmedOutput) {
        if (workspace == nullptr || token == nullptr) return BootTransition::None;
        token->frameIndex = stream.DisplayedIndex();
        if (token->frameIndex == UINT16_MAX) token->frameIndex = stream.RequestedIndex();
        if (!stream.PreparePresentation(*workspace, presentationEpoch, *token)) {
            // A bad first page has no previously displayed frame to lease.
            // Let the boot display release the board on the next confirmed output.
            if ((playback.bootFlags & 1U) != 0U && stream.RequestedFrameFailed())
                return BootTransition::Cancelled;
            wake();
            return BootTransition::None;
        }
    }
    const auto request = [&](const uint16_t index) {
        return port.confirmedOutput ? stream.RequestPresentation(*token, index)
            : stream.Request(index);
    };
    AdvanceClock(playback, now, context.transferSuspended);

    if ((!playback.playing || playback.suspended) && playback.frameStarted &&
        playback.awaitingPlaybackFrame) {
        // A late successor stays staged while the displayed frame is paused.
        // Play preserves it and shifts its authored boundary by the pause;
        // an explicit seek clears this playback wait and presents its target.
        return BootTransition::None;
    }

    if (presentRequested()) {
        uint16_t displayed = 0U;
        uint16_t duration = 0U;
        if (copyDisplayed(displayed, duration)) {
            const uint8_t previous = playback.selectedFrame;
            playback.selectedFrame = static_cast<uint8_t>(displayed);
            if (!playback.frameStarted) {
                // A fresh initial/seek/history anchor retires any older
                // playback wait before it can label this frame as expired.
                playback.awaitingPlaybackFrame = false;
                playback.frameStartedAtMs = now;
                playback.frameStarted = true;
                if (!playback.playing) playback.pausedAtMs = now;
            } else if (previous != playback.selectedFrame) {
                // Normal playback advances the authored schedule, not the
                // render sampling instant. A delayed decode therefore catches
                // up instead of permanently adding its delay to every later
                // frame. Explicit seeks still start a fresh local boundary.
                if (playback.awaitingPlaybackFrame) {
                    playback.awaitingPlaybackFrame = false;
                } else {
                    playback.frameStartedAtMs = now;
                    if (!playback.playing) playback.pausedAtMs = now;
                }
            }
            if (((playback.bootFlags & 1U) != 0U) && previous != playback.selectedFrame)
                playback.bootFlags |= 8U;
            playback.awaitingInitialFrame = false;
            playback.awaitingRecoveryFrame = false;
            playback.corruptDwell = false;
        }
    } else {
        if (((playback.bootFlags & 1U) != 0U) && (stream.RequestedFrameCorrupt() ||
            stream.RequestedFrameFailed())) {
            return BootTransition::Cancelled;
        }
        wake();
        if (playback.awaitingRecoveryFrame) {
            if (stream.RequestedFrameCorrupt()) {
                uint16_t displayed = 0U;
                uint16_t duration = 0U;
                if (copyDisplayed(displayed, duration) && duration != 0U) {
                    if (!playback.corruptDwell) {
                        playback.corruptDwell = true;
                        playback.corruptStartedAtMs = now;
                    } else if (static_cast<uint32_t>(
                                   now - playback.corruptStartedAtMs) >=
                               duration) {
                        const uint16_t following = static_cast<uint16_t>(
                            (stream.RequestedIndex() + 1U) %
                            stream.FrameCount());
                        (void)request(following);
                        playback.frameStartedAtMs = now;
                        playback.corruptDwell = false;
                        wake();
                    }
                }
            }
            return BootTransition::None;
        }
    }
    if (!playback.playing || playback.suspended || !playback.frameStarted ||
        (((playback.bootFlags & 1U) != 0U) && (playback.bootFlags & 8U) != 0U)) return BootTransition::None;

    for (uint8_t catchup = 0U; catchup < 8U; ++catchup) {
        uint16_t displayed = 0U;
        uint16_t duration = 0U;
        if (!copyDisplayed(displayed, duration) || duration == 0U ||
            static_cast<uint32_t>(now - playback.frameStartedAtMs) < duration) {
            break;
        }
        const uint16_t count = stream.FrameCount();
        if (((playback.bootFlags & 1U) != 0U) && displayed + 1U >= count) {
            return BootTransition::Finished;
        }
        const uint16_t next = static_cast<uint16_t>((displayed + 1U) % count);
        (void)request(next);
        if (!presentRequested()) {
            if (stream.RequestedFrameCorrupt()) {
                if (!playback.corruptDwell) {
                    playback.corruptDwell = true;
                    playback.corruptStartedAtMs = now;
                } else if (static_cast<uint32_t>(
                               now - playback.corruptStartedAtMs) >= duration) {
                    const uint16_t following =
                        static_cast<uint16_t>((next + 1U) % count);
                    (void)request(following);
                    // Consume the corrupt frame's dwell exactly once. The
                    // production coordinator may sample more than once at the
                    // same timestamp; anchoring here prevents a later sample
                    // from re-requesting the corrupt slot before `following`
                    // finishes loading.
                    playback.frameStartedAtMs = now;
                    playback.corruptDwell = false;
                    playback.awaitingRecoveryFrame = true;
                    wake();
                }
            } else {
                // A healthy pending page owns this authored boundary until it
                // presents. Sampling the held frame again must not consume
                // its dwell repeatedly and move the playback schedule.
                if (!playback.awaitingPlaybackFrame) {
                    playback.frameStartedAtMs += duration;
                    playback.awaitingPlaybackFrame = true;
                }
                wake();
            }
            break;
        }
        playback.frameStartedAtMs += duration;
        playback.corruptDwell = false;
        playback.selectedFrame = static_cast<uint8_t>(next);
        if (((playback.bootFlags & 1U) != 0U)) {
            playback.bootFlags |= 8U;
            wake();
            return BootTransition::None; // every boot frame receives its complete visible dwell
        }
        wake();
    }
    return BootTransition::None;
}

} // namespace modules::litchcraft_presentation
