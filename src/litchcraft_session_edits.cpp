#include "modules/litchcraft_session.hpp"

#include <algorithm>

#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_persistence.hpp"

namespace modules {
using namespace litchcraft_animation;

LitchcraftSession::PresentationAccess
LitchcraftSession::OrderedPresentationAccess(const uint8_t opcode) const {
    switch (opcode) {
    case 1U: case 2U: case 5U: case 8U: case 11U:
        return PresentationAccess::Control;
    case 3U: case 4U: case 6U: case 7U: case 9U: case 10U:
        return FramePresentationAccess();
    default: return PresentationAccess::None;
    }
}

bool LitchcraftSession::OrderedWaitsForPresentedFrame(const uint8_t opcode,
    const uint8_t* const bytes, const size_t size) const {
    if (!m_independentPresentation || !HasStreamedDocument() || bytes == nullptr ||
        (opcode != 3U && opcode != 4U && opcode != 9U && opcode != 10U)) return false;
    const size_t sourceOffset = opcode == 10U ? 1U : 0U;
    if (size <= sourceOffset) return false;
    const uint16_t frame = bytes[sourceOffset];
    // A newer source decision must reach ordinary domain rejection, rather
    // than retaining a stale named edit forever behind a different selection.
    return frame == m_advancedSelectedFrame && frame < m_frameStream->FrameCount() &&
        PresentationFramePending();
}

LitchcraftSession::Status LitchcraftSession::ApplyConnectionOrdered(
    const uint8_t opcode, const uint8_t* const payload, const size_t size,
    const uint32_t connectionEpoch) {
    CancelBootArtwork();
    if (!m_active || m_mutationsPaused) {
        CancelPendingStoreHistory();
        return Status::Busy;
    }
    if (opcode == 11U) return ApplyStoreNewCanvas(payload, size, connectionEpoch);
    if (!HasStreamedDocument()) CancelPendingStoreHistory();
    if (opcode == 5U) {
        if (size != 5U || payload == nullptr || payload[0] > 1U ||
            payload[1] == 0U || payload[1] > 15U) {
            return Status::Malformed;
        }
        if (!StoreIsSoleArtworkOwner() && !HasStreamedDocument() &&
            !HasAdvancedDocument()) return Status::Busy;
        // Match compact Tool selection without routing through its general
        // command dispatcher or repainting a resting physical brush.
        EndStroke();
        SelectTool(payload[0] != 0U, payload[1],
                   {payload[2], payload[3], payload[4]}, 0U, false);
        return Status::Ok;
    }
    if (HasStreamedDocument()) {
        if (m_frameStream->ReplacementPending() && !m_pendingStoreStructural) {
            CancelPendingStoreHistory();
            return Status::Busy;
        }
        if (m_pendingStoreStructural &&
            opcode != m_pendingStoreOpcode) return Status::Busy;
        if (opcode == 6U || opcode == 7U) {
            return ApplyStoreHistory(opcode == 7U, size, connectionEpoch);
        }
        if (payload == nullptr) return Status::Malformed;
        if (opcode == 8U) {
            m_storeGuardConnectionEpoch = 0U;
            m_storeGuardRevision = 0U;
            if (size != 8U || connectionEpoch == 0U) {
                return Status::Malformed;
            }
            const uint32_t generation = Read32(payload);
            const uint32_t revision = Read32(payload + 4U);
            const auto authority = m_frameStream->ReadLiveAuthority();
            if (revision == 0U || authority.generation == 0U ||
                authority.revision != revision ||
                !m_frameStream->AcceptsGeneration(generation)) {
                return Status::RevisionConflict;
            }
            m_storeGuardConnectionEpoch = connectionEpoch;
            m_storeGuardGeneration = generation;
            m_storeGuardRevision = revision;
            return Status::Ok;
        }
        if (opcode == 9U) return ApplyStoreCanvas(payload, size, connectionEpoch);
        if (opcode == 10U) return ApplyStoreFrameEdit(payload, size, connectionEpoch);
        if (opcode == 3U || opcode == 4U) {
            return ApplyStoreStroke(opcode, payload, size, connectionEpoch);
        }
        if (opcode != 2U || size != 6U) {
            return Status::Unsupported;
        }
        // Keep Store generation validation in its existing runtime owner.
        std::array<uint8_t, 7U> runtime{};
        runtime[0U] = payload[2U];
        runtime[1U] = payload[3U];
        runtime[2U] = payload[4U];
        runtime[3U] = payload[5U];
        runtime[4U] = payload[0U];
        runtime[5U] = payload[1U];
        return ApplyStoreRuntime(runtime.data(), runtime.size());
    }
    if (!HasAdvancedDocument()) return Status::Busy;
    if (opcode == 2U) {
        return ApplyV2Edit(opcode, payload, size);
    }
    if (opcode == 3U || opcode == 4U) {
        return ApplyV2Stroke(opcode, payload, size);
    }
    if (opcode == 6U || opcode == 7U) {
        return ApplyV2Edit(opcode, payload, size);
    }
    return Status::Unsupported;
}

uint32_t LitchcraftSession::ConnectionRevision() const {
    // Tool can coalesce a pending edit checkpoint; keep its Store authority
    // in the live revision domain even after a persistence-only save.
    if (HasStreamedDocument()) return m_frameStream->ReadLiveAuthority().revision;
    return HasAdvancedDocument() ? m_advancedPersistence->AdvancedGeneration()
                                 : m_revision;
}

litchcraft_frame_stream::FrameStream::LiveAuthority
LitchcraftSession::StoreLiveAuthority() const {
    return m_frameStream == nullptr
        ? litchcraft_frame_stream::FrameStream::LiveAuthority{}
        : m_frameStream->ReadLiveAuthority();
}

LitchcraftSession::PhysicalEditScope::PhysicalEditScope(LitchcraftSession& owner)
    : session(owner) {
    session.m_advancedPhysicalCommand = true;
    session.m_advancedCommandChanged = false;
    session.m_advancedCommandHasHistory = false;
}
LitchcraftSession::PhysicalEditScope::~PhysicalEditScope() {
    session.m_advancedPhysicalCommand = false;
}

void LitchcraftSession::RecordAdvancedMutation(const uint8_t beforeFrame,
                                               const bool extend) {
#if defined(LITCH_ESP32S3)
    (void)beforeFrame;
    (void)extend;
#else
    auto& owner = *m_advancedPersistence;
    m_strokeChanged = m_advancedHistory.Record(owner.m_advanced->staging,
        owner.AdvancedDocument().Value(), beforeFrame, m_advancedSelectedFrame,
        extend || (m_advancedPhysicalCommand && m_advancedCommandHasHistory));
    if (!m_advancedPhysicalCommand || !m_advancedCommandChanged) ++owner.m_advancedGeneration;
    if (m_advancedPhysicalCommand) {
        m_advancedCommandChanged = true;
        m_advancedCommandHasHistory = m_strokeChanged;
    }
    owner.m_advancedChangedAtMs = fxe::Millis();
    m_activity = true;
    SyncAdvancedCanvas();
#endif
}

bool LitchcraftSession::CommitAdvancedEdit(const uint8_t beforeFrame,
                                          const bool extend) {
    auto& storage = *m_advancedPersistence->m_advanced;
    if (DocumentsEqual(storage.staging, storage.document.Value())) return false;
    if (!ValidatedDocument::TryAdoptBySwap(storage.staging, storage.document)) return false;
    RecordAdvancedMutation(beforeFrame, extend);
    return true;
}

LitchcraftSession::Status LitchcraftSession::ApplyV2Edit(
    const uint8_t op, const uint8_t* const payload, const size_t size) {
    if (HasStreamedDocument()) return Status::Busy;
    if (!HasAdvancedDocument()) return Status::Busy;
    InitializeAdvancedRuntime();
    if (op == 3U || op == 4U) return ApplyV2Stroke(op, payload, size);
    if (op == 5U) return ApplyV2FrameEdit(payload, size);
    if (op == 6U || op == 7U) {
        if (size != 0U) return Status::Malformed;
        return AdvancedUndoRedo(op == 7U);
    }
    if (size != 2U || payload[0] > 2U ||
        payload[1] >= m_advancedPersistence->AdvancedDocument().Value().frameCount) return Status::Malformed;
    const auto action = payload[0];
    // Sampling runtime time is harmless, but an invalid command cannot pause
    // or seek it. Selection/play wait until a held brush reaches its boundary.
    SampleAdvancedPlayback();
    // Pause names the frame visible at the initiating input. Delivery across
    // a frame boundary must not choose a later board frame instead.
    if (action == 1U && payload[1] != m_advancedSelectedFrame) return Status::Malformed;
    if (m_strokeSource != Source::None && action != 0U) return Status::Busy;
    if (action == 2U) SelectAdvancedFrame(payload[1]);
    else if (action == 0U) {
        PauseAdvancedPlayback();
        if (payload[1] != m_advancedSelectedFrame) SelectAdvancedFrame(payload[1]);
    }
    else { SetAnimationPlaying(true); m_advancedClock = fxe::Millis(); }
    return Status::Ok;
}

LitchcraftSession::Status LitchcraftSession::ApplyStoreRuntime(
    const uint8_t* const payload, const size_t size) {
    // Store Runtime: expected committed generation:u32, action:u8,
    // selected frame:u16. This is intentionally distinct from schema-3 op 2:
    // it can name all 256 frames and has Store generation authority.
    if (!HasStreamedDocument()) return Status::Unsupported;
    if (m_frameStream->ReplacementPending()) return Status::Busy;
    if (size != 7U || payload[4] > 2U) return Status::Malformed;
    if (!m_frameStream->AcceptsGeneration(Read32(payload))) {
        return Status::RevisionConflict;
    }
    const uint16_t frame = Read16(payload + 5U);
    if (frame >= m_frameStream->FrameCount()) return Status::Malformed;
    const uint8_t action = payload[4];
    m_pendingPhysicalHistoryFrame = UINT16_MAX;
    SampleStreamedPlayback();
    if (m_strokeSource != Source::None && action != 0U) return Status::Busy;
    const uint32_t now = fxe::Millis();
    if (action == 0U) {
        PinStreamedFrameForEdit();
        if (frame != m_advancedSelectedFrame) {
            m_streamFrameStarted = false;
            m_streamAwaitingPlaybackFrame = false;
            m_advancedElapsed = 0U;
            m_advancedSelectedFrame = static_cast<uint8_t>(frame);
            (void)m_frameStream->Request(frame);
        }
    } else if (action == 1U) {
        if (frame != m_advancedSelectedFrame) return Status::RevisionConflict;
        if (m_streamFrameStarted && !m_advancedPlaying) {
            // A pause freezes this displayed frame's remaining dwell; wall
            // time while paused must never consume its next boundary.
            m_streamFrameStartedAtMs += now - m_streamPausedAtMs;
        }
        m_streamPausedAtMs = 0U;
        SetAnimationPlaying(true);
        m_advancedClock = now;
    } else {
        SetAnimationPlaying(false);
        m_streamPausedAtMs = now;
        m_streamFrameStarted = false;
        m_streamAwaitingPlaybackFrame = false;
        m_advancedElapsed = 0U;
        m_advancedSelectedFrame = static_cast<uint8_t>(frame);
        (void)m_frameStream->Request(frame);
        WakeStreamedStorage();
    }
    WakeStreamedStorage();
    return Status::Ok;
}

LitchcraftSession::Status LitchcraftSession::ApplyOptimisticEdit(
    const uint8_t op, const uint16_t sequence, const uint8_t* const payload,
    const size_t size) {
    if (!m_optimisticActive || m_optimisticResync ||
        sequence != static_cast<uint16_t>(m_optimisticHighestSequence + 1U) ||
        payload == nullptr || size == 0U || payload[0] != m_optimisticFrame) {
        return Status::StaleRequest;
    }
    const Status status = ApplyV2Stroke(op == 24U ? 24U : 25U, payload, size);
    if (status == Status::Ok) {
        m_optimisticHighestSequence = sequence;
        ++m_optimisticAcceptedSinceWatermark;
    }
    return status;
}

LitchcraftSession::Status LitchcraftSession::ApplyV2Stroke(
    const uint8_t op, const uint8_t* const payload, const size_t size) {
    const bool paint = op == 3U || op == 24U;
    const bool optimistic = op == 24U || op == 25U;
    if (size < 8U || size > 11U) return Status::Malformed;
    const auto frame = payload[0];
    const auto stroke = Read16(payload + (paint ? 1U : 5U));
    const auto flags = payload[paint ? 3U : 7U];
    const auto mask = payload[4];
    if (frame >= m_advancedPersistence->AdvancedDocument().Value().frameCount ||
        stroke == 0U || flags > (paint ? (optimistic ? 7U : 3U) : 3U) ||
        mask == 0U || mask > 15U) return Status::Malformed;
    if (!paint) {
        if (!IsValidEffect(payload[1])) return Status::Unsupported;
        if (!IsValidEffectDuration(static_cast<Effect>(payload[1]), Read16(payload + 2U))) return Status::Malformed;
    }
    for (size_t i = 8U; i < size; ++i) if (payload[i] >= 64U) return Status::Malformed;
    if (optimistic && (flags & 4U) != 0U &&
        (payload[5] != 0U || payload[6] != 0U || payload[7] != 0U)) {
        return Status::Malformed;
    }
    const bool continuing = m_strokeSource == Source::App && m_strokeId == stroke;
    if (continuing && frame != m_advancedStrokeFrame) return Status::Malformed;
    if (m_strokeSource == Source::Board) {
        // A physical brush owns its latched frame until an accepted edit
        // crosses the stroke boundary. An app Paint/Effect on that same frame
        // is such a boundary; a different frame must not redirect the brush.
        if (frame != m_advancedStrokeFrame) return Status::Busy;
        EndStroke();
    }
    if ((flags & 1U) != 0U) EndStroke();
    if (m_strokeSource != Source::App || m_strokeId != stroke) {
        EndStroke();
        PauseAdvancedPlayback();
        if (frame != m_advancedSelectedFrame) SelectAdvancedFrame(frame);
        StartStroke(Source::App, stroke);
        if (!paint && m_supportActions != nullptr) m_supportActions->SetKind(m_supportStroke, trace::SupportActionKind::Effect);
    }
    auto& storage = *m_advancedPersistence->m_advanced;
    storage.staging = storage.document.Value();
    for (size_t i = 8U; i < size; ++i) {
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            if ((mask & (1U << slot)) == 0U) continue;
            auto& emitter = storage.staging.frames[frame].emitters[payload[i] * 4U + slot];
            if (paint) {
                emitter.color = {payload[5], payload[6], payload[7]};
                if ((flags & 4U) != 0U) {
                    emitter.effect = Effect::Off;
                    emitter.durationMs = 0U;
                }
            }
            else { emitter.effect = static_cast<Effect>(payload[1]); emitter.durationMs = Read16(payload + 2U); }
        }
    }
    (void)CommitAdvancedEdit(frame, m_strokeChanged);
    if (m_strokeChanged) {
        for (size_t i = 8U; i < size; ++i) m_supportStrokeSquares |= UINT64_C(1) << payload[i];
    }
    if ((flags & 2U) != 0U) EndStroke();
    return Status::Ok;
}

