#include "modules/litchcraft_session.hpp"

#include <algorithm>
#include <new>

#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_persistence.hpp"

namespace modules {
uint16_t LitchcraftSession::FrameCount() const {
    if (HasStreamedDocument()) return m_frameStream->FrameCount();
    if (StoreIsSoleArtworkOwner()) return 0U;
    return HasAdvancedDocument()
        ? m_advancedPersistence->AdvancedDocument().Value().frameCount
        : 1U;
}

uint16_t LitchcraftSession::AnimationFrameElapsedMs() const {
    if (HasStreamedDocument()) {
        if (!m_streamFrameStarted) return 0U;
        std::array<litchcraft_frame_store::Emitter, 4U> square{};
        uint16_t displayed = 0U;
        uint16_t duration = 0U;
        if (!m_frameStream->CopyDisplayedSquare(
                0U, square, displayed, duration) ||
            displayed != m_advancedSelectedFrame || duration == 0U) {
            return 0U;
        }
        if (m_streamAwaitingPlaybackFrame) {
            // The authored boundary now belongs to the requested successor.
            // The still-displayed frame has consumed its complete dwell.
            return static_cast<uint16_t>(duration - 1U);
        }
        const uint32_t at = m_advancedPlaying
            ? (m_bulkTransferActive ? m_advancedClock : fxe::Millis()) : m_streamPausedAtMs;
        return static_cast<uint16_t>(std::min<uint32_t>(
            static_cast<uint32_t>(at - m_streamFrameStartedAtMs),
            duration - 1U));
    }
    if (!HasAdvancedDocument()) return 0U;
    const auto& document =
        m_advancedPersistence->AdvancedDocument().Value();
    uint64_t cycle = 0U;
    for (uint16_t index = 0U; index < document.frameCount; ++index) {
        cycle += document.frames[index].durationMs;
    }
    if (cycle == 0U || m_advancedSelectedFrame >= document.frameCount) {
        return 0U;
    }
    uint64_t start = 0U;
    for (uint16_t index = 0U; index < m_advancedSelectedFrame; ++index) {
        start += document.frames[index].durationMs;
    }
    const uint64_t phase = m_advancedElapsed % cycle;
    if (phase < start) return 0U;
    return static_cast<uint16_t>(std::min<uint64_t>(
        phase - start,
        static_cast<uint64_t>(document.frames[m_advancedSelectedFrame]
                                  .durationMs - 1U)));
}

void LitchcraftSession::AdoptStreamedModeAuthority() {
    if (!HasStreamedDocument() ||
        m_streamGeneration == m_frameStream->Generation()) {
        return;
    }
    // Boot may restore persisted mode authority. After an explicit mode
    // decision, a delayed preview/root rollback cannot undo Exit; only Enter
    // can make the session active again.
    const bool nextActive = m_frameStream->Active() &&
        (m_active || m_modeGeneration == 0U);
    m_streamGeneration = m_frameStream->Generation();
    if (m_active && !nextActive) {
        // A committed schema-4 root is mode authority, not merely render
        // metadata. Retire the live interaction exactly as an explicit Exit
        // does so the coordinator restores Setup/Hall ownership immediately.
        MarkModeChanged();
        FinishExit();
        return;
    }
    m_active = nextActive;
    if (nextActive) {
        if (m_session == 0U) {
            m_session = ++m_nonce;
            if (m_session == 0U) m_session = ++m_nonce;
        }
        m_exited = false;
    }
}
bool LitchcraftSession::HasStreamedDocument() const {
    return m_frameStream != nullptr && m_frameStream->Generation() != 0U &&
        m_frameStream->FrameCount() != 0U;
}

void LitchcraftSession::AttachFrameStream(
    litchcraft_frame_stream::FrameStream* const stream,
    void (*const wakeStorage)(void*, bool, uint32_t, bool),
    void* const wakeStorageContext) {
    m_bootArtworkFlags = 0U;
    m_frameStream = stream;
    if (stream != nullptr) stream->SetConfirmedPresentation(m_independentPresentation);
    m_presentationCaptured = false;
    m_frameStreamWakeStorage = wakeStorage;
    m_frameStreamWakeStorageContext = wakeStorageContext;
    m_streamGeneration = 0U;
    m_streamDocumentEpoch = 0U;
}

void LitchcraftSession::WakeStreamedStorage() {
    if (m_frameStreamWakeStorage != nullptr) {
        m_frameStreamWakeStorage(m_frameStreamWakeStorageContext,
                                 m_advancedPlaying, m_session, m_active);
    }
}
bool LitchcraftSession::HasAdvancedDocument() const {
    return m_advancedPersistence != nullptr && m_advancedPersistence->HasAdvancedDocument();
}

uint8_t LitchcraftSession::AnimationTopology() const {
    if (HasStreamedDocument()) return m_frameStream->Topology();
    return HasAdvancedDocument()
        ? m_advancedPersistence->AdvancedDocument().Value().topology : 3U;
}

void LitchcraftSession::ResetStreamedPlayback() {
    if (!HasStreamedDocument()) return;
    m_pendingPhysicalHistoryFrame = UINT16_MAX;
    if (m_pickPending &&
        m_pickDocumentEpoch != m_frameStream->DocumentEpoch()) {
        EndStroke();
        PublishColorPickStatus();
    }
    const uint32_t now = fxe::Millis();
    m_streamGeneration = m_frameStream->Generation();
    m_streamDocumentEpoch = m_frameStream->DocumentEpoch();
    m_advancedElapsed = 0U;
    m_advancedEffectElapsed = 0U;
    m_advancedClock = now;
    m_advancedSelectedFrame = 0U;
    SetAnimationPlaying(false);
    m_streamFrameStartedAtMs = now;
    m_streamPausedAtMs = m_advancedPlaying ? 0U : now;
    m_streamCorruptStartedAtMs = 0U;
    m_streamFrameStarted = false;
    m_streamCorruptDwell = false;
    m_streamAwaitingInitialFrame = true;
    m_streamAwaitingPlaybackFrame = false;
    m_streamAwaitingRecoveryFrame = false;
    (void)m_frameStream->Request(0U);
}

void LitchcraftSession::PinStreamedFrameForEdit() {
    if (!HasStreamedDocument()) return;
    SampleStreamedPlayback();
    const uint32_t now = fxe::Millis();
    if (m_independentPresentation && m_presentationActual &&
        m_presentationActualContext.session == m_session &&
        m_presentationActualContext.modeGeneration == m_modeGeneration &&
        m_presentationActualContext.documentEpoch == m_frameStream->DocumentEpoch() &&
        m_presentationActualPlayback.selectedFrame == m_frameStream->DisplayedIndex() &&
        m_presentationActualPlayback.selectedFrame == m_advancedSelectedFrame &&
        (m_presentationActualPlayback.bootFlags & 1U) == (m_bootArtworkFlags & 1U)) {
        // Refresh the confirmed cursor's phase without overriding a pending
        // explicit seek. Its existing admission must wait for that target.
        const uint8_t bootFlags = m_bootArtworkFlags;
        const uint8_t bootResume = m_bootResumeFrame;
        auto actual = m_presentationActualPlayback;
        litchcraft_presentation::AdvanceClock(actual, now, m_bulkTransferActive);
        actual.bootFlags = bootFlags;
        actual.bootResumeFrame = bootResume;
        ApplyPresentationPlayback(actual);
    }
    if (!m_advancedPlaying && m_streamFrameStarted) {
        // Refreshing a pause must preserve the displayed frame's frozen phase.
        m_streamFrameStartedAtMs += now - m_streamPausedAtMs;
    }
    SetAnimationPlaying(false);
    m_streamPausedAtMs = now;
    if (m_streamFrameStarted && m_streamAwaitingPlaybackFrame) {
        std::array<litchcraft_frame_store::Emitter, 4U> square{};
        uint16_t displayed = 0U;
        uint16_t duration = 0U;
        if (m_frameStream->CopyDisplayedSquare(
                0U, square, displayed, duration) &&
            displayed == m_advancedSelectedFrame && duration != 0U) {
            // Canceling the successor restores the still-displayed frame's
            // anchor before Play can consume its dwell again.
            m_streamFrameStartedAtMs -= duration;
            m_streamAwaitingPlaybackFrame = false;
        }
    }
    (void)m_frameStream->Request(m_advancedSelectedFrame);
}

void LitchcraftSession::SampleStreamedPlayback() {
    if (!HasStreamedDocument()) return;
    // Reopening the same resident changes its document epoch at the same
    // saved generation. Recognize that replacement before a later edit saves.
    if (m_streamGeneration != m_frameStream->Generation() ||
        m_streamDocumentEpoch != m_frameStream->DocumentEpoch()) {
        if (m_streamDocumentEpoch == m_frameStream->DocumentEpoch()) {
            m_streamGeneration = m_frameStream->Generation();
        } else {
            CancelBootArtwork();
            // Publish the replacement epoch before mode adoption can run the
            // exit lifecycle and sample playback again.
            ResetStreamedPlayback();
            m_streamGeneration = 0U;
            AdoptStreamedModeAuthority();
        }
    }
    if (m_independentPresentation) return;
    auto playback = CapturePresentationPlayback();
    litchcraft_presentation::Context context{};
    context.session = m_session;
    context.documentEpoch = m_frameStream->DocumentEpoch();
    context.active = m_active;
    context.transferSuspended = m_bulkTransferActive;
    context.transferEpoch = m_bulkTransferEpoch;
    const litchcraft_presentation::StreamPort port{
        m_frameStream,
        [](void* owner, bool playing, uint32_t session, bool active, bool) {
            auto& self = *static_cast<LitchcraftSession*>(owner);
            if (self.m_frameStreamWakeStorage != nullptr)
                self.m_frameStreamWakeStorage(self.m_frameStreamWakeStorageContext,
                    playing, session, active);
        }, this, false};
    const auto transition = litchcraft_presentation::AdvanceStreamed(
        playback, context, port, fxe::Millis());
    ApplyPresentationPlayback(playback);
    if (transition != litchcraft_presentation::BootTransition::None) {
        CancelBootArtwork();
        if (transition == litchcraft_presentation::BootTransition::Finished)
            m_bootArtworkFlags = 2U;
    }
}

void LitchcraftSession::InitializeAdvancedRuntime() {
#if !defined(LITCH_ESP32S3)
    if (m_advancedRuntime || !HasAdvancedDocument()) return;
    // Both schemas have one history owner. Placement construction changes the
    // active union member without allocating or retaining the static ring.
    new (&m_advancedHistory) LitchcraftHistory();
    m_advancedRuntime = true;
    m_strokeSource = Source::None;
    m_strokeChanged = false;
    ResetAdvancedPlayback();
#endif
}

void LitchcraftSession::ResetAdvancedPlayback() {
    m_advancedElapsed = 0U;
    m_advancedEffectElapsed = 0U;
    m_advancedClock = fxe::Millis();
    m_advancedSelectedFrame = 0U;
    SetAnimationPlaying(false);
    SyncAdvancedCanvas();
}

void LitchcraftSession::SampleAdvancedPlayback() {
    if (!HasAdvancedDocument()) return;
    InitializeAdvancedRuntime();
    const uint32_t now = fxe::Millis();
    auto playback = CapturePresentationPlayback();
    litchcraft_presentation::AdvanceClock(playback, now, m_bulkTransferActive);
    ApplyPresentationPlayback(playback);
    if (m_advancedPlaying) {
        m_advancedSelectedFrame = litchcraft_animation::FrameIndexAt(
            m_advancedPersistence->AdvancedDocument(), m_advancedElapsed);
    }
    m_advancedClock = now;
}

void LitchcraftSession::PauseAdvancedPlayback() {
    SampleAdvancedPlayback();
    SetAnimationPlaying(false);
    SyncAdvancedCanvas();
}

void LitchcraftSession::SelectAdvancedFrame(const uint8_t frame) {
    SampleAdvancedPlayback();
    SetAnimationPlaying(false);
    m_advancedSelectedFrame = frame;
    (void)litchcraft_animation::TryFrameStartElapsed(
        m_advancedPersistence->AdvancedDocument(), frame, m_advancedElapsed);
    m_advancedClock = fxe::Millis();
    SyncAdvancedCanvas();
}

void LitchcraftSession::SyncAdvancedCanvas() {
    if (!HasAdvancedDocument()) return;
    const auto& document = m_advancedPersistence->AdvancedDocument().Value();
    m_recentColors = document.favorites;
    if (m_advancedSelectedFrame >= document.frameCount) m_advancedSelectedFrame = 0U;
    const auto& frame = document.frames[m_advancedSelectedFrame];
    for (size_t i = 0U; i < m_canvas.size(); ++i) m_canvas[i] = frame.emitters[i].color;
}

void LitchcraftSession::QueueTimeline(const uint16_t request) {
    if (HasStreamedDocument()) SampleStreamedPlayback();
    else SampleAdvancedPlayback();
    if (++m_timelineSample == 0U) ++m_timelineSample;
    const auto base = V2State(request, Status::Ok);
    for (uint8_t half = 0U; half < (m_v2EffectClockSubscribed ? 4U : 2U); ++half) {
        auto packet = base;
        const bool effectClock = half >= 2U;
        packet.bytes[2] = effectClock ? 0x8bU + half - 2U : 0x88U + half;
        packet.bytes[13] = m_timelineSample;
        packet.bytes[14] = m_timelineSample >> 8U;
        packet.bytes[15] = effectClock ? 0U :
            (half == 0U ? (m_advancedPlaying ? 1U : 0U) : m_advancedSelectedFrame);
        const uint64_t clockElapsed = effectClock ? m_advancedEffectElapsed : m_advancedElapsed;
        const uint32_t elapsed = static_cast<uint32_t>(clockElapsed >> ((half % 2U) * 32U));
        for (uint8_t i = 0U; i < 4U; ++i) packet.bytes[16U + i] = elapsed >> (i * 8U);
        packet.size = 20U;
        Queue(packet);
    }
}
} // namespace modules
