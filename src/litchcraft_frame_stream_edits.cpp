#include "modules/litchcraft_frame_stream.hpp"

#include "modules/litchcraft_frame_store.hpp"

namespace modules::litchcraft_frame_stream {
namespace store = litchcraft_frame_store;
namespace {

bool SameFrame(const store::Frame& first, const store::Frame& second) {
    if (first.durationMs != second.durationMs) return false;
    for (size_t index = 0U; index < first.emitters.size(); ++index) {
        const auto& a = first.emitters[index];
        const auto& b = second.emitters[index];
        if (a.color.r != b.color.r || a.color.g != b.color.g ||
            a.color.b != b.color.b || a.effect != b.effect ||
            a.effectDurationMs != b.effectDurationMs) return false;
    }
    return true;
}

} // namespace

FrameStream::ServiceResult FrameStream::ServiceEdits(
    store::FrameStore& store, const uint32_t generation) {
    const uint8_t active = m_activeSlot.load(std::memory_order_acquire);
    if (!m_saveImmediately.load(std::memory_order_acquire) &&
        m_requested.load(std::memory_order_acquire) != DisplayedIndex()) {
        const uint16_t requested =
            m_requested.load(std::memory_order_acquire);
        for (uint8_t slot = 0U; slot < m_frames.size(); ++slot) {
            if (m_generations[slot].load(std::memory_order_acquire) ==
                    generation &&
                m_indices[slot].load(std::memory_order_acquire) ==
                    requested) {
                return ServiceResult::WaitingForPresentation;
            }
        }
        for (uint8_t slot = 0U; slot < m_frames.size(); ++slot) {
            if (slot != active && !SlotDirty(slot)) {
                return ServicePage(store, generation);
            }
        }
    }
    LockPublication();
    const uint8_t current = m_activeSlot.load(std::memory_order_acquire);
    uint8_t editedSlot = NO_SLOT;
    if (current < m_frames.size()) {
        for (uint8_t slot = 0U; slot < m_frames.size(); ++slot) {
            if (slot != current && SlotDirty(slot)) { editedSlot = slot; break; }
        }
        if (editedSlot == NO_SLOT && SlotDirty(current)) editedSlot = current;
    }
    if (editedSlot >= m_frames.size() || m_replacementReserved ||
        HasPendingBlankDocument() || m_structuralEdit.running ||
        Generation() != generation || m_generations[editedSlot].load() != generation) {
        UnlockPublication();
        return ServiceResult::Loading;
    }
    const uint16_t editedIndex = m_indices[editedSlot].load();
    const uint32_t captured = m_slotEditSequence[editedSlot].load();
    const uint32_t documentEpoch = m_documentEpoch.load();
    const auto structuralResult = m_structuralEdit.result;
    const uint32_t structuralRevision = m_structuralEdit.revision;
    const auto retirement = m_retirementState.load();
    const uint32_t retiredGeneration = m_retiredGeneration.load();
    UnlockPublication();

    // The worker reads the authoritative logical slot, including a saved stage,
    // before BeginEdit can rebuild summaries or fold either resident. Keep all
    // flash I/O outside the publication lock and reuse the Store decode owner.
    store.CancelReadFrame(m_readCursor);
    m_loadingIndex = NO_FRAME;
    m_loadingGeneration = 0U;
    store::FrameStore::ReadCursor comparison{};
    auto status = store.BeginReadFrame(editedIndex, comparison);
    while (status == store::Status::Ok && comparison.active) {
        status = store.ContinueReadFrame(comparison, store::STAGE_SLOT_BYTES);
        if (status == store::Status::Incomplete) status = store::Status::Ok;
    }
    if (status != store::Status::Ok) return ServiceResult::IoFailure;

    // Called only under the publication lock. A read or fold may yield to new
    // paint, New, retirement or a structural request; none may settle a reused
    // slot or acknowledge a sequence newer than the one we captured.
    const auto stillOwned = [&]() {
        return Generation() == generation && store.Generation() == generation &&
            m_documentEpoch.load() == documentEpoch && !m_replacementReserved &&
            !HasPendingBlankDocument() && !m_structuralEdit.running &&
            m_structuralEdit.result == structuralResult &&
            m_structuralEdit.revision == structuralRevision &&
            m_retirementState.load() == retirement &&
            m_retiredGeneration.load() == retiredGeneration &&
            m_indices[editedSlot].load() == editedIndex &&
            m_generations[editedSlot].load() == generation &&
            m_slotEditSequence[editedSlot].load() == captured && SlotDirty(editedSlot);
    };
    LockPublication();
    if (!stillOwned()) { UnlockPublication(); return ServiceResult::Loading; }
    if (SameFrame(m_frames[editedSlot], store.DecodedFrame())) {
        m_slotPersistedSequence[editedSlot].store(captured, std::memory_order_release);
        if (!HasUnpersistedEdit()) m_saveImmediately.store(false, std::memory_order_release);
        UnlockPublication();
        return HasUnpersistedEdit() || HasPendingStructuralEdit()
            ? ServiceResult::Loading : ServiceResult::Idle;
    }
    UnlockPublication();
    // Ordinary saves share this worker with playback. Keep its existing
    // pager moving between completed fold/root file operations; the Store
    // excludes checksum phases while their wire/decode scratch is busy.
    store.SetEditPagingCallback([](void* context, store::FrameStore& frames) {
        auto& stream = *static_cast<FrameStream*>(context);
        if (stream.RequestedIndex() != stream.DisplayedIndex()) {
            (void)stream.Service(frames, false, ServiceMode::PagingOnly);
        }
    }, this);
    struct EditPagingScope final {
        store::FrameStore& frames;
        ~EditPagingScope() { frames.SetEditPagingCallback(nullptr, nullptr); }
    } pagingScope{store};
    store::TransactionToken token{};
    status = store.BeginEdit(generation, 0x5354524dU, token);
    if (status != store::Status::Ok) return ServiceResult::IoFailure;
    LockPublication();
    if (!stillOwned()) {
        UnlockPublication();
        (void)store.Abort(token);
        return ServiceResult::Loading;
    }
    // ReplaceFrame copies into the Store's existing edit buffer before
    // publication unlocks; no full frame belongs on the worker stack.
    status = store.ReplaceFrame(token, editedIndex, m_frames[editedSlot]);
    UnlockPublication();
    if (status == store::Status::Ok) status = store.Commit(token);
    if (status != store::Status::Ok) {
        (void)store.Abort(token);
        return ServiceResult::IoFailure;
    }
    const uint32_t nextGeneration = store.Generation();
    LockPublication();
    if (m_documentEpoch.load(std::memory_order_relaxed) != documentEpoch) {
        if (HasPendingBlankDocument() && Generation() == generation) {
            for (auto& cachedGeneration : m_generations) {
                if (cachedGeneration.load(std::memory_order_relaxed) == generation) {
                    cachedGeneration.store(nextGeneration, std::memory_order_release);
                }
            }
            m_authorityGeneration.store(nextGeneration, std::memory_order_release);
        }
        UnlockPublication();
        return HasPendingBlankDocument() ? ServiceResult::Loading : ServiceResult::Idle;
    }
    if (m_retirementState.load(std::memory_order_relaxed) ==
            RetirementState::Bound &&
        m_retiredGeneration.load(std::memory_order_relaxed) == generation) {
        // A retirement that won during flash commit must also cover the
        // saved generation produced by that commit.
        m_retiredGeneration.store(nextGeneration,
                                  std::memory_order_release);
    }
    for (uint8_t slot = 0U; slot < m_frames.size(); ++slot) {
        if (m_generations[slot].load(std::memory_order_acquire) == generation) {
            m_generations[slot].store(nextGeneration, std::memory_order_release);
        }
    }
    m_authorityGeneration.store(nextGeneration, std::memory_order_release);
    m_slotPersistedSequence[editedSlot].store(
        captured, std::memory_order_release);
    if (!HasUnpersistedEdit()) {
        m_saveImmediately.store(false, std::memory_order_release);
    }
    UnlockPublication();
    return HasUnpersistedEdit() || HasPendingStructuralEdit()
        ? ServiceResult::Loading : ServiceResult::Idle;
}

} // namespace modules::litchcraft_frame_stream