LitchcraftSession::Status LitchcraftSession::ApplyV2FrameEdit(
    const uint8_t* const payload, const size_t size) {
    if (size != 9U) return Status::Malformed;
    const auto action = payload[4];
    const auto source = payload[5];
    const auto destination = payload[6];
    const auto duration = Read16(payload + 7U);
    auto& storage = *m_advancedPersistence->m_advanced;
    const auto count = storage.document.Value().frameCount;
    if (action > 3U || source >= count || (action != 2U && destination != 0U) ||
        (action != 3U && duration != 0U) ||
        (action == 3U && !IsValidFrameDuration(duration)) ||
        (action == 2U && destination >= count)) return Status::Malformed;
    if (Read32(payload) != m_advancedPersistence->AdvancedGeneration()) return Status::RevisionConflict;
    if (m_strokeSource != Source::None) return Status::Busy;
    SampleAdvancedPlayback();
    if (action == 0U && source != m_advancedSelectedFrame) return Status::Malformed;
    if ((action == 0U && count == MAX_FRAME_COUNT) || (action == 1U && count == MIN_FRAME_COUNT)) return Status::ResourceLimit;
    PauseAdvancedPlayback();
    const auto before = m_advancedSelectedFrame;
    auto selected = before;
    storage.staging = storage.document.Value();
    auto& candidate = storage.staging;
    if (action == 0U) {
        for (uint8_t i = count; i > source + 1U; --i) candidate.frames[i] = candidate.frames[i - 1U];
        candidate.frames[source + 1U] = candidate.frames[source];
        ++candidate.frameCount;
        selected = source + 1U;
    } else if (action == 1U) {
        for (uint8_t i = source; i + 1U < count; ++i) candidate.frames[i] = candidate.frames[i + 1U];
        --candidate.frameCount;
        selected = source == 0U ? 0U : source - 1U;
    } else if (action == 2U) {
        // Read the moved frame from live authority; no frame/body scratch.
        if (source < destination) {
            for (uint8_t i = source; i < destination; ++i) candidate.frames[i] = candidate.frames[i + 1U];
        } else {
            for (uint8_t i = source; i > destination; --i) candidate.frames[i] = candidate.frames[i - 1U];
        }
        candidate.frames[destination] = storage.document.Value().frames[source];
        if (before == source) selected = destination;
        else if (source < before && destination >= before) --selected;
        else if (source > before && destination <= before) ++selected;
    } else candidate.frames[source].durationMs = duration;
    m_advancedSelectedFrame = selected;
    (void)CommitAdvancedEdit(before, false);
    SelectAdvancedFrame(selected);
    return Status::Ok;
}

