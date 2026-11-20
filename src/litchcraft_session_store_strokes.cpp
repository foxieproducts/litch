#include "modules/litchcraft_session.hpp"

#include "foxie_embedded/millis.hpp"

namespace modules {

LitchcraftSession::Status LitchcraftSession::ApplyStoreStroke(
    const uint8_t op, const uint8_t* const payload, const size_t size,
    const uint32_t connectionEpoch) {
    const bool paint = op == 3U;
    if (size < 8U || size > 10U) return Status::Malformed;
    const uint16_t frame = payload[0];
    const uint16_t stroke = Read16(payload + (paint ? 1U : 5U));
    const uint8_t flags = payload[paint ? 3U : 7U];
    const uint8_t mask = payload[4U];
    if (frame >= m_frameStream->FrameCount() ||
        frame != m_advancedSelectedFrame || stroke == 0U || flags > 3U ||
        mask == 0U || mask > 15U) return Status::Malformed;
    if (!paint && (!litchcraft_animation::IsValidEffect(payload[1U]) ||
        !litchcraft_animation::IsValidEffectDuration(
                               static_cast<litchcraft_animation::Effect>(payload[1U]),
                               Read16(payload + 2U)))) {
        return Status::Malformed;
    }
    for (size_t i = 8U; i < size; ++i) {
        if (payload[i] >= 64U) return Status::Malformed;
    }
    if (connectionEpoch == 0U ||
        m_storeGuardConnectionEpoch != connectionEpoch ||
        m_storeGuardRevision == 0U ||
        !m_frameStream->AcceptsGeneration(m_storeGuardGeneration)) {
        return Status::RevisionConflict;
    }
    if ((flags & 1U) != 0U) EndStroke();
    StartStroke(Source::App, stroke);
    if (m_strokeSource != Source::App) return Status::Busy;
    if (!paint && m_supportActions != nullptr) m_supportActions->SetKind(m_supportStroke, trace::SupportActionKind::Effect);
    using Edit = litchcraft_frame_stream::FrameStream::GuardedEditResult;
    Edit result = Edit::Unchanged;
    uint32_t resultingRevision = m_storeGuardRevision;
    if (size == 8U) {
        if (m_frameStream->ReadLiveAuthority().revision !=
            m_storeGuardRevision) result = Edit::Conflict;
    } else if (paint) {
        result = m_frameStream->PaintDisplayedBatchIfRevision(
            m_storeGuardRevision, frame, payload + 8U, size - 8U, mask,
            {payload[5U], payload[6U], payload[7U]}, false,
            &resultingRevision);
    } else {
        result = m_frameStream->ApplyDisplayedEffectBatchIfRevision(
            m_storeGuardRevision, frame, payload + 8U, size - 8U, mask,
            payload[1U], Read16(payload + 2U), &resultingRevision);
    }
    if (result == Edit::Conflict) {
        if (m_supportActions != nullptr) m_supportActions->Finish(m_supportStroke, trace::SupportActionOutcome::Rejected);
        EndStroke();
        m_storeGuardConnectionEpoch = 0U;
        m_storeGuardRevision = 0U;
        return Status::RevisionConflict;
    }
    const bool changed = result == Edit::Changed;
    m_storeGuardRevision = resultingRevision;
    m_strokeChanged = m_strokeChanged || changed;
    if (changed) {
        for (size_t i = 8U; i < size; ++i) m_supportStrokeSquares |= UINT64_C(1) << payload[i];
        m_activity = true;
        WakeStreamedStorage();
    }
    if ((flags & 2U) != 0U) EndStroke();
    return Status::Ok;
}

LitchcraftSession::Status LitchcraftSession::ApplyStoreCanvas(
    const uint8_t* const payload, const size_t size,
    const uint32_t connectionEpoch) {
    if (size != 6U || payload[1U] == 0U || payload[1U] > 15U) {
        return Status::Malformed;
    }
    using Stream = litchcraft_frame_stream::FrameStream;
    const auto action = static_cast<Stream::CanvasAction>(payload[5U]);
    const bool rotate = action == Stream::CanvasAction::Clockwise ||
        action == Stream::CanvasAction::HalfTurn ||
        action == Stream::CanvasAction::Counterclockwise;
    const bool erase = action == Stream::CanvasAction::Erase ||
        action == Stream::CanvasAction::LightErase ||
        action == Stream::CanvasAction::DarkErase;
    const bool fill = action == Stream::CanvasAction::Fill ||
        action == Stream::CanvasAction::LightFill ||
        action == Stream::CanvasAction::DarkFill;
    if ((!erase && !fill && !rotate) ||
        ((action == Stream::CanvasAction::Erase || rotate) && payload[1U] != 15U) ||
        ((erase || rotate) && (payload[2U] != 0U || payload[3U] != 0U || payload[4U] != 0U)) ||
        payload[0U] >= m_frameStream->FrameCount()) return Status::Malformed;
    if (connectionEpoch == 0U ||
        m_storeGuardConnectionEpoch != connectionEpoch ||
        m_storeGuardRevision == 0U) return Status::RevisionConflict;
    if (m_pendingStoreHistoryOrdered ||
        m_pendingPhysicalHistoryFrame != UINT16_MAX) return Status::Busy;
    SampleStreamedPlayback();
    if (payload[0U] != m_advancedSelectedFrame) return Status::RevisionConflict;
    uint32_t resultingRevision = m_storeGuardRevision;
    const auto result = m_frameStream->EditDisplayedCanvasIfRevision(
        {m_storeGuardGeneration, m_storeGuardRevision, payload[0U], payload[1U],
         {payload[2U], payload[3U], payload[4U]}, action}, &resultingRevision);
    if (result == Stream::GuardedEditResult::Conflict) {
        m_storeGuardConnectionEpoch = 0U;
        m_storeGuardRevision = 0U;
        return Status::RevisionConflict;
    }
    EndStroke();
    m_storeGuardRevision = resultingRevision;
    if (!m_advancedPlaying) {
        PinStreamedFrameForEdit();
    } else {
        // Admission chose this frame; a second sample could select its successor.
        SetAnimationPlaying(false);
        m_streamPausedAtMs = fxe::Millis();
    }
    if (action == Stream::CanvasAction::Fill) {
        SelectTool(false, payload[1U], {payload[2U], payload[3U], payload[4U]},
                   0U, false);
    }
    m_activity = true;
    if (result == Stream::GuardedEditResult::Changed) WakeStreamedStorage();
    return Status::Ok;
}

LitchcraftSession::Status LitchcraftSession::ApplyStoreFrameEdit(
    const uint8_t* const payload, const size_t size, const uint32_t connectionEpoch) {
    if (size != 5U || payload == nullptr) return Status::Malformed;
    const uint8_t action = payload[0U];
    const uint16_t source = payload[1U];
    const uint16_t duration = Read16(payload + 3U);
    if ((action != 0U && action != 1U && action != 3U) ||
        (action == 0U && (source == 255U || payload[2U] != source + 1U || duration != 0U)) ||
        (action == 1U && (payload[2U] != 0U || duration != 0U)) ||
        (action == 3U && (payload[2U] != 0U || duration < 50U || duration > 5000U))) {
        return Status::Malformed;
    }
    if (connectionEpoch == 0U || m_storeGuardConnectionEpoch != connectionEpoch ||
        m_storeGuardRevision == 0U ||
        !m_frameStream->AcceptsGeneration(m_storeGuardGeneration)) {
        CancelPendingStoreHistory();
        return Status::RevisionConflict;
    }
    if (m_pendingStoreStructural) {
        if (m_pendingStoreOpcode != 10U || m_pendingStoreFrameAction != action ||
            m_pendingPhysicalHistoryFrame != source) return Status::Busy;
        return CompleteStoreStructural();
    }
    if (source >= m_frameStream->FrameCount()) return Status::Malformed;
    SampleStreamedPlayback();
    if (m_pendingStoreHistoryOrdered) {
        if (m_pendingStoreOpcode != 10U || m_pendingStoreFrameAction != action ||
            m_pendingPhysicalHistoryFrame != source) return Status::Busy;
        if (source != m_advancedSelectedFrame ||
            m_frameStream->RequestedIndex() != source ||
            m_frameStream->LiveRevision() != m_pendingPhysicalHistoryRevision) {
            CancelPendingStoreHistory();
            return Status::RevisionConflict;
        }
        if (m_frameStream->RequestedFrameFailed()) {
            CancelPendingStoreHistory();
            return Status::ResourceLimit;
        }
        if (m_frameStream->DisplayedIndex() != source) return Status::Busy;
        m_pendingStoreHistoryOrdered = false;
        m_pendingStoreOpcode = 0U;
        m_pendingPhysicalHistoryFrame = UINT16_MAX;
    } else if (m_pendingPhysicalHistoryFrame != UINT16_MAX) {
        return Status::Busy;
    }
    if (source != m_advancedSelectedFrame) return Status::RevisionConflict;
    if (m_frameStream->DisplayedIndex() != source) {
        // Timeline accepts the seek before the worker pages its presentation.
        // Retain this exact Ordered action through that existing frame request.
        EndStroke();
        m_pendingStoreHistoryOrdered = true;
        m_pendingStoreOpcode = 10U;
        m_pendingStoreFrameAction = action;
        m_pendingPhysicalHistoryFrame = source;
        m_pendingPhysicalHistoryRevision = m_storeGuardRevision;
        SetAnimationPlaying(false);
        m_streamPausedAtMs = fxe::Millis();
        (void)m_frameStream->Request(source);
        WakeStreamedStorage();
        return Status::Busy;
    }
    using Stream = litchcraft_frame_stream::FrameStream;
    if (action == 1U && m_frameStream->FrameCount() == 1U) {
        const std::array<uint8_t, 6U> clear{static_cast<uint8_t>(source), 15U, 0U, 0U, 0U, 1U};
        return ApplyStoreCanvas(clear.data(), clear.size(), connectionEpoch);
    }
    if (action == 0U && m_frameStream->FrameCount() == 256U) return Status::ResourceLimit;
    const Stream::FrameEdit edit{m_storeGuardGeneration, m_storeGuardRevision,
        source, duration, static_cast<Stream::FrameEditAction>(action)};
    if (action == 3U) {
        uint32_t revision = m_storeGuardRevision;
        const auto result = m_frameStream->EditDisplayedDurationIfRevision(edit, &revision);
        if (result == Stream::GuardedEditResult::Conflict) return Status::RevisionConflict;
        EndStroke();
        m_storeGuardRevision = revision;
        SetAnimationPlaying(false);
        m_streamPausedAtMs = fxe::Millis();
        if (result == Stream::GuardedEditResult::Changed) WakeStreamedStorage();
        m_activity = true;
        return Status::Ok;
    }
    if (m_frameStream->QueueStructuralEdit(edit) != Stream::StructuralResult::Pending) {
        return Status::RevisionConflict;
    }
    EndStroke();
    m_pendingStoreHistoryOrdered = true;
    m_pendingStoreStructural = true;
    m_pendingStoreOpcode = 10U;
    m_pendingStoreFrameAction = action;
    m_pendingPhysicalHistoryFrame = source;
    m_pendingPhysicalHistoryRevision = m_storeGuardRevision;
    SetAnimationPlaying(false);
    m_streamPausedAtMs = fxe::Millis();
    WakeStreamedStorage();
    return CompleteStoreStructural();
}

LitchcraftSession::Status LitchcraftSession::CompleteStoreStructural() {
    using Stream = litchcraft_frame_stream::FrameStream;
    uint16_t selected = 0U;
    uint32_t revision = 0U;
    const auto result = m_frameStream->TakeStructuralResult(selected, revision);
    if (result == Stream::StructuralResult::Pending) return Status::Busy;
    const bool history = m_pendingStoreOpcode != 10U;
    const bool physicalHistory = history && !m_pendingStoreHistoryOrdered;
    const bool redo = m_pendingPhysicalHistoryRedo;
    const UiOwner owner = m_pendingPhysicalHistoryOwner;
    m_pendingStoreHistoryOrdered = false;
    m_pendingStoreStructural = false;
    m_pendingStoreOpcode = 0U;
    m_pendingPhysicalHistoryFrame = UINT16_MAX;
    if (result != Stream::StructuralResult::Changed) {
        return result == Stream::StructuralResult::IoFailure
            ? Status::ResourceLimit : Status::RevisionConflict;
    }
    m_storeGuardRevision = revision;
    // A newer Timeline choice after internal publication keeps its ownership.
    if (m_frameStream->RequestedIndex() == selected &&
        m_frameStream->DisplayedIndex() == selected) {
        m_advancedSelectedFrame = static_cast<uint8_t>(selected);
    }
    m_advancedStrokeFrame = static_cast<uint8_t>(selected);
    if (history) PublishPhysicalStoreHistoryStep(revision);
    if (physicalHistory) ReportPhysicalFeedback(
        redo ? ControlFeedbackAction::Redo : ControlFeedbackAction::Undo,
        owner, redo ? ControlRole::O : ControlRole::X);
    m_activity = true;
    WakeStreamedStorage();
    return Status::Ok;
}

} // namespace modules
