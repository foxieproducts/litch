#include "modules/litchcraft_frame_stream.hpp"

namespace modules::litchcraft_frame_stream {
namespace store = litchcraft_frame_store;

FrameStream::ServiceResult FrameStream::ServiceStructuralSave(store::FrameStore& frames) {
    LockPublication();
    if (m_persistCount.load() == 0U) { UnlockPublication(); return ServiceResult::Idle; }
    // The domain result must be consumed before this accepted operation saves.
    if (m_structuralEdit.result == StructuralResult::Changed) {
        UnlockPublication(); return ServiceResult::Loading;
    }
    auto& head = m_structuralSaves[m_persistHead];
    StructuralSave captured = head;
    uint16_t position = NO_FRAME;
    if (!captured.IsUncertain()) {
        if (captured.documentEpoch != m_documentEpoch.load() || Generation() == 0U ||
            frames.Generation() != Generation()) {
            UnlockPublication(); return ServiceResult::Loading;
        }
        const uint32_t sought = captured.action == 1U ? captured.identity : captured.sourceIdentity;
        for (uint16_t index = 0U; index < m_durableCount; ++index) {
            if (m_durableIdentities[index] == sought) { position = index; break; }
        }
        if (captured.action == 4U && captured.sourceIdentity == 0U) position = m_durableCount;
        if (captured.action == 0U && position != NO_FRAME) ++position;
        store::FrameStore::ResidentDescriptor resident{};
        if (position == NO_FRAME || !frames.CurrentResident(resident)) {
            UnlockPublication(); return ServiceResult::IoFailure;
        }
        head.previousGeneration = resident.generation;
        head.previousCrc = resident.bodyCrc32;
        head.resident = resident.rootSlot;
        captured = head;
    }
    UnlockPublication();

    frames.SetEditPagingCallback([](void* context, store::FrameStore& store) {
        auto& stream = *static_cast<FrameStream*>(context);
        if (stream.RequestedIndex() != stream.DisplayedIndex()) {
            (void)stream.Service(store, false, ServiceMode::PagingOnly);
        }
    }, this);
    struct PagingScope final {
        store::FrameStore& store;
        ~PagingScope() { store.SetEditPagingCallback(nullptr, nullptr); }
    } paging{frames};
    store::Status status = store::Status::Ok;
    store::TransactionToken token{};
    if (captured.IsUncertain()) {
        status = frames.Recover();
    } else {
        status = frames.BeginEdit(captured.previousGeneration, 0x53545243U, token);
        if (status == store::Status::Ok) {
            LockPublication();
            if (m_persistCount.load() == 0U || head.identity != captured.identity ||
                m_documentEpoch.load() != captured.documentEpoch) {
                status = store::Status::RevisionConflict;
            } else {
                head.committedGeneration = token.generation;
                head.slot = CachedIdentity(head.identity);
                head.sequence = head.slot < CACHE_SLOTS
                    ? m_slotEditSequence[head.slot].load() : 0U;
                captured = head;
                if (captured.action == 1U) status = frames.DeleteFrame(token, position);
                else {
                    status = captured.HasPayload() && captured.slot < CACHE_SLOTS
                        ? frames.InsertFrame(token, position, m_frames[captured.slot])
                        : store::Status::RevisionConflict;
                }
            }
            UnlockPublication();
        }
        if (status == store::Status::Ok) status = frames.PrepareCommit(token);
        if (status == store::Status::Ok) status = frames.PublishPreparedCommit(token);
        if (status != store::Status::Ok && token.nonce != 0U) {
            const auto reconciled = frames.Abort(token);
            if (reconciled != store::Status::Ok) status = reconciled;
        }
    }
    if (status != store::Status::IoFailure && status != store::Status::Corrupt &&
        frames.Generation() != captured.committedGeneration &&
        frames.Generation() != captured.previousGeneration) {
        status = frames.SelectResident(captured.resident, captured.previousGeneration,
                                        captured.previousCrc);
    }
    LockPublication();
    const bool sameOwner = m_persistCount.load() != 0U &&
        head.identity == captured.identity && head.action == captured.action &&
        captured.documentEpoch == m_documentEpoch.load();
    if (!sameOwner) {
        // New/retirement may win while a synchronous file operation completes.
        // Its accepted live document stays intact; only the saved fence moves.
        const uint32_t generation = frames.Generation();
        if (generation != 0U && m_authorityGeneration.load() == captured.previousGeneration) {
            if (m_retiredGeneration.load() == captured.previousGeneration) {
                m_retiredGeneration.store(generation);
            }
            for (auto& cached : m_generations) {
                if (cached.load() == captured.previousGeneration) cached.store(generation);
            }
            m_authorityGeneration.store(generation);
        }
        UnlockPublication(); return ServiceResult::Loading;
    }
    if (status == store::Status::IoFailure || status == store::Status::Corrupt) {
        head.SetUncertain(true);
        UnlockPublication(); return ServiceResult::IoFailure;
    }
    const bool committed = captured.committedGeneration != 0U &&
        frames.Generation() == captured.committedGeneration;
    if (!committed) {
        head.SetUncertain(false);
        UnlockPublication();
        return status == store::Status::Ok ? ServiceResult::Loading : ServiceResult::IoFailure;
    }
    // Recovery can complete a prior attempt, so resolve the saved ID again.
    const uint32_t sought = captured.action == 1U ? captured.identity : captured.sourceIdentity;
    position = NO_FRAME;
    for (uint16_t index = 0U; index < m_durableCount; ++index) {
        if (m_durableIdentities[index] == sought) { position = index; break; }
    }
    if (captured.action == 4U && captured.sourceIdentity == 0U) position = m_durableCount;
    if (captured.action == 0U && position != NO_FRAME) ++position;
    if (position == NO_FRAME) { UnlockPublication(); return ServiceResult::IoFailure; }
    if (captured.action == 1U) {
        for (uint16_t index = position; index + 1U < m_durableCount; ++index) {
            m_durableIdentities[index] = m_durableIdentities[index + 1U];
        }
        m_durableIdentities[--m_durableCount] = 0U;
    } else {
        for (uint16_t index = m_durableCount; index > position; --index) {
            m_durableIdentities[index] = m_durableIdentities[index - 1U];
        }
        m_durableIdentities[position] = captured.identity;
        ++m_durableCount;
    }
    const uint32_t generation = frames.Generation();
    for (auto& cached : m_generations) {
        if (cached.load() == captured.previousGeneration) cached.store(generation);
    }
    if (captured.HasPayload() && captured.slot < CACHE_SLOTS &&
        m_cacheIdentities[captured.slot] == captured.identity) {
        m_slotPersistedSequence[captured.slot].store(captured.sequence);
    }
    m_authorityGeneration.store(generation, std::memory_order_release);
    m_persistHead = (m_persistHead + 1U) % PERSIST_CAPACITY;
    m_persistCount.fetch_sub(1U, std::memory_order_release);
    ReindexCachedFrames();
    if (!HasUnpersistedEdit()) m_saveImmediately.store(false);
    UnlockPublication();
    return HasUnpersistedEdit() || HasPendingStructuralEdit() ? ServiceResult::Loading
                                                           : ServiceResult::Idle;
}

} // namespace modules::litchcraft_frame_stream
