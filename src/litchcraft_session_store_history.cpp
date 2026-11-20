#include "modules/litchcraft_session.hpp"

#include "foxie_embedded/millis.hpp"

namespace modules {

LitchcraftSession::Status LitchcraftSession::ApplyStoreNewCanvas(
    const uint8_t* const payload, const size_t size,
    const uint32_t connectionEpoch) {
    if (payload == nullptr || size != 8U || connectionEpoch == 0U) return Status::Malformed;
    if (!HasStreamedDocument() || m_pendingStoreStructural ||
        m_strokeSource == Source::Board) return Status::Busy;
    uint32_t revision = 0U;
    const auto result = m_frameStream->NewBlankDocument(
        Read32(payload), Read32(payload + 4U), LITCHCRAFT_DEFAULT_QUICK_PALETTE, &revision);
    if (result == litchcraft_frame_stream::FrameStream::GuardedEditResult::Conflict) {
        return Status::RevisionConflict;
    }
    EndStroke();
    CancelPendingStoreHistory();
    ResetPhysicalStoreBatch();
    ResetStreamedPlayback();
    m_storeGuardConnectionEpoch = connectionEpoch;
    m_storeGuardGeneration = Read32(payload);
    m_storeGuardRevision = revision;
    m_activity = true;
    WakeStreamedStorage();
    return Status::Ok;
}

LitchcraftSession::Status LitchcraftSession::ApplyStoreHistory(
    const bool redo, const size_t size, const uint32_t connectionEpoch) {
    if (size != 0U) return Status::Malformed;
    if (m_frameStream->ReplacementPending() && !m_pendingStoreStructural) return Status::Busy;
    if (connectionEpoch == 0U ||
        m_storeGuardConnectionEpoch != connectionEpoch ||
        m_storeGuardRevision == 0U ||
        !m_frameStream->AcceptsGeneration(m_storeGuardGeneration)) {
        CancelPendingStoreHistory();
        return Status::RevisionConflict;
    }
    if (m_pendingStoreStructural) {
        if (m_pendingStoreOpcode != (redo ? 7U : 6U)) return Status::Busy;
        return CompleteStoreStructural();
    }
    EndStroke();
    if (m_frameStream->HistoryIsStructural(redo)) {
        using Stream = litchcraft_frame_stream::FrameStream;
        if (m_frameStream->QueueStructuralHistory(m_storeGuardRevision, redo) !=
            Stream::StructuralResult::Pending) return Status::RevisionConflict;
        m_pendingStoreHistoryOrdered = true;
        m_pendingStoreStructural = true;
        m_pendingStoreOpcode = redo ? 7U : 6U;
        m_pendingPhysicalHistoryFrame = m_advancedSelectedFrame;
        m_pendingPhysicalHistoryRevision = m_storeGuardRevision;
        SetAnimationPlaying(false);
        m_streamPausedAtMs = fxe::Millis();
        WakeStreamedStorage();
        return CompleteStoreStructural();
    }
    const uint16_t frame = m_frameStream->HistoryTargetFrame(redo);
    if (m_pendingStoreHistoryOrdered) {
        const bool cancelled = frame == UINT16_MAX ||
            frame != m_pendingPhysicalHistoryFrame ||
            m_advancedSelectedFrame != frame ||
            m_frameStream->LiveRevision() != m_pendingPhysicalHistoryRevision;
        const bool failed = m_frameStream->RequestedFrameFailed() ||
            static_cast<uint32_t>(fxe::Millis() - m_pendingPhysicalHistoryAtMs) >= 5000U;
        if (cancelled || failed) {
            CancelPendingStoreHistory();
            return cancelled ? Status::RevisionConflict : Status::ResourceLimit;
        }
        if (m_frameStream->DisplayedIndex() != frame) return Status::Busy;
        m_pendingStoreHistoryOrdered = false;
        m_pendingPhysicalHistoryFrame = UINT16_MAX;
    } else {
        if (frame == UINT16_MAX) return Status::NoHistory;
        if (frame != m_advancedSelectedFrame ||
            m_frameStream->DisplayedIndex() != frame) {
            SetAnimationPlaying(false);
            m_streamPausedAtMs = fxe::Millis();
            m_streamFrameStarted = false;
            m_advancedSelectedFrame = static_cast<uint8_t>(frame);
            m_pendingPhysicalHistoryFrame = frame;
            m_pendingPhysicalHistoryRevision = m_storeGuardRevision;
            m_pendingPhysicalHistoryAtMs = fxe::Millis();
            m_pendingStoreHistoryOrdered = true;
            (void)m_frameStream->Request(frame);
            WakeStreamedStorage();
            return Status::Busy;
        }
    }
    const auto result = m_frameStream->StepHistoryIfRevision(
        m_storeGuardRevision, frame, redo);
    using Edit = litchcraft_frame_stream::FrameStream::GuardedEditResult;
    if (result == Edit::Conflict) {
        m_storeGuardConnectionEpoch = 0U;
        m_storeGuardRevision = 0U;
        return Status::RevisionConflict;
    }
    if (result == Edit::Unchanged) return Status::NoHistory;
    m_storeGuardRevision = m_frameStream->LiveRevision();
    m_advancedStrokeFrame = static_cast<uint8_t>(frame);
    PublishPhysicalStoreHistoryStep(m_storeGuardRevision);
    PinStreamedFrameForEdit();
    m_activity = true;
    WakeStreamedStorage();
    return Status::Ok;
}

LitchcraftSession::Status LitchcraftSession::ApplyPhysicalStoreHistory(const bool redo, const UiOwner owner) {
    if (!m_active) return Status::StaleSession;
    if (m_mutationsPaused) return Status::Busy;
    if (m_frameStream->ReplacementPending()) return Status::Busy;
    if (m_pendingStoreHistoryOrdered) return Status::Busy;
    if (m_pendingStoreStructural) return Status::Busy;
    if (m_pendingPhysicalHistoryFrame != UINT16_MAX) return Status::Busy;
    m_pendingPhysicalHistoryRedo = redo;
    m_pendingPhysicalHistoryOwner = owner;
    EndStroke();
    if (m_frameStream->HistoryIsStructural(redo)) {
        using Stream = litchcraft_frame_stream::FrameStream;
        if (m_frameStream->QueueStructuralHistory(m_frameStream->LiveRevision(), redo) !=
            Stream::StructuralResult::Pending) return Status::RevisionConflict;
        m_pendingStoreStructural = true;
        m_pendingStoreOpcode = 0U;
        m_pendingPhysicalHistoryFrame = m_advancedSelectedFrame;
        m_pendingPhysicalHistoryRevision = m_frameStream->LiveRevision();
        SetAnimationPlaying(false);
        m_streamPausedAtMs = fxe::Millis();
        WakeStreamedStorage();
        return Status::Ok;
    }
    const uint16_t frame = m_frameStream->HistoryTargetFrame(redo);
    if (frame == UINT16_MAX) return Status::NoHistory;
    const uint32_t revision = m_frameStream->LiveRevision();
    if (frame != m_advancedSelectedFrame ||
        m_frameStream->DisplayedIndex() != frame) {
        SetAnimationPlaying(false);
        m_streamPausedAtMs = fxe::Millis();
        m_streamFrameStarted = false;
        m_advancedSelectedFrame = static_cast<uint8_t>(frame);
        m_pendingPhysicalHistoryFrame = frame;
        m_pendingPhysicalHistoryRevision = revision;
        m_pendingPhysicalHistoryAtMs = fxe::Millis();
        (void)m_frameStream->Request(frame);
        WakeStreamedStorage();
        return Status::Ok;
    }
    const auto result = m_frameStream->StepHistoryIfRevision(
        revision, frame, redo);
    using Edit = litchcraft_frame_stream::FrameStream::GuardedEditResult;
    if (result == Edit::Conflict) return Status::RevisionConflict;
    if (result == Edit::Unchanged) return Status::NoHistory;
    m_advancedStrokeFrame = static_cast<uint8_t>(frame);
    PublishPhysicalStoreHistoryStep(m_frameStream->LiveRevision());
    // Physical history has chosen its frame; only refresh an existing pause.
    if (!m_advancedPlaying) {
        PinStreamedFrameForEdit();
    } else {
        SetAnimationPlaying(false);
        m_streamPausedAtMs = fxe::Millis();
    }
    ReportPhysicalFeedback(redo ? ControlFeedbackAction::Redo : ControlFeedbackAction::Undo,
        owner, redo ? ControlRole::O : ControlRole::X);
    m_activity = true;
    WakeStreamedStorage();
    return Status::Ok;
}

void LitchcraftSession::PublishPhysicalStoreHistoryStep(
    const uint32_t revision) {
    const auto* const stream = m_frameStream;
    const auto authority = stream->ReadLiveAuthority();
    const uint32_t documentEpoch = stream->DocumentEpoch();
    const uint32_t session = m_session;
    const uint8_t frame = m_advancedStrokeFrame;
    const auto current = [&]() {
        if (m_frameStream != stream || m_session != session ||
            m_advancedStrokeFrame != frame || m_advancedSelectedFrame != frame ||
            stream->DocumentEpoch() != documentEpoch ||
            stream->DisplayedIndex() != frame) return false;
        const auto live = stream->ReadLiveAuthority();
        return authority.generation != 0U && revision != 0U &&
            authority.revision == revision &&
            live.generation == authority.generation && live.revision == revision;
    };
    if (!current()) {
        m_physicalStoreBatchOverflow = true;
        return;
    }
    if (stream->SteppedNeedsRepair(revision)) {
        std::array<litchcraft_frame_store::Emitter, 4U> square{};
        uint16_t displayedFrame = 0U;
        uint16_t duration = 0U;
        if (stream->CopyDisplayedSquare(
                0U, square, displayedFrame, duration) &&
            displayedFrame == frame && current()) {
            const auto& emitter = square[0U];
            RecordPhysicalStoreEdit(UINT64_C(1), emitter.color, 1U,
                emitter.effect == 0U && emitter.effectDurationMs == 0U,
                authority, frame);
        }
        m_physicalStoreBatchOverflow = true;
        return;
    }
    const size_t count = stream->SteppedEmitterCount(revision);
    if (count == 0U) {
        m_physicalStoreBatchOverflow = true;
        return;
    }
    uint8_t pendingSquare = 0xffU;
    uint8_t pendingMask = 0U;
    Rgb pendingColor{};
    bool pendingClearEffects = false;
    uint64_t footprint = 0U;
    uint8_t mask = 0U;
    Rgb color{};
    bool clearEffects = false;
    const auto publish = [&]() {
        if (mask == 0U) return true;
        if (!current()) {
            m_physicalStoreBatchOverflow = true;
            return false;
        }
        // Keep the authority captured with the copied inverse, even if a
        // storage publication advances generation after this final check.
        RecordPhysicalStoreEdit(footprint, color, mask, clearEffects,
                                authority, frame);
        footprint = 0U;
        mask = 0U;
        return true;
    };
    const auto finishSquare = [&]() {
        if (pendingMask == 0U) return true;
        if (mask != 0U &&
            (mask != pendingMask || color.r != pendingColor.r ||
             color.g != pendingColor.g || color.b != pendingColor.b ||
             clearEffects != pendingClearEffects) && !publish()) return false;
        footprint |= UINT64_C(1) << pendingSquare;
        mask = pendingMask;
        color = pendingColor;
        clearEffects = pendingClearEffects;
        pendingMask = 0U;
        return true;
    };
    for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
        uint8_t index = 0U;
        litchcraft_frame_store::Emitter emitter{};
        if (!stream->CopySteppedEmitter(revision, ordinal, index, emitter)) {
            m_physicalStoreBatchOverflow = true;
            return;
        }
        const uint8_t square = index / 4U;
        const bool emitterClearEffects = emitter.effect == 0U &&
            emitter.effectDurationMs == 0U;
        if (!emitterClearEffects) m_physicalStoreBatchOverflow = true;
        if (pendingMask != 0U &&
            (pendingSquare != square ||
             pendingColor.r != emitter.color.r ||
             pendingColor.g != emitter.color.g ||
             pendingColor.b != emitter.color.b ||
             pendingClearEffects != emitterClearEffects) &&
            !finishSquare()) return;
        pendingSquare = square;
        pendingColor = emitter.color;
        pendingClearEffects = emitterClearEffects;
        pendingMask |= static_cast<uint8_t>(1U << (index % 4U));
    }
    if (!finishSquare()) return;
    (void)publish();
}

} // namespace modules
