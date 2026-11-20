#include "modules/litchcraft_frame_stream.hpp"

#include <algorithm>

#include "foxie_embedded/millis.hpp"

namespace modules::litchcraft_frame_stream {
namespace store = litchcraft_frame_store;

FrameStream::StructuralResult FrameStream::QueueStructuralEdit(const FrameEdit& edit) {
    LockPublication();
    const uint8_t slot = m_activeSlot.load();
    const uint16_t count = m_frameCount.load();
    const bool ready = !m_replacementReserved &&
        m_structuralEdit.result == StructuralResult::None &&
        edit.revision != 0U && AcceptsGeneration(edit.generation) &&
        m_liveRevision.load() == edit.revision && edit.source < count &&
        m_requested.load() == edit.source && slot < CACHE_SLOTS &&
        m_indices[slot].load() == edit.source && m_generations[slot].load() == Generation() &&
        (m_historyFrame == NO_FRAME || m_historyFrame == edit.source) &&
        ((edit.action == FrameEditAction::AddCopy && count < 256U) ||
         (edit.action == FrameEditAction::Delete && count > 1U));
    if (!ready) { UnlockPublication(); return StructuralResult::Conflict; }
    if (m_historyFrame != NO_FRAME) {
        m_history.End(m_historyFrame, m_frames[slot]);
        m_historyFrame = NO_FRAME;
    }
    m_structuralEdit = {};
    m_structuralEdit.revision = edit.revision;
    m_structuralEdit.documentEpoch = m_documentEpoch.load();
    m_structuralEdit.source = edit.source;
    m_structuralEdit.requested = edit.source;
    m_structuralEdit.action = static_cast<uint8_t>(edit.action);
    m_structuralEdit.result = StructuralResult::Pending;
    (void)ApplyStructuralLocked();
    m_saveImmediately.store(true, std::memory_order_release);
    UnlockPublication();
    return StructuralResult::Pending;
}

FrameStream::StructuralResult FrameStream::QueueStructuralHistory(
    const uint32_t expectedRevision, const bool redo) {
    LockPublication();
    litchcraft_store_history::StoreHistory::StructuralStep step{};
    const bool ready = !m_replacementReserved &&
        m_structuralEdit.result == StructuralResult::None &&
        expectedRevision != 0U && m_liveRevision.load() == expectedRevision &&
        m_history.NextStructural(redo, step) && m_historyFrame == NO_FRAME;
    if (!ready) { UnlockPublication(); return StructuralResult::Conflict; }
    m_structuralEdit = {};
    m_structuralEdit.revision = expectedRevision;
    m_structuralEdit.documentEpoch = m_documentEpoch.load();
    m_structuralEdit.source = step.source;
    m_structuralEdit.requested = m_requested.load();
    m_structuralEdit.action = step.action;
    m_structuralEdit.history = true;
    m_structuralEdit.redo = redo;
    m_structuralEdit.result = StructuralResult::Pending;
    (void)ApplyStructuralLocked();
    m_saveImmediately.store(true, std::memory_order_release);
    UnlockPublication();
    return StructuralResult::Pending;
}

bool FrameStream::ApplyStructuralLocked() {
    auto& edit = m_structuralEdit;
    const uint16_t count = m_frameCount.load();
    const uint8_t pending = m_persistCount.load();
    if (pending == PERSIST_CAPACITY || m_nextFrameIdentity == UINT32_MAX) return false;
    const bool deleting = edit.action == 1U;
    const uint16_t insertion = edit.action == 0U ? edit.source + 1U : edit.source;
    if ((deleting && (count <= 1U || edit.source >= count)) ||
        (!deleting && (count == store::FRAME_COUNT || insertion > count))) {
        edit.result = StructuralResult::Conflict;
        return false;
    }
    const uint16_t selected = deleting
        ? (edit.source == 0U ? 0U : edit.source - 1U) : insertion;
    const uint16_t oldSelected = deleting && edit.source == 0U ? 1U : selected;
    const uint32_t sourceIdentity = edit.source < count ? m_liveIdentities[edit.source] : 0U;
    const uint8_t sourceSlot = CachedIdentity(sourceIdentity);
    uint8_t target = deleting ? CachedIdentity(m_liveIdentities[oldSelected])
        : FreeStructuralSlot(sourceSlot);
    if (target == NO_SLOT || (!edit.history && sourceSlot == NO_SLOT) ||
        (edit.action == 0U && sourceSlot == NO_SLOT)) return false;
    if (!deleting) {
        if (edit.action == 4U) {
            if (!m_history.ReadRemovedFrame(edit.redo, m_frames[target])) {
                edit.result = StructuralResult::Conflict;
                return false;
            }
        } else m_frames[target] = m_frames[sourceSlot];
    }
    if (!edit.history) m_history.Begin(edit.source, m_frames[sourceSlot]);
    auto& save = m_structuralSaves[(m_persistHead + pending) % PERSIST_CAPACITY];
    save = {};
    save.action = edit.action;
    save.documentEpoch = edit.documentEpoch;
    save.identity = deleting ? sourceIdentity : m_nextFrameIdentity++;
    save.sourceIdentity = edit.action == 0U ? sourceIdentity
        : edit.action == 4U && insertion < count ? m_liveIdentities[insertion] : 0U;
    // Every accepted copy owns this exact cached snapshot before its source
    // can change. Cache pressure keeps the existing ordered action pending.
    save.SetPayload(!deleting);
    m_persistCount.store(pending + 1U, std::memory_order_release);
    if (deleting) {
        for (uint16_t index = edit.source; index + 1U < count; ++index) {
            m_liveIdentities[index] = m_liveIdentities[index + 1U];
        }
        m_liveIdentities[count - 1U] = 0U;
        m_frameCount.store(count - 1U);
    } else {
        for (uint16_t index = count; index > insertion; --index) {
            m_liveIdentities[index] = m_liveIdentities[index - 1U];
        }
        m_liveIdentities[insertion] = save.identity;
        m_frameCount.store(count + 1U);
        m_cacheIdentities[target] = save.identity;
        m_generations[target].store(Generation());
        m_slotEditSequence[target].store(0U);
        m_slotPersistedSequence[target].store(0U);
        if (save.HasPayload()) RecordEdit(target);
    }
    if (edit.history) (void)m_history.CommitStructuralStep(edit.redo);
    else m_history.CommitStructural(deleting
        ? litchcraft_store_history::StoreHistory::Kind::Delete
        : litchcraft_store_history::StoreHistory::Kind::AddCopy,
        deleting ? edit.source : insertion);
    ReindexCachedFrames();
    m_activeSlot.store(target, std::memory_order_release);
    m_requested.store(selected, std::memory_order_release);
    m_presentationCount.fetch_add(1U);
    m_failedIndex.store(NO_FRAME);
    m_failedGeneration.store(0U);
    AdvanceLiveRevision(deleting ? store::MutationScope::Delete : store::MutationScope::Insert,
                        deleting ? edit.source : insertion);
    edit.selected = selected;
    edit.revision = m_liveRevision.load();
    edit.result = StructuralResult::Changed;
    m_lastEditAtMs.store(fxe::Millis());
    return true;
}

FrameStream::StructuralResult FrameStream::TakeStructuralResult(
    uint16_t& selected, uint32_t& revision) {
    LockPublication();
    const StructuralResult result = m_structuralEdit.result;
    selected = m_structuralEdit.selected;
    revision = m_structuralEdit.revision;
    if (result != StructuralResult::Pending) m_structuralEdit = {};
    UnlockPublication();
    return result;
}

void FrameStream::CancelStructuralEdit() {
    LockPublication();
    if (m_structuralEdit.running) m_structuralEdit.cancelled = true;
    else m_structuralEdit = {};
    // Accepted live edits persist independently of a departed transport owner.
    UnlockPublication();
}

bool FrameStream::HasPendingStructuralEdit() const {
    LockPublication();
    const bool pending = m_structuralEdit.result == StructuralResult::Pending;
    UnlockPublication();
    return pending;
}

FrameStream::ServiceResult FrameStream::ServiceStructural(store::FrameStore& frames) {
    LockPublication();
    auto& edit = m_structuralEdit;
    if (edit.result != StructuralResult::Pending) {
        UnlockPublication(); return ServiceResult::Idle;
    }
    const bool current = !edit.cancelled && !m_replacementReserved &&
        edit.documentEpoch == m_documentEpoch.load() &&
        edit.revision == m_liveRevision.load() && m_requested.load() == edit.requested;
    if (!current) {
        edit.result = StructuralResult::Conflict;
        UnlockPublication(); return ServiceResult::Idle;
    }
    if (ApplyStructuralLocked() || edit.result != StructuralResult::Pending) {
        UnlockPublication(); return ServiceResult::Loading;
    }
    const uint16_t wanted = edit.action == 1U
        ? (edit.source == 0U ? 1U : edit.source - 1U) : edit.source;
    const uint8_t free = FreeStructuralSlot();
    if (free == NO_SLOT || m_persistCount.load() == PERSIST_CAPACITY ||
        wanted >= m_frameCount.load() || CachedIdentity(m_liveIdentities[wanted]) != NO_SLOT) {
        const bool structural = m_persistCount.load() != 0U;
        UnlockPublication();
        return structural ? ServiceStructuralSave(frames) : ServiceEdits(frames, Generation());
    }
    const uint32_t identity = m_liveIdentities[wanted];
    const uint16_t durable = DurableIndexLocked(identity);
    if (durable == NO_FRAME) { UnlockPublication(); return ServiceResult::Loading; }
    const uint32_t epoch = edit.documentEpoch;
    const uint32_t revision = edit.revision;
    edit.running = true;
    UnlockPublication();
    store::FrameStore::ReadCursor cursor{};
    auto status = frames.BeginReadFrame(durable, cursor);
    while (status == store::Status::Ok && cursor.active) {
        status = frames.ContinueReadFrame(cursor, store::STAGE_SLOT_BYTES);
        if (status == store::Status::Incomplete) status = store::Status::Ok;
    }
    LockPublication();
    if (edit.documentEpoch != epoch || edit.revision != revision) {
        UnlockPublication(); return ServiceResult::Loading;
    }
    edit.running = false;
    if (edit.cancelled || m_documentEpoch.load() != epoch ||
        m_liveRevision.load() != revision || m_requested.load() != edit.requested) {
        edit.result = StructuralResult::Conflict;
    } else if (status != store::Status::Ok) edit.result = StructuralResult::IoFailure;
    else {
        m_frames[free] = frames.DecodedFrame();
        m_cacheIdentities[free] = identity;
        m_indices[free].store(wanted);
        m_generations[free].store(Generation());
        m_slotEditSequence[free].store(0U);
        m_slotPersistedSequence[free].store(0U);
        (void)ApplyStructuralLocked();
    }
    UnlockPublication();
    return ServiceResult::Loading;
}

} // namespace modules::litchcraft_frame_stream
