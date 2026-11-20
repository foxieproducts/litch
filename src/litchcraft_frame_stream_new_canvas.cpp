#include "modules/litchcraft_frame_stream.hpp"

namespace modules::litchcraft_frame_stream {
namespace store = litchcraft_frame_store;

FrameStream::GuardedEditResult FrameStream::NewBlankDocument(
    const uint32_t expectedGeneration, const uint32_t expectedRevision,
    const std::array<Rgb, 8U>& favorites, uint32_t* const resultingRevision) {
    LockPublication();
    const uint32_t generation = Generation();
    if (generation == 0U || expectedRevision == 0U ||
        expectedRevision == UINT32_MAX ||
        expectedRevision != m_liveRevision.load(std::memory_order_relaxed) ||
        !AcceptsGeneration(expectedGeneration) || m_replacementReserved ||
        m_structuralEdit.result == StructuralResult::Pending ||
        m_structuralEdit.running) {
        UnlockPublication();
        return GuardedEditResult::Conflict;
    }
    m_history.Clear();
    m_historyFrame = NO_FRAME;
    m_structuralEdit = {};
    const uint8_t active = m_activeSlot.load(std::memory_order_relaxed);
    const uint8_t slot = active < m_frames.size()
        ? static_cast<uint8_t>((active + 1U) % m_frames.size()) : 0U;
    for (uint8_t index = 0U; index < m_frames.size(); ++index) {
        m_indices[index].store(NO_FRAME, std::memory_order_release);
        m_generations[index].store(0U, std::memory_order_release);
        m_slotEditSequence[index].store(0U, std::memory_order_release);
        m_slotPersistedSequence[index].store(0U, std::memory_order_release);
    }
    m_sequences[slot].fetch_add(1U, std::memory_order_acq_rel);
    m_frames[slot].durationMs = 1000U;
    m_frames[slot].emitters.fill({});
    m_generations[slot].store(generation, std::memory_order_release);
    m_indices[slot].store(0U, std::memory_order_release);
    m_sequences[slot].fetch_add(1U, std::memory_order_release);
    m_frameCount.store(1U, std::memory_order_release);
    ResetFrameIdentities(1U);
    m_requested.store(0U, std::memory_order_release);
    m_documentBaseGeneration.store(generation, std::memory_order_release);
    m_playbackOnEntry.store(false, std::memory_order_release);
    m_sparkleSeed.store(1U, std::memory_order_release);
    m_blankFavorites = favorites;
    m_editSequence.store(0U, std::memory_order_release);
    RecordEdit(slot);
    m_blankInitialSequence = m_slotEditSequence[slot].load(std::memory_order_relaxed);
    uint32_t epoch = m_documentEpoch.load(std::memory_order_relaxed) + 1U;
    if (epoch == 0U) epoch = 1U;
    m_documentEpoch.store(epoch, std::memory_order_release);
    AdvanceLiveRevision();
    m_failedIndex.store(NO_FRAME, std::memory_order_release);
    m_failedGeneration.store(0U, std::memory_order_release);
    m_failedStatus.store(static_cast<uint8_t>(store::Status::Ok),
                         std::memory_order_release);
    m_saveImmediately.store(true, std::memory_order_release);
    m_blankDocumentPending.store(true, std::memory_order_release);
    m_activeSlot.store(slot, std::memory_order_release);
    m_presentationCount.fetch_add(1U, std::memory_order_release);
    if (resultingRevision != nullptr) {
        *resultingRevision = m_liveRevision.load(std::memory_order_relaxed);
    }
    UnlockPublication();
    return GuardedEditResult::Changed;
}

FrameStream::ServiceResult FrameStream::ServiceBlankDocument(
    store::FrameStore& frames) {
    LockPublication();
    if (!HasPendingBlankDocument() || Generation() == 0U) {
        UnlockPublication();
        return ServiceResult::Idle;
    }
    const uint32_t epoch = m_documentEpoch.load(std::memory_order_relaxed);
    const uint32_t previousGeneration = m_authorityGeneration.load(std::memory_order_relaxed);
    const uint32_t initialSequence = m_blankInitialSequence;
    store::Manifest manifest{};
    manifest.topology = m_topology.load(std::memory_order_relaxed);
    manifest.active = m_documentActive.load(std::memory_order_relaxed);
    manifest.favorites = m_blankFavorites;
    UnlockPublication();

    frames.CancelReadFrame(m_readCursor);
    m_loadingIndex = NO_FRAME;
    m_loadingGeneration = 0U;
    store::TransactionToken token{};
    auto status = frames.Begin(manifest, frames.Generation(), 0x4e455743U, token);
    if (status != store::Status::Ok) return ServiceResult::IoFailure;
    // Only the known blank baseline is persisted here. Later accepted paint
    // stays dirty and is copied by the ordinary edit saver under its lock.
    // No live frame is read across flash I/O or added to the worker stack.
    status = frames.WriteBlankFirstFrame(token);
    if (status == store::Status::Ok) status = frames.Commit(token);
    if (status != store::Status::Ok) {
        const auto reconciled = frames.Abort(token);
        if (reconciled != store::Status::Ok || frames.Generation() != token.generation) {
            return ServiceResult::IoFailure;
        }
    }
    const uint32_t generation = frames.Generation();
    LockPublication();
    if (m_retirementState.load(std::memory_order_relaxed) == RetirementState::Bound &&
        m_retiredGeneration.load(std::memory_order_relaxed) == previousGeneration) {
        m_retiredGeneration.store(generation, std::memory_order_release);
        UnlockPublication();
        return ServiceResult::Idle;
    }
    if (!HasPendingBlankDocument()) {
        UnlockPublication();
        return ServiceResult::Idle;
    }
    // A newer New may have arrived during I/O. Advance only the durable base;
    // it keeps its own content, epoch and pending baseline publication.
    for (auto& cachedGeneration : m_generations) {
        if (cachedGeneration.load(std::memory_order_relaxed) == previousGeneration) {
            cachedGeneration.store(generation, std::memory_order_release);
        }
    }
    m_authorityGeneration.store(generation, std::memory_order_release);
    if (m_documentEpoch.load(std::memory_order_relaxed) == epoch) {
        // Later RAM Add/Delete may have changed selection or removed the
        // original blank. Its saved sequence belongs only to that identity.
        const uint8_t slot = CachedIdentity(1U);
        if (slot < m_frames.size()) {
            m_slotPersistedSequence[slot].store(initialSequence, std::memory_order_release);
        }
        m_blankDocumentPending.store(false, std::memory_order_release);
        m_failedIndex.store(NO_FRAME, std::memory_order_release);
        m_failedGeneration.store(0U, std::memory_order_release);
        m_failedStatus.store(static_cast<uint8_t>(store::Status::Ok),
                             std::memory_order_release);
        if (!HasUnpersistedEdit()) m_saveImmediately.store(false, std::memory_order_release);
    }
    UnlockPublication();
    return HasUnpersistedEdit() || HasPendingStructuralEdit()
        ? ServiceResult::Loading : ServiceResult::Idle;
}

} // namespace modules::litchcraft_frame_stream
