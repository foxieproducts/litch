#include "modules/litchcraft_frame_stream.hpp"

namespace modules::litchcraft_frame_stream {
namespace store = litchcraft_frame_store;

bool FrameStream::MatchesLiveSnapshotLocked(const store::LiveSnapshot& snapshot) const {
    return !HasPendingBlankDocument() && !m_replacementReserved &&
        m_structuralEdit.result != StructuralResult::Pending &&
        snapshot.generation != 0U && snapshot.revision != 0U &&
        Generation() == snapshot.generation &&
        m_liveRevision.load(std::memory_order_relaxed) == snapshot.revision &&
        m_documentEpoch.load(std::memory_order_relaxed) == snapshot.documentEpoch &&
        m_frameCount.load(std::memory_order_relaxed) == snapshot.frameCount;
}

bool FrameStream::CaptureLiveSnapshot(const uint16_t selector,
                                      store::LiveSnapshot& snapshot) const {
    LockPublication();
    const uint8_t active = m_activeSlot.load(std::memory_order_relaxed);
    const uint16_t selected = active < m_frames.size()
        ? m_indices[active].load(std::memory_order_relaxed) : NO_FRAME;
    const uint16_t index = selector == NO_FRAME || selector == 0xfffeU
        ? selected : selector;
    const bool valid = snapshot.generation == Generation() &&
        snapshot.revision == m_liveRevision.load(std::memory_order_relaxed) &&
        snapshot.generation != 0U && snapshot.revision != 0U &&
        !HasPendingBlankDocument() && !m_replacementReserved && m_structuralEdit.result != StructuralResult::Pending &&
        index < m_frameCount.load(std::memory_order_relaxed);
    if (valid) {
        snapshot.documentEpoch = m_documentEpoch.load(std::memory_order_relaxed);
        snapshot.frameCount = m_frameCount.load(std::memory_order_relaxed);
        snapshot.frameIndex = index;
        snapshot.baseRevision = m_scopeBaseRevision;
        snapshot.scope = m_scope;
        snapshot.scopeIndex = m_scopeIndex;
        snapshot.scopeAux = m_scopeAux;
    }
    UnlockPublication();
    return valid;
}

bool FrameStream::MatchesLiveSnapshot(const store::LiveSnapshot& snapshot) const {
    LockPublication();
    const bool valid = MatchesLiveSnapshotLocked(snapshot);
    UnlockPublication();
    return valid;
}

FrameStream::LiveFrameResult FrameStream::CopyLiveFrame(
    const store::LiveSnapshot& snapshot, store::Frame& out) const {
    LockPublication();
    if (!MatchesLiveSnapshotLocked(snapshot) || snapshot.frameIndex >= snapshot.frameCount) {
        UnlockPublication();
        return LiveFrameResult::Conflict;
    }
    for (uint8_t slot = 0U; slot < m_frames.size(); ++slot) {
        if (m_indices[slot].load(std::memory_order_relaxed) == snapshot.frameIndex &&
            m_generations[slot].load(std::memory_order_relaxed) == snapshot.generation) {
            out = m_frames[slot];
            UnlockPublication();
            return LiveFrameResult::Cached;
        }
    }
    // Dirty slots cannot be evicted. Absence therefore permits the exact
    // durable slot, with the same authority checked after its worker read.
    UnlockPublication();
    return LiveFrameResult::Durable;
}

FrameStream::LiveFrameResult FrameStream::CopyDirtyFrame(
    const store::LiveSnapshot& snapshot, const uint8_t slot,
    store::Frame& out, uint16_t& index) const {
    LockPublication();
    if (!MatchesLiveSnapshotLocked(snapshot) || slot >= m_frames.size()) {
        UnlockPublication();
        return LiveFrameResult::Conflict;
    }
    if (!SlotDirty(slot)) {
        UnlockPublication();
        return LiveFrameResult::Durable;
    }
    index = m_indices[slot].load(std::memory_order_relaxed);
    if (index == NO_FRAME) {
        // A removed frame may still be pinned by an accepted pending insert;
        // its bytes are not part of this current live layout.
        UnlockPublication();
        return LiveFrameResult::Durable;
    }
    const bool valid = index < snapshot.frameCount &&
        m_generations[slot].load(std::memory_order_relaxed) == snapshot.generation;
    if (valid) out = m_frames[slot];
    UnlockPublication();
    return valid ? LiveFrameResult::Cached : LiveFrameResult::Conflict;
}

} // namespace modules::litchcraft_frame_stream