LitchcraftSession::Status LitchcraftSession::ApplyV2CanvasEdit(
    const uint8_t* const payload, const size_t size) {
    if (size != 10U || payload[5] == 0U || payload[5] > 15U || payload[9] > 8U) {
        return Status::Malformed;
    }
    const bool erase = payload[9] == 1U;
    const bool rotate = payload[9] >= 2U;
    const bool checker = payload[9] >= 5U;
    const bool checkerErase = payload[9] >= 7U;
    if ((!checker && (erase || rotate) &&
         (payload[5] != 15U || payload[6] != 0U || payload[7] != 0U || payload[8] != 0U)) ||
        (checkerErase && (payload[6] != 0U || payload[7] != 0U || payload[8] != 0U))) {
        return Status::Malformed;
    }
    if (!HasAdvancedDocument()) return Status::Busy;
    InitializeAdvancedRuntime();
    auto& owner = *m_advancedPersistence;
    auto& storage = *owner.m_advanced;
    const auto frame = payload[4];
    if (frame >= storage.document.Value().frameCount) return Status::Malformed;
    if (Read32(payload) != owner.AdvancedGeneration()) return Status::RevisionConflict;
    if (m_strokeSource != Source::None || m_v2EnvelopeLeased) return Status::Busy;
    SampleAdvancedPlayback();
    // A playback boundary or a physical frame selection cannot redirect this
    // edit. Rejection happens before pausing, history, or document mutation.
    if (frame != m_advancedSelectedFrame) return Status::RevisionConflict;
    PauseAdvancedPlayback();
    storage.staging = storage.document.Value();
    if (rotate && !checker) {
        const uint8_t turns = payload[9] - 1U;
        const auto& source = storage.document.Value().frames[frame];
        for (uint8_t square = 0U; square < 64U; ++square) {
            uint8_t destination = square;
            for (uint8_t turn = 0U; turn < turns; ++turn) {
                destination = (destination % 8U) * 8U + 7U - destination / 8U;
            }
            for (uint8_t slot = 0U; slot < 4U; ++slot) {
                storage.staging.frames[frame].emitters[
                    destination * 4U + (slot + turns) % 4U] =
                    source.emitters[square * 4U + slot];
            }
        }
        (void)CommitAdvancedEdit(frame, false);
        return Status::Ok;
    }
    for (size_t index = 0U; index < EMITTER_COUNT; ++index) {
        if (checker) {
            const uint8_t square = static_cast<uint8_t>(index / 4U);
            const bool dark = ((square % 8U) + (square / 8U)) % 2U != 0U;
            if (dark != ((payload[9] & 1U) == 0U)) continue;
        }
        if ((payload[5] & (1U << (index % 4U))) == 0U) continue;
        auto& emitter = storage.staging.frames[frame].emitters[index];
        emitter.color = {payload[6], payload[7], payload[8]};
        if (erase || checkerErase) { emitter.effect = Effect::Off; emitter.durationMs = 0U; }
    }
    (void)CommitAdvancedEdit(frame, false);
    return Status::Ok;
}

