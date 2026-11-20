#include "modules/litchcraft_session.hpp"

#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_persistence.hpp"

namespace modules {
namespace presentation = litchcraft_presentation;

LitchcraftSession::PresentationAccess LitchcraftSession::FramePresentationAccess() const {
    // Known refusals must not wait for pixels and block a later Enter/repair.
    const bool missing = StoreIsSoleArtworkOwner() && !HasStreamedDocument();
    const bool failed = HasStreamedDocument() && !m_frameStream->PresentationReady() &&
        (m_frameStream->RequestedFrameFailed() || m_frameStream->RequestedFrameCorrupt());
    return !m_active || m_mutationsPaused || missing || failed
        ? PresentationAccess::Control : PresentationAccess::Frame;
}

LitchcraftSession::PresentationAccess LitchcraftSession::ButtonPresentationAccess(
    const ButtonEvent& event) const {
    if (!m_active || m_modeChordFormed) return PresentationAccess::Control;
    if (IsXButton(event.button) || IsOButton(event.button))
        return event.event == fxe::Button::Event::Release
            ? FramePresentationAccess() : PresentationAccess::Control;
    return m_occupancy == 0U ? PresentationAccess::Control : FramePresentationAccess();
}

LitchcraftSession::PresentationAccess LitchcraftSession::UpdatePresentationAccess() const {
    // Use stored activity: IsActive() can already reflect an inactive Store
    // generation before Update has completed its Exit handoff.
    const bool adoptingStore = HasStreamedDocument() &&
        (m_streamGeneration != m_frameStream->Generation() ||
         m_streamDocumentEpoch != m_frameStream->DocumentEpoch());
    return m_active || IsBootArtworkPlaying() || m_exited || adoptingStore
        ? PresentationAccess::Control : PresentationAccess::None;
}

bool LitchcraftSession::PresentationFramePending() const {
    return m_independentPresentation && m_active && !m_mutationsPaused && HasStreamedDocument() &&
        m_advancedSelectedFrame != m_frameStream->DisplayedIndex() &&
        !m_frameStream->RequestedFrameFailed() && !m_frameStream->RequestedFrameCorrupt();
}

bool LitchcraftSession::CanMutatePresentedFrame() const {
    // Sample under admission; ordinary maintenance/timeouts still run while
    // frame-mutating timers wait for the exact target to become presentable.
    return !m_independentPresentation || !HasStreamedDocument() ||
        (m_frameStream->PresentationReady() && !PresentationFramePending());
}

presentation::Playback LitchcraftSession::CapturePresentationPlayback() const {
    return {m_advancedElapsed, m_advancedEffectElapsed, m_advancedClock,
        m_streamFrameStartedAtMs, m_streamPausedAtMs, m_streamCorruptStartedAtMs,
        m_advancedSelectedFrame, m_bootArtworkFlags, m_bootResumeFrame,
        m_advancedPlaying, m_streamFrameStarted, m_streamCorruptDwell,
        m_streamAwaitingInitialFrame, m_streamAwaitingPlaybackFrame,
        m_streamAwaitingRecoveryFrame, m_playbackSuspended};
}

void LitchcraftSession::ApplyPresentationPlayback(const presentation::Playback& playback) {
    m_advancedElapsed = playback.elapsedMs;
    m_advancedEffectElapsed = playback.effectElapsedMs;
    m_advancedClock = playback.clockMs;
    m_streamFrameStartedAtMs = playback.frameStartedAtMs;
    m_streamPausedAtMs = playback.pausedAtMs;
    m_streamCorruptStartedAtMs = playback.corruptStartedAtMs;
    m_advancedSelectedFrame = static_cast<uint8_t>(playback.selectedFrame);
    m_bootArtworkFlags = playback.bootFlags;
    m_bootResumeFrame = playback.bootResumeFrame;
    m_advancedPlaying = playback.playing;
    m_streamFrameStarted = playback.frameStarted;
    m_streamCorruptDwell = playback.corruptDwell;
    m_streamAwaitingInitialFrame = playback.awaitingInitialFrame;
    m_streamAwaitingPlaybackFrame = playback.awaitingPlaybackFrame;
    m_streamAwaitingRecoveryFrame = playback.awaitingRecoveryFrame;
    m_playbackSuspended = playback.suspended;
}

void LitchcraftSession::SetBulkTransferActive(const bool active) {
    if (m_bulkTransferActive == active) return;
    auto playback = CapturePresentationPlayback();
    presentation::AdvanceClock(playback, fxe::Millis(), active);
    ApplyPresentationPlayback(playback);
    m_bulkTransferActive = active;
    if (++m_bulkTransferEpoch == 0U) ++m_bulkTransferEpoch;
    // A receipt captured before this edge cannot restore its older clock.
    m_presentationCaptured = false;
    m_presentationActual = false;
}

void LitchcraftSession::SetIndependentPresentation(const bool enabled) {
    if (m_independentPresentation == enabled) return;
    m_independentPresentation = enabled;
    m_presentationCaptured = false;
    m_presentationFeedbackSequence = 0U;
    m_presentationActual = false;
    if (m_frameStream != nullptr) m_frameStream->SetConfirmedPresentation(enabled);
}

void LitchcraftSession::CapturePresentation(presentation::Inputs& inputs) {
    inputs = {};
    const bool streamed = HasStreamedDocument();
    const bool waiting = StoreIsSoleArtworkOwner() && !streamed;
    const bool advanced = !streamed && !waiting && HasAdvancedDocument();
    if (streamed && (m_streamGeneration != m_frameStream->Generation() ||
        m_streamDocumentEpoch != m_frameStream->DocumentEpoch())) {
        SampleStreamedPlayback();
    } else if (advanced) {
        // This compatibility converter resolves one selected frame. It does
        // not export/clone a document for autonomous legacy frame generation.
        SampleAdvancedPlayback();
        m_advancedSelectedFrame = litchcraft_animation::FrameIndexAt(
            m_advancedPersistence->AdvancedDocument(), m_advancedElapsed);
    }
    inputs.source = streamed ? presentation::Source::Stream : waiting
        ? presentation::Source::WaitingForStore : advanced
        ? presentation::Source::Legacy : presentation::Source::Static;
    inputs.playback = CapturePresentationPlayback();
    inputs.context = {m_session, m_modeGeneration,
        streamed ? m_frameStream->DocumentEpoch() : m_canvasGeneration,
        streamed ? m_frameStream->LiveRevision() : m_revision,
        m_presentationIntentEpoch, m_active, m_bulkTransferActive, m_bulkTransferEpoch};
    if (advanced) {
        const auto& document = m_advancedPersistence->AdvancedDocument().Value();
        const auto& frame = document.frames[m_advancedSelectedFrame];
        inputs.frame.durationMs = frame.durationMs;
        inputs.sparkleSeed = document.sparkleSeed;
        for (size_t index = 0U; index < inputs.frame.emitters.size(); ++index) {
            const auto& emitter = frame.emitters[index];
            inputs.frame.emitters[index] = {emitter.color,
                static_cast<uint8_t>(emitter.effect), emitter.durationMs};
        }
    } else if (!streamed && !waiting) {
        for (size_t index = 0U; index < inputs.frame.emitters.size(); ++index)
            inputs.frame.emitters[index].color = m_canvas[index];
    } else if (streamed) {
        inputs.sparkleSeed = m_frameStream->SparkleSeed();
    }
    inputs.capturedAtMs = fxe::Millis();
    inputs.blockedOccupancy = m_blockedOccupancy;
    inputs.quickPalette = m_quickPaletteColors;
    inputs.pickBefore = m_pickBefore;
    for (uint8_t slot = 0U; slot < inputs.quickPaletteSquares.size(); ++slot)
        inputs.quickPaletteSquares[slot] = QuickPaletteSquare(slot);
    inputs.playbackStatusChangedAtMs = m_playbackStatusChangedAtMs;
    inputs.playbackStatusFrom256 = m_playbackStatusFrom256;
    inputs.quickPaletteTouchedMs = m_quickPaletteTouchedMs;
    inputs.quickPaletteFadeAtMs = m_quickPaletteFadeAtMs;
    inputs.pickStartedMs = m_pickStartedMs;
    inputs.eraseSelectedMs = m_eraseSelectedMs;
    inputs.shakeEraseAtMs = m_shakeEraseAnimationAtMs;
    inputs.entryHoldStartedMs = m_modeChordStartedMs;
    inputs.color = m_color;
    inputs.hardware = m_hardware;
    inputs.pickSquare = m_pickSquare;
    inputs.quickPaletteIndex = m_quickPaletteIndex;
    inputs.active = m_active;
    inputs.erasing = m_erase;
    inputs.eraseCueStarted = m_eraseCueStarted;
    inputs.quickPaletteVisible = m_quickPaletteVisible;
    inputs.quickPaletteFading = m_quickPaletteFading;
    inputs.pickPending = m_pickPending;
    inputs.shakeEraseActive = m_shakeEraseAnimationActive;
    inputs.exitHoldActive = IsExitHoldActive();
    const auto& before = m_presentationCapturedContext;
    const bool cursorChanged = !m_presentationCaptured || m_presentationSource != inputs.source ||
        before.session != inputs.context.session ||
        before.modeGeneration != inputs.context.modeGeneration ||
        before.documentEpoch != inputs.context.documentEpoch ||
        !presentation::SamePlayback(inputs.playback, m_presentationCapturedPlayback);
    if (cursorChanged) {
        if (++m_presentationIntentEpoch == 0U) ++m_presentationIntentEpoch;
    }
    inputs.context.intentEpoch = m_presentationIntentEpoch;
    if (cursorChanged || before.contentRevision != inputs.context.contentRevision ||
        before.active != inputs.context.active ||
        !presentation::SameVisuals(inputs, m_presentationCapturedVisuals)) {
        if (++m_presentationEpoch == 0U) ++m_presentationEpoch;
    }
    inputs.presentationEpoch = m_presentationEpoch;
    m_presentationCapturedVisuals = inputs;
    m_presentationCapturedPlayback = inputs.playback;
    m_presentationCapturedContext = inputs.context;
    m_presentationSource = inputs.source;
    m_presentationCaptured = true;
    if (streamed && m_independentPresentation)
        (void)m_frameStream->PublishPresentationContext(inputs.presentationEpoch);
}

bool LitchcraftSession::ApplyPresentationFeedback(const presentation::Feedback& feedback) {
    if (!feedback.ready || feedback.context.session != m_session ||
        feedback.context.modeGeneration != m_modeGeneration ||
        feedback.context.transferSuspended != m_bulkTransferActive ||
        feedback.context.transferEpoch != m_bulkTransferEpoch ||
        (m_independentPresentation && feedback.sequence <= m_presentationFeedbackSequence)) return false;
    const bool streamed = HasStreamedDocument();
    if (streamed && (feedback.context.documentEpoch != m_frameStream->DocumentEpoch() ||
        !m_frameStream->PresentationReady() ||
        feedback.playback.selectedFrame != m_frameStream->DisplayedIndex())) return false;
    if (streamed && m_independentPresentation) {
        m_presentationActualPlayback = feedback.playback;
        m_presentationActualContext = feedback.context;
        m_presentationActual = true;
        m_presentationFeedbackSequence = feedback.sequence;
    }
    if (!m_presentationCaptured || !feedback.intentApplied ||
        !presentation::SameContext(feedback.context, m_presentationCapturedContext) ||
        !presentation::SamePlayback(CapturePresentationPlayback(), m_presentationCapturedPlayback) ||
        (streamed && feedback.context.contentRevision != m_frameStream->LiveRevision())) return false;
    if (feedback.bootTransition != presentation::BootTransition::None) {
        CancelBootArtwork();
        if (feedback.bootTransition == presentation::BootTransition::Finished)
            m_bootArtworkFlags = 2U;
        m_presentationCaptured = false;
        return true;
    }
    ApplyPresentationPlayback(feedback.playback);
    m_presentationCapturedPlayback = feedback.playback;
    m_presentationFeedbackSequence = feedback.sequence;
    if (HasStreamedDocument()) m_streamGeneration = m_frameStream->Generation();
    return true;
}

} // namespace modules
