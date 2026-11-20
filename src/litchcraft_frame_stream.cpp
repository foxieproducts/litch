#include "modules/litchcraft_frame_stream.hpp"

#include <algorithm>

#if defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "foxie_embedded/millis.hpp"

namespace modules::litchcraft_frame_stream {
namespace store = litchcraft_frame_store;

namespace {
uint32_t NextLiveRevision(uint32_t revision) {
    // Zero is an invalid wire base. Exhaustion stays invalid for this boot,
    // so a wrapped token cannot authorize replay.
    return revision == 0U || revision == UINT32_MAX ? 0U : revision + 1U;
}
} // namespace

bool FrameStream::RetireDocument(const RetirementScope scope) {
    LockPublication();
    if (m_replacementReserved ||
        (scope == RetirementScope::EmptyOnly &&
         (Generation() != 0U || FrameCount() != 0U || HasPendingBlankDocument()))) {
        UnlockPublication();
        return false;
    }
    m_blankDocumentPending.store(false, std::memory_order_release);
    const uint32_t generation = Generation();
    if (generation != 0U) {
        RetirePresentationLocked();
        m_publishedPresentationContext = 0U;
        m_history.Clear();
        m_structuralEdit = {};
        m_persistCount.store(0U);
        m_historyFrame = NO_FRAME;
        m_retiredGeneration.store(generation, std::memory_order_release);
        m_retirementState.store(RetirementState::Bound, std::memory_order_release);
        AdvanceLiveRevision();
        UnlockPublication();
        return true;
    }
    const auto state = m_retirementState.load(std::memory_order_acquire);
    if (state == RetirementState::None) {
        m_history.Clear();
        m_structuralEdit = {};
        m_persistCount.store(0U);
        m_historyFrame = NO_FRAME;
        m_retirementState.store(RetirementState::AwaitingRoot, std::memory_order_release);
        AdvanceLiveRevision();
        UnlockPublication();
        return true;
    }
    UnlockPublication();
    return state == RetirementState::AwaitingRoot;
}

void FrameStream::BindRetirementGeneration(const uint32_t generation) {
    LockPublication();
    if (m_retirementState.load(std::memory_order_acquire) ==
        RetirementState::AwaitingRoot) {
        m_retiredGeneration.store(generation, std::memory_order_release);
        m_retirementState.store(RetirementState::Bound, std::memory_order_release);
    }
    UnlockPublication();
}

bool FrameStream::AdvanceDurableRetirementGeneration(const uint32_t generation) {
    LockPublication();
    const auto state = m_retirementState.load(std::memory_order_acquire);
    const uint32_t retired = RetiredGeneration();
    const bool valid = generation != 0U && state != RetirementState::None &&
        (retired == 0U || static_cast<int32_t>(generation - retired) > 0);
    if (valid) {
        m_retiredGeneration.store(generation, std::memory_order_release);
        m_retirementState.store(RetirementState::Bound, std::memory_order_release);
    }
    UnlockPublication();
    return valid;
}

void FrameStream::Clear() {
    LockPublication();
    if (m_replacementReserved) { UnlockPublication(); return; }
    m_blankDocumentPending.store(false, std::memory_order_release);
    m_history.Clear();
    m_structuralEdit = {};
    ResetFrameIdentities(0U);
    m_historyFrame = NO_FRAME;
    m_activeSlot.store(NO_SLOT, std::memory_order_release);
    for (auto& index : m_indices) {
        index.store(NO_FRAME, std::memory_order_release);
    }
    for (auto& generation : m_generations) {
        generation.store(0U, std::memory_order_release);
    }
    m_authorityGeneration.store(0U, std::memory_order_release);
    m_editSequence.store(0U, std::memory_order_release);
    for (auto& sequence : m_slotEditSequence) {
        sequence.store(0U, std::memory_order_release);
    }
    for (auto& sequence : m_slotPersistedSequence) {
        sequence.store(0U, std::memory_order_release);
    }
    m_lastEditAtMs.store(0U, std::memory_order_release);
    m_saveImmediately.store(false, std::memory_order_release);
    m_scopeBaseRevision = 0U;
    m_scope = store::MutationScope::Unavailable;
    m_scopeIndex = 0U;
    m_scopeAux = 0U;
    const uint32_t priorLiveRevision =
        m_liveRevision.load(std::memory_order_relaxed);
    if (priorLiveRevision != 0U) {
        AdvanceLiveRevision();
    }
    uint32_t epoch = m_documentEpoch.load(std::memory_order_relaxed) + 1U;
    if (epoch == 0U) epoch = 1U;
    m_documentEpoch.store(epoch, std::memory_order_release);
    UnlockPublication();
    m_failedIndex.store(NO_FRAME, std::memory_order_release);
    m_failedGeneration.store(0U, std::memory_order_release);
    m_failedStatus.store(static_cast<uint8_t>(store::Status::Ok),
                         std::memory_order_release);
    m_readCursor = {};
    m_loadingIndex = NO_FRAME;
    m_loadingGeneration = 0U;
    m_frameCount.store(0U, std::memory_order_release);
    m_requested.store(NO_FRAME, std::memory_order_release);
    m_documentBaseGeneration.store(0U, std::memory_order_release);
}

bool FrameStream::Configure(const store::Manifest& manifest,
                            const uint32_t generation,
                            const uint16_t firstFrame) {
    return AdoptDocument(manifest, generation, firstFrame, nullptr).kind ==
        store::PresentationAdmission::Kind::Accepted;
}

store::PresentationAdmission FrameStream::TryReserveReplacement(
    const uint32_t expectedCurrentGeneration,
    const store::Manifest& manifest, const uint32_t generation) {
    using Kind = store::PresentationAdmission::Kind;
    if (manifest.frameCount == 0U || manifest.frameCount > store::FRAME_COUNT ||
        manifest.topology < 1U || manifest.topology > 3U ||
        manifest.sparkleSeed == 0U || generation == 0U) return {};
    LockPublication();
    const uint32_t retired = RetiredGeneration();
    const bool dirty = HasUnpersistedEdit();
    const bool compatible = !m_replacementReserved && !HasPendingBlankDocument() &&
        m_historyFrame == NO_FRAME &&
        m_retirementState.load(std::memory_order_acquire) != RetirementState::AwaitingRoot &&
        (retired == 0U || static_cast<int32_t>(generation - retired) > 0) &&
        Generation() == expectedCurrentGeneration && !dirty;
    if (!compatible) {
        UnlockPublication();
        return {Kind::Rejected, dirty};
    }
    if (HasPresentationCandidate()) {
        UnlockPublication();
        return {Kind::PendingPresentation, false};
    }
    RetirePresentationLocked();
    m_replacementReserved = true;
    m_reservedPreviousGeneration = expectedCurrentGeneration;
    m_reservedGeneration = generation;
    UnlockPublication();
    return {Kind::Accepted, false};
}

bool FrameStream::PublishReservedReplacement(
    const uint32_t expectedCurrentGeneration,
    const store::Manifest& manifest, const uint32_t generation,
    const store::Frame& firstFrame) {
    LockPublication();
    const bool reserved = m_replacementReserved &&
        m_reservedPreviousGeneration == expectedCurrentGeneration &&
        m_reservedGeneration == generation;
    UnlockPublication();
    if (!reserved || AdoptDocument(manifest, generation, 0U, &firstFrame,
                                   expectedCurrentGeneration).kind !=
            store::PresentationAdmission::Kind::Accepted) return false;
    LockPublication();
    m_replacementReserved = false;
    m_reservedPreviousGeneration = 0U;
    m_reservedGeneration = 0U;
    UnlockPublication();
    return true;
}

bool FrameStream::ReplacementPending() const {
    LockPublication();
    const bool pending = m_replacementReserved;
    UnlockPublication();
    return pending;
}

store::Status FrameStream::SaveModeRoot(store::FrameStore& frames,
                                        const bool active) {
    LockPublication();
    const uint32_t previousGeneration = Generation();
    const uint32_t documentEpoch =
        m_documentEpoch.load(std::memory_order_acquire);
    if (m_replacementReserved || previousGeneration == 0U ||
        previousGeneration != frames.Generation() ||
        HasUnpersistedEdit()) {
        UnlockPublication();
        return store::Status::RevisionConflict;
    }
    UnlockPublication();
    // The flash arbiter may wait for the next render generation. Rendering
    // reads the published frame under this lock, so the root write must not.
    const store::Status saved = frames.UpdateModeActive(
        active, previousGeneration);
    if (saved != store::Status::Ok) return saved;
    const uint32_t generation = frames.Generation();
    LockPublication();
    if (Generation() != previousGeneration ||
        m_documentEpoch.load(std::memory_order_acquire) != documentEpoch) {
        UnlockPublication();
        return store::Status::RevisionConflict;
    }
    for (uint8_t slot = 0U; slot < m_frames.size(); ++slot) {
        if (m_generations[slot].load(std::memory_order_acquire) ==
            previousGeneration) {
            m_generations[slot].store(generation, std::memory_order_release);
        }
    }
    m_documentActive.store(active, std::memory_order_release);
    m_authorityGeneration.store(generation, std::memory_order_release);
    // Mode persistence changes saved authority, not the live artwork. Keep
    // its content revision and mutation coverage through Exit and re-entry.
    UnlockPublication();
    return store::Status::Ok;
}

bool FrameStream::AcceptsGeneration(const uint32_t generation) const {
    const uint32_t first = m_documentBaseGeneration.load(std::memory_order_acquire);
    const uint32_t current = Generation();
    return first != 0U && current != 0U &&
        static_cast<int32_t>(generation - first) >= 0 &&
        static_cast<int32_t>(current - generation) >= 0;
}

store::PresentationAdmission FrameStream::AdoptDocument(const store::Manifest& manifest,
                                const uint32_t generation,
                                const uint16_t firstFrameIndex,
                                const store::Frame* const preparedFirstFrame,
                                const std::optional<uint32_t>
                                    expectedCurrentGeneration) {
    using Kind = store::PresentationAdmission::Kind;
    if (manifest.frameCount == 0U || manifest.frameCount > store::FRAME_COUNT ||
        manifest.topology < 1U || manifest.topology > 3U ||
        manifest.sparkleSeed == 0U || firstFrameIndex >= manifest.frameCount ||
        generation == 0U) return {};
    LockPublication();
    const uint32_t retired = RetiredGeneration();
    const bool dirty = HasUnpersistedEdit();
    if (HasPendingBlankDocument() || HasPendingStructuralSave() ||
        (m_replacementReserved && !expectedCurrentGeneration.has_value()) ||
        m_retirementState.load(std::memory_order_acquire) == RetirementState::AwaitingRoot ||
        (retired != 0U && static_cast<int32_t>(generation - retired) <= 0) ||
        (expectedCurrentGeneration.has_value() && Generation() != *expectedCurrentGeneration) ||
        (preparedFirstFrame != nullptr && dirty)) {
        UnlockPublication();
        return {Kind::Rejected, dirty};
    }
    if (HasPresentationCandidate()) {
        UnlockPublication();
        return {Kind::PendingPresentation, false};
    }
    m_readCursor = {};
    m_loadingIndex = NO_FRAME;
    m_loadingGeneration = 0U;
    m_history.Clear();
    m_structuralEdit = {};
    m_historyFrame = NO_FRAME;
    uint8_t target = NO_SLOT;
    if (preparedFirstFrame == nullptr) {
        m_activeSlot.store(NO_SLOT, std::memory_order_release);
        for (auto& index : m_indices) index.store(NO_FRAME, std::memory_order_release);
        for (auto& slotGeneration : m_generations) {
            slotGeneration.store(0U, std::memory_order_release);
        }
    } else {
        const uint8_t active = m_activeSlot.load(std::memory_order_acquire);
        target = active < m_frames.size()
            ? static_cast<uint8_t>((active + 1U) % m_frames.size()) : 0U;
        m_sequences[target].fetch_add(1U, std::memory_order_acq_rel);
        m_indices[target].store(NO_FRAME, std::memory_order_release);
        m_frames[target] = *preparedFirstFrame;
        m_generations[target].store(generation, std::memory_order_release);
        m_indices[target].store(firstFrameIndex, std::memory_order_release);
        m_sequences[target].fetch_add(1U, std::memory_order_release);
    }
    m_failedIndex.store(NO_FRAME, std::memory_order_release);
    m_failedGeneration.store(0U, std::memory_order_release);
    m_failedStatus.store(static_cast<uint8_t>(store::Status::Ok),
                         std::memory_order_release);
    m_sparkleSeed.store(manifest.sparkleSeed, std::memory_order_release);
    m_topology.store(manifest.topology, std::memory_order_release);
    m_documentActive.store(manifest.active, std::memory_order_release);
    m_playbackOnEntry.store(manifest.playbackOnEntry,
                            std::memory_order_release);
    m_frameCount.store(manifest.frameCount, std::memory_order_release);
    ResetFrameIdentities(manifest.frameCount);
    m_requested.store(firstFrameIndex, std::memory_order_release);
    m_documentBaseGeneration.store(generation, std::memory_order_release);
    m_editSequence.store(0U, std::memory_order_release);
    for (auto& sequence : m_slotEditSequence) {
        sequence.store(0U, std::memory_order_release);
    }
    for (auto& sequence : m_slotPersistedSequence) {
        sequence.store(0U, std::memory_order_release);
    }
    m_lastEditAtMs.store(0U, std::memory_order_release);
    m_saveImmediately.store(false, std::memory_order_release);
    m_scopeBaseRevision = 0U;
    m_scope = store::MutationScope::Unavailable;
    m_scopeIndex = 0U;
    m_scopeAux = 0U;
    const uint32_t priorLiveRevision =
        m_liveRevision.load(std::memory_order_relaxed);
    if (m_liveRevisionExhausted) {
        m_liveRevision.store(0U, std::memory_order_release);
    } else if (priorLiveRevision == 0U) {
        m_liveRevision.store(generation, std::memory_order_release);
    } else {
        AdvanceLiveRevision();
    }
    uint32_t epoch = m_documentEpoch.load(std::memory_order_relaxed) + 1U;
    if (epoch == 0U) epoch = 1U;
    m_documentEpoch.store(epoch, std::memory_order_release);
    if (target != NO_SLOT) {
        m_activeSlot.store(target, std::memory_order_release);
        m_presentationCount.fetch_add(1U, std::memory_order_release);
    }
    m_authorityGeneration.store(generation, std::memory_order_release);
    CompleteInitialRecovery();
    UnlockPublication();
    return {Kind::Accepted, false};
}

bool FrameStream::Request(const uint16_t frameIndex) {
    LockPublication();
    const bool ready = (!m_replacementReserved ||
        (m_structuralEdit.running && m_structuralEdit.cancelled)) && frameIndex < FrameCount();
    if (ready) m_requested.store(frameIndex, std::memory_order_release);
    UnlockPublication();
    return ready;
}

bool FrameStream::TryPresentRequested() {
    LockPublication();
    if (Generation() == 0U || m_replacementReserved) {
        UnlockPublication();
        return false;
    }
    const uint16_t requested = m_requested.load(std::memory_order_acquire);
    const uint32_t generation =
        m_authorityGeneration.load(std::memory_order_acquire);
    const uint8_t active = m_activeSlot.load(std::memory_order_acquire);
    if (active < m_frames.size() &&
        PresentationReadyLocked() &&
        m_generations[active].load(std::memory_order_acquire) == generation &&
        m_indices[active].load(std::memory_order_acquire) == requested) {
        UnlockPublication();
        return true;
    }
    for (uint8_t slot = 0U; slot < m_frames.size(); ++slot) {
        if (slot != active &&
            !m_confirmedPresentation &&
            (m_sequences[slot].load(std::memory_order_acquire) & 1U) == 0U &&
            m_generations[slot].load(std::memory_order_acquire) == generation &&
            m_indices[slot].load(std::memory_order_acquire) == requested) {
            m_activeSlot.store(slot, std::memory_order_release);
            m_presentationCount.fetch_add(1U, std::memory_order_release);
            UnlockPublication();
            return true;
        }
    }
    UnlockPublication();
    return false;
}

bool FrameStream::CopyDisplayed(store::Frame& out, uint16_t& outIndex) const {
    if (Generation() == 0U) return false;
    LockPublication();
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    if (slot >= m_frames.size()) {
        UnlockPublication();
        return false;
    }
    if (!PresentationReadyLocked()) {
        UnlockPublication();
        return false;
    }
    out = m_frames[slot];
    outIndex = m_indices[slot].load(std::memory_order_acquire);
    UnlockPublication();
    return true;
}

uint16_t FrameStream::DisplayedIndex() const {
    LockPublication();
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    const uint16_t index = slot < m_frames.size() && PresentationReadyLocked()
        ? m_indices[slot].load(std::memory_order_acquire)
        : NO_FRAME;
    UnlockPublication();
    return index;
}

uint32_t FrameStream::DisplayedFingerprint() const {
    uint32_t fingerprint = 2166136261U;
    LockPublication();
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    if (slot < m_frames.size() && PresentationReadyLocked()) {
        const auto& frame = m_frames[slot];
        fingerprint = (fingerprint ^ frame.durationMs) * 16777619U;
        for (const auto& emitter : frame.emitters) {
            fingerprint = (fingerprint ^ emitter.color.r) * 16777619U;
            fingerprint = (fingerprint ^ emitter.color.g) * 16777619U;
            fingerprint = (fingerprint ^ emitter.color.b) * 16777619U;
            fingerprint = (fingerprint ^ emitter.effect) * 16777619U;
            fingerprint =
                (fingerprint ^ emitter.effectDurationMs) * 16777619U;
        }
    } else {
        fingerprint = 0U;
    }
    UnlockPublication();
    return fingerprint;
}

bool FrameStream::CopyDisplayedSquare(
    const uint8_t square, std::array<store::Emitter, 4U>& out,
    uint16_t& outIndex, uint16_t& outDurationMs) const {
    if (square >= 64U || Generation() == 0U) return false;
    LockPublication();
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    if (slot >= m_frames.size()) {
        UnlockPublication();
        return false;
    }
    if (!PresentationReadyLocked()) {
        UnlockPublication();
        return false;
    }
    const auto& frame = m_frames[slot];
    outDurationMs = frame.durationMs;
    const size_t base = static_cast<size_t>(square) * out.size();
    std::copy_n(frame.emitters.data() + base, out.size(), out.data());
    outIndex = m_indices[slot].load(std::memory_order_acquire);
    UnlockPublication();
    return true;
}

bool FrameStream::RestoreDisplayedSquare(
    const uint32_t expectedDocumentEpoch, const uint16_t frameIndex,
    const uint8_t square,
    const std::array<store::Emitter, 4U>& emitters) {
    if (square >= 64U || Generation() == 0U) return false;
    LockPublication();
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    if (m_replacementReserved ||
        m_documentEpoch.load(std::memory_order_relaxed) !=
            expectedDocumentEpoch ||
        slot >= m_frames.size() ||
        m_indices[slot].load(std::memory_order_acquire) != frameIndex ||
        m_generations[slot].load(std::memory_order_acquire) != Generation()) {
        UnlockPublication();
        return false;
    }
    bool changed = false;
    auto& frame = m_frames[slot];
    for (uint8_t index = 0U; index < 4U; ++index) {
        auto& target = frame.emitters[static_cast<size_t>(square) * 4U + index];
        const auto& before = emitters[index];
        if (target.color.r == before.color.r &&
            target.color.g == before.color.g &&
            target.color.b == before.color.b &&
            target.effect == before.effect &&
            target.effectDurationMs == before.effectDurationMs) {
            continue;
        }
        target = before;
        changed = true;
    }
    if (changed) {
        m_lastEditAtMs.store(fxe::Millis(), std::memory_order_release);
        RecordEdit(slot);
        AdvanceLiveRevision(store::MutationScope::Frame, m_indices[slot].load());
    }
    UnlockPublication();
    return changed;
}

FrameStream::LiveAuthority FrameStream::ReadLiveAuthority() const {
    LockPublication();
    const LiveAuthority authority{Generation(),
                                  m_liveRevision.load(std::memory_order_relaxed),
                                  HasUnpersistedEdit()};
    UnlockPublication();
    return authority;
}

bool FrameStream::PaintDisplayed(const uint16_t frameIndex,
                                 const uint8_t square, const uint8_t mask,
                                 const Rgb color, const bool clearEffects) {
    return PaintDisplayedImpl(std::nullopt, frameIndex, &square, 1U, mask, color,
                              clearEffects) == GuardedEditResult::Changed;
}

FrameStream::GuardedEditResult FrameStream::PaintDisplayedIfRevision(
    const uint32_t expectedRevision, const uint16_t frameIndex,
    const uint8_t square, const uint8_t mask, const Rgb color,
    const bool clearEffects) {
    return PaintDisplayedImpl(expectedRevision, frameIndex, &square, 1U, mask,
                              color, clearEffects);
}

FrameStream::GuardedEditResult FrameStream::PaintDisplayedBatchIfRevision(
    const uint32_t expectedRevision, const uint16_t frameIndex,
    const uint8_t* const squares, const size_t squareCount, const uint8_t mask,
    const Rgb color, const bool clearEffects, uint32_t* const resultingRevision) {
    return PaintDisplayedImpl(expectedRevision, frameIndex, squares, squareCount,
                              mask, color, clearEffects, resultingRevision);
}

FrameStream::GuardedEditResult FrameStream::PaintDisplayedImpl(
    const std::optional<uint32_t> expectedRevision, const uint16_t frameIndex,
    const uint8_t* const squares, const size_t squareCount, const uint8_t mask,
    const Rgb color, const bool clearEffects, uint32_t* const resultingRevision) {
    if (squares == nullptr || squareCount == 0U || squareCount > 2U ||
        mask == 0U || mask > 15U || Generation() == 0U) {
        return GuardedEditResult::Conflict;
    }
    std::array<uint8_t, 2U> targetSquares{};
    for (size_t i = 0U; i < squareCount; ++i) {
        targetSquares[i] = squares[i];
        if (targetSquares[i] >= 64U) return GuardedEditResult::Conflict;
    }
    bool changed = false;
    LockPublication();
    if (m_replacementReserved ||
        (expectedRevision.has_value() &&
         m_liveRevision.load(std::memory_order_relaxed) != *expectedRevision)) {
        UnlockPublication();
        return GuardedEditResult::Conflict;
    }
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    if (slot < m_frames.size() &&
        m_indices[slot].load(std::memory_order_acquire) == frameIndex &&
        m_generations[slot].load(std::memory_order_acquire) == Generation()) {
        auto& frame = m_frames[slot];
        for (size_t i = 0U; i < squareCount; ++i) {
            if (i != 0U && targetSquares[i] == targetSquares[0U]) continue;
            bool squareChanged = false;
            for (uint8_t emitter = 0U; emitter < 4U; ++emitter) {
                if ((mask & (1U << emitter)) == 0U) continue;
                auto& target = frame.emitters[
                    static_cast<size_t>(targetSquares[i]) * 4U + emitter];
                if (target.color.r != color.r || target.color.g != color.g ||
                    target.color.b != color.b) {
                    target.color = color;
                    squareChanged = true;
                }
                if (clearEffects &&
                    (target.effect != 0U || target.effectDurationMs != 0U)) {
                    target.effect = 0U;
                    target.effectDurationMs = 0U;
                    squareChanged = true;
                }
            }
            if (squareChanged) {
                RecordEdit(slot);
                AdvanceLiveRevision(store::MutationScope::Frame, frameIndex);
                changed = true;
            }
        }
        if (changed) {
            m_lastEditAtMs.store(fxe::Millis(), std::memory_order_release);
        }
    } else {
        UnlockPublication();
        return GuardedEditResult::Conflict;
    }
    if (resultingRevision != nullptr) {
        *resultingRevision = m_liveRevision.load(std::memory_order_relaxed);
    }
    UnlockPublication();
    return changed ? GuardedEditResult::Changed
                   : GuardedEditResult::Unchanged;
}

bool FrameStream::ApplyDisplayedEffect(const uint16_t frameIndex,
                                       const uint8_t square,
                                       const uint8_t mask,
                                       const uint8_t effect,
                                       const uint16_t durationMs) {
    return ApplyDisplayedEffectImpl(
        std::nullopt, frameIndex, &square, 1U, mask, effect, durationMs) ==
        GuardedEditResult::Changed;
}

FrameStream::GuardedEditResult FrameStream::ApplyDisplayedEffectIfRevision(
    const uint32_t expectedRevision, const uint16_t frameIndex,
    const uint8_t square, const uint8_t mask, const uint8_t effect,
    const uint16_t durationMs) {
    return ApplyDisplayedEffectImpl(expectedRevision, frameIndex, &square, 1U,
                                    mask, effect, durationMs);
}

FrameStream::GuardedEditResult FrameStream::ApplyDisplayedEffectBatchIfRevision(
    const uint32_t expectedRevision, const uint16_t frameIndex,
    const uint8_t* const squares, const size_t squareCount, const uint8_t mask,
    const uint8_t effect, const uint16_t durationMs,
    uint32_t* const resultingRevision) {
    return ApplyDisplayedEffectImpl(expectedRevision, frameIndex, squares,
                                    squareCount, mask, effect, durationMs,
                                    resultingRevision);
}

FrameStream::GuardedEditResult FrameStream::ApplyDisplayedEffectImpl(
    const std::optional<uint32_t> expectedRevision, const uint16_t frameIndex,
    const uint8_t* const squares, const size_t squareCount, const uint8_t mask,
    const uint8_t effect, const uint16_t durationMs,
    uint32_t* const resultingRevision) {
    if (squares == nullptr || squareCount == 0U || squareCount > 2U ||
        mask == 0U || mask > 15U || Generation() == 0U) {
        return GuardedEditResult::Conflict;
    }
    std::array<uint8_t, 2U> targetSquares{};
    for (size_t i = 0U; i < squareCount; ++i) {
        targetSquares[i] = squares[i];
        if (targetSquares[i] >= 64U) return GuardedEditResult::Conflict;
    }
    bool changed = false;
    LockPublication();
    if (m_replacementReserved ||
        (expectedRevision.has_value() &&
         m_liveRevision.load(std::memory_order_relaxed) != *expectedRevision)) {
        UnlockPublication();
        return GuardedEditResult::Conflict;
    }
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    if (slot < m_frames.size() &&
        m_indices[slot].load(std::memory_order_acquire) == frameIndex &&
        m_generations[slot].load(std::memory_order_acquire) == Generation()) {
        auto& frame = m_frames[slot];
        for (size_t i = 0U; i < squareCount; ++i) {
            if (i != 0U && targetSquares[i] == targetSquares[0U]) continue;
            bool squareChanged = false;
            for (uint8_t emitter = 0U; emitter < 4U; ++emitter) {
                if ((mask & (1U << emitter)) == 0U) continue;
                auto& target = frame.emitters[
                    static_cast<size_t>(targetSquares[i]) * 4U + emitter];
                if (target.effect != effect ||
                    target.effectDurationMs != durationMs) {
                    target.effect = effect;
                    target.effectDurationMs = durationMs;
                    squareChanged = true;
                }
            }
            if (squareChanged) {
                RecordEdit(slot);
                AdvanceLiveRevision(store::MutationScope::Frame, frameIndex);
                changed = true;
            }
        }
        if (changed) {
            m_lastEditAtMs.store(fxe::Millis(), std::memory_order_release);
        }
    } else {
        UnlockPublication();
        return GuardedEditResult::Conflict;
    }
    if (resultingRevision != nullptr) {
        *resultingRevision = m_liveRevision.load(std::memory_order_relaxed);
    }
    UnlockPublication();
    return changed ? GuardedEditResult::Changed
                   : GuardedEditResult::Unchanged;
}

bool FrameStream::PublishLoaded(const uint8_t slot, const uint16_t frameIndex,
                                const uint32_t generation,
                                const store::Frame& frame, const uint32_t identity) {
    if (slot >= m_frames.size()) return false;
    LockPublication();
    const uint16_t requested = m_requested.load(std::memory_order_relaxed);
    const uint16_t victim = m_indices[slot].load(std::memory_order_relaxed);
    const bool victimNeeded =
        m_generations[slot].load(std::memory_order_relaxed) == generation &&
        (victim == requested || victim == NextIndex(requested));
    // A seek may have repurposed this victim while the file read yielded.
    // Only fulfilling the new requested frame may replace its warm successor.
    if (HasPendingBlankDocument() || generation != Generation() || frameIndex >= FrameCount() ||
        m_liveIdentities[frameIndex] != identity ||
        StructuralPayloadPinned(m_cacheIdentities[slot]) ||
        slot == m_activeSlot.load(std::memory_order_acquire) ||
        (victimNeeded && frameIndex != requested) ||
        m_slotEditSequence[slot].load(std::memory_order_relaxed) !=
            m_slotPersistedSequence[slot].load(std::memory_order_relaxed)) {
        UnlockPublication();
        return false;
    }
    m_sequences[slot].fetch_add(1U, std::memory_order_acq_rel);
    m_indices[slot].store(NO_FRAME, std::memory_order_release);
    m_frames[slot] = frame;
    m_cacheIdentities[slot] = identity;
    m_slotEditSequence[slot].store(0U, std::memory_order_release);
    m_slotPersistedSequence[slot].store(0U, std::memory_order_release);
    m_generations[slot].store(generation, std::memory_order_release);
    m_indices[slot].store(frameIndex, std::memory_order_release);
    m_sequences[slot].fetch_add(1U, std::memory_order_release);
    UnlockPublication();
    return true;
}

void FrameStream::AdvanceLiveRevision(const store::MutationScope scope,
                                      const uint16_t index,
                                      const uint16_t aux) {
    const uint32_t prior = m_liveRevision.load(std::memory_order_relaxed);
    if (prior == UINT32_MAX) m_liveRevisionExhausted = true;
    const uint32_t next = NextLiveRevision(prior);
    m_scopeBaseRevision = next != 0U && scope != store::MutationScope::Unavailable
        ? prior : 0U;
    m_scope = m_scopeBaseRevision != 0U ? scope : store::MutationScope::Unavailable;
    m_scopeIndex = m_scopeBaseRevision != 0U ? index : 0U;
    m_scopeAux = m_scopeBaseRevision != 0U ? aux : 0U;
    m_liveRevision.store(next, std::memory_order_release);
}

void FrameStream::RecordEdit(const uint8_t slot) {
    const uint32_t sequence =
        m_editSequence.fetch_add(1U, std::memory_order_release) + 1U;
    m_slotEditSequence[slot].store(sequence, std::memory_order_release);
}

bool FrameStream::SlotDirty(const uint8_t slot) const {
    return slot < m_frames.size() &&
        m_slotEditSequence[slot].load(std::memory_order_acquire) !=
            m_slotPersistedSequence[slot].load(std::memory_order_acquire);
}

bool FrameStream::EditSaveReady(const uint32_t nowMs) const {
    if (!HasUnpersistedEdit() ||
        m_saveImmediately.load(std::memory_order_acquire)) {
        return true;
    }
    if (m_requested.load(std::memory_order_acquire) != DisplayedIndex()) {
        return true;
    }
    return static_cast<uint32_t>(
        nowMs - m_lastEditAtMs.load(std::memory_order_acquire)) >=
        EDIT_SAVE_QUIET_MS;
}

void FrameStream::LockPublication() const {
    while (m_publicationLock.test_and_set(std::memory_order_acquire)) {
#if defined(ARDUINO)
        // Animation can preempt the lower-priority main owner on the same
        // core. Block for a tick so that owner can finish its bounded copy;
        // taskYIELD alone would only admit equal-or-higher priority work.
        vTaskDelay(1U);
#endif
    }
}

void FrameStream::UnlockPublication() const {
    m_publicationLock.clear(std::memory_order_release);
}

uint16_t FrameStream::NextIndex(const uint16_t frameIndex) const {
    const uint16_t count = m_frameCount.load(std::memory_order_acquire);
    return count == 0U ? NO_FRAME : static_cast<uint16_t>((frameIndex + 1U) % count);
}

FrameStream::ServiceResult FrameStream::Service(
    store::FrameStore& store, const bool controlPending,
    const ServiceMode mode, const PrefetchPolicy prefetch) {
    if (controlPending) return ServiceResult::DeferredForControl;
    if (HasPendingBlankDocument()) {
        // Paging may interleave with another worker-owned file transaction.
        // New retains its wake for the ordinary mutation service boundary.
        return mode == ServiceMode::PagingOnly ? ServiceResult::Idle
                                               : ServiceBlankDocument(store);
    }
    if (mode == ServiceMode::PagingAndEdits && HasPendingStructuralEdit()) {
        return ServiceStructural(store);
    }
    if (mode == ServiceMode::PagingAndEdits && m_persistCount.load() != 0U) {
        return ServiceStructuralSave(store);
    }
    if (Generation() == 0U) return ServiceResult::Idle;
    const uint32_t generation = store.Generation();
    if (generation == 0U || generation !=
            m_authorityGeneration.load(std::memory_order_acquire)) {
        return mode == ServiceMode::PagingOnly ? ServiceResult::Loading : ServiceResult::IoFailure;
    }
    if (mode == ServiceMode::PagingOnly) return ServicePage(store, generation, prefetch);
    if (HasUnpersistedEdit()) return ServiceEdits(store, generation);
    // Paging decodes into Store-owned scratch before copying into a protected
    // cache slot; neither path reserves another full frame on the worker stack.
    return ServicePage(store, generation, prefetch);
}

FrameStream::ServiceResult FrameStream::ServicePage(
    store::FrameStore& store, const uint32_t generation,
    const PrefetchPolicy prefetch) {
    LockPublication();
    const uint16_t requested = m_requested.load(std::memory_order_acquire);
    const uint16_t count = m_frameCount.load(std::memory_order_acquire);
    if (requested >= count) {
        UnlockPublication();
        return ServiceResult::Idle;
    }

    const uint8_t active = m_activeSlot.load(std::memory_order_acquire);
    const auto cached = [&](const uint16_t index) {
        for (uint8_t slot = 0U; slot < m_frames.size(); ++slot) {
            if (m_generations[slot].load(std::memory_order_relaxed) == generation &&
                m_indices[slot].load(std::memory_order_relaxed) == index) {
                return slot;
            }
        }
        return NO_SLOT;
    };
    const uint8_t requestedSlot = cached(requested);
    const uint8_t successorSlot = cached(NextIndex(requested));
    uint16_t wanted = requested;
    const uint8_t candidates = std::min<uint16_t>(count,
        prefetch == PrefetchPolicy::None ? 1U :
        prefetch == PrefetchPolicy::TwoAhead ? 3U : 2U);
    for (uint8_t candidate = 0U; candidate < candidates; ++candidate) {
        if (cached(wanted) == NO_SLOT) break;
        if (candidate + 1U == candidates) {
            UnlockPublication();
            return ServiceResult::Idle;
        }
        wanted = NextIndex(wanted);
    }
    const auto loaded = wanted == requested ? ServiceResult::LoadedRequested
                                            : ServiceResult::PrefetchedNext;

    if (m_failedGeneration.load(std::memory_order_acquire) == generation &&
        m_failedIndex.load(std::memory_order_acquire) == wanted) {
        UnlockPublication();
        return ServiceResult::Idle;
    }

    uint8_t target = NO_SLOT;
    for (uint8_t slot = 0U; slot < m_frames.size(); ++slot) {
        if (slot == active || slot == requestedSlot ||
            slot == m_presentationSlot.load(std::memory_order_relaxed) ||
            (slot == successorSlot && wanted != requested) ||
            SlotDirty(slot) || StructuralPayloadPinned(m_cacheIdentities[slot])) continue;
        const bool vacant =
            m_generations[slot].load(std::memory_order_acquire) != generation ||
            m_indices[slot].load(std::memory_order_acquire) == NO_FRAME;
        if (target == NO_SLOT || vacant) target = slot;
        if (vacant) break;
    }
    if (target == NO_SLOT) {
        UnlockPublication();
        return ServiceResult::Idle;
    }

    const uint32_t identity = wanted < m_frameCount.load() ? m_liveIdentities[wanted] : 0U;
    const uint16_t durable = DurableIndexLocked(identity);
    UnlockPublication();
    if (durable == NO_FRAME) return ServiceResult::Loading;

    if (m_readCursor.active &&
        (m_loadingGeneration != generation || m_loadingIdentity != identity)) {
        store.CancelReadFrame(m_readCursor);
        m_loadingIndex = NO_FRAME;
    }
    if (!m_readCursor.active) {
        const auto begin = store.BeginReadFrame(durable, m_readCursor);
        if (begin != store::Status::Ok) return ServiceResult::IoFailure;
        m_loadingIndex = wanted;
        m_loadingGeneration = generation;
        m_loadingIdentity = identity;
    }

    const auto status = store.ContinueReadFrame(
        m_readCursor,
        store::WIRE_RECORD_HEADER_BYTES + store::WIRE_FRAME_MAX_BYTES);
    if (status == store::Status::Incomplete) return ServiceResult::Loading;
    m_loadingIndex = NO_FRAME;
    LockPublication();
    const bool sameIdentity = generation == Generation() && wanted < m_frameCount.load() &&
        m_liveIdentities[wanted] == identity;
    UnlockPublication();
    if (!sameIdentity) return ServiceResult::Loading;
    if (status == store::Status::Corrupt || status == store::Status::IoFailure) {
        m_failedGeneration.store(generation, std::memory_order_release);
        m_failedIndex.store(wanted, std::memory_order_release);
        m_failedStatus.store(static_cast<uint8_t>(status), std::memory_order_release);
        return status == store::Status::Corrupt ? ServiceResult::Corrupt
                                                : ServiceResult::IoFailure;
    }
    if (status != store::Status::Ok) return ServiceResult::IoFailure;
    if (!PublishLoaded(target, wanted, generation, store.DecodedFrame(), identity)) {
        return ServiceResult::Idle;
    }
    return loaded;
}

bool FrameStream::RequestedFrameCorrupt() const {
    if (HasPendingBlankDocument()) return false;
    return m_failedStatus.load(std::memory_order_acquire) ==
               static_cast<uint8_t>(store::Status::Corrupt) &&
        m_failedGeneration.load(std::memory_order_acquire) ==
            m_authorityGeneration.load(std::memory_order_acquire) &&
        m_failedIndex.load(std::memory_order_acquire) ==
            m_requested.load(std::memory_order_acquire);
}

bool FrameStream::RequestedFrameFailed() const {
    if (HasPendingBlankDocument()) return false;
    const auto status = static_cast<store::Status>(
        m_failedStatus.load(std::memory_order_acquire));
    return (status == store::Status::Corrupt ||
            status == store::Status::IoFailure) &&
        m_failedGeneration.load(std::memory_order_acquire) ==
            m_authorityGeneration.load(std::memory_order_acquire) &&
        m_failedIndex.load(std::memory_order_acquire) ==
            m_requested.load(std::memory_order_acquire);
}

} // namespace modules::litchcraft_frame_stream