LitchcraftSession::Status LitchcraftSession::AdvancedUndoRedo(const bool redo) {
#if defined(LITCH_ESP32S3)
    (void)redo;
    return Status::Unsupported;
#else
    InitializeAdvancedRuntime();
    EndStroke();
    if ((redo ? m_advancedHistory.RedoCount() : m_advancedHistory.UndoCount()) == 0U) return Status::NoHistory;
    PauseAdvancedPlayback();
    auto& owner = *m_advancedPersistence;
    auto& storage = *owner.m_advanced;
    storage.staging = storage.document.Value();
    auto selected = m_advancedSelectedFrame;
    if (redo) (void)m_advancedHistory.Redo(storage.staging, selected);
    else (void)m_advancedHistory.Undo(storage.staging, selected);
    if (!ValidatedDocument::TryAdoptBySwap(storage.staging, storage.document)) return Status::IntegrityFailure;
    ++owner.m_advancedGeneration;
    owner.m_advancedChangedAtMs = fxe::Millis();
    SelectAdvancedFrame(selected);
    m_activity = true;
    return Status::Ok;
#endif
}

void LitchcraftSession::PaintAdvancedFootprint(const uint64_t footprint) {
    auto& storage = *m_advancedPersistence->m_advanced;
    storage.staging = storage.document.Value();
    const auto color = m_erase ? RGB_BLACK : m_color;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((footprint & (UINT64_C(1) << square)) == 0U) continue;
        for (uint8_t slot = 0U; slot < 4U; ++slot) {
            if ((m_target & (1U << slot)) == 0U) continue;
            auto& emitter = storage.staging.frames[m_advancedStrokeFrame].emitters[square * 4U + slot];
            emitter.color = color;
            if (m_erase) {
                emitter.effect = Effect::Off;
                emitter.durationMs = 0U;
            }
        }
    }
    (void)CommitAdvancedEdit(m_advancedStrokeFrame, m_strokeChanged);
}

void LitchcraftSession::CompleteAdvancedColorPick(const Rgb sample) {
    auto& storage = *m_advancedPersistence->m_advanced;
    storage.staging = storage.document.Value();
    for (uint8_t slot = 0U; slot < 4U; ++slot) {
        storage.staging.frames[m_advancedStrokeFrame].emitters[m_pickSquare * 4U + slot].color = m_pickBefore[slot];
    }
    (void)CommitAdvancedEdit(m_advancedStrokeFrame, m_strokeChanged);
    EndStroke();
    const bool black = sample.r == 0U && sample.g == 0U && sample.b == 0U;
    SelectTool(black, m_target, black ? m_color : sample, 0U);
    m_pickConfirmed = true;
    m_pickConfirmedMs = fxe::Millis();
    Ack(0U);
    PublishColorPickStatus();
}

void LitchcraftSession::UpdateAdvancedFavorites() {
    const auto requested = m_recentColors;
    PauseAdvancedPlayback();
    auto& storage = *m_advancedPersistence->m_advanced;
    storage.staging = storage.document.Value();
    storage.staging.favorites = requested;
    (void)CommitAdvancedEdit(m_advancedSelectedFrame, false);
    SyncAdvancedCanvas();
}
} // namespace modules
