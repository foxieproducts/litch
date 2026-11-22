#include "modules/litchcraft_frame_stream.hpp"

namespace modules::litchcraft_frame_stream {
namespace store = litchcraft_frame_store;

void FrameStream::ResetFrameIdentities(const uint16_t count) {
    RetirePresentationLocked();
    m_confirmedDocumentEpoch = 0U;
    m_publishedPresentationContext = 0U;
    m_persistCount.store(0U, std::memory_order_release);
    m_persistHead = 0U;
    m_nextFrameIdentity = count + 1U;
    m_durableCount = count;
    for (uint16_t index = 0U; index < store::FRAME_COUNT; ++index) {
        m_liveIdentities[index] = index < count ? index + 1U : 0U;
        m_durableIdentities[index] = m_liveIdentities[index];
    }
    for (uint8_t slot = 0U; slot < CACHE_SLOTS; ++slot) {
        const uint16_t index = m_indices[slot].load();
        m_cacheIdentities[slot] = index < count ? m_liveIdentities[index] : 0U;
    }
}

uint16_t FrameStream::FindLiveIdentity(const uint32_t identity) const {
    if (identity != 0U) {
        for (uint16_t index = 0U; index < m_frameCount.load(); ++index) {
            if (m_liveIdentities[index] == identity) return index;
        }
    }
    return NO_FRAME;
}

uint16_t FrameStream::DurableIndexLocked(const uint32_t identity) const {
    if (identity != 0U) {
        for (uint16_t index = 0U; index < m_durableCount; ++index) {
            if (m_durableIdentities[index] == identity) return index;
        }
    }
    return NO_FRAME;
}

uint8_t FrameStream::CachedIdentity(const uint32_t identity) const {
    if (identity != 0U) {
        for (uint8_t slot = 0U; slot < CACHE_SLOTS; ++slot) {
            if (m_cacheIdentities[slot] == identity &&
                m_generations[slot].load() == Generation()) return slot;
        }
    }
    return NO_SLOT;
}

bool FrameStream::StructuralPayloadPinned(const uint32_t identity) const {
    if (identity == 0U) return false;
    for (uint8_t offset = 0U; offset < m_persistCount.load(); ++offset) {
        const auto& pending = m_structuralSaves[(m_persistHead + offset) % PERSIST_CAPACITY];
        if (pending.identity == identity && pending.HasPayload()) return true;
    }
    return false;
}

uint8_t FrameStream::FreeStructuralSlot(const uint8_t protectedSlot) const {
    const uint8_t active = m_activeSlot.load();
    for (uint8_t slot = 0U; slot < CACHE_SLOTS; ++slot) {
        if (slot != active && slot != protectedSlot &&
            slot != m_presentationSlot.load(std::memory_order_relaxed) && !SlotDirty(slot) &&
            !StructuralPayloadPinned(m_cacheIdentities[slot])) return slot;
    }
    return NO_SLOT;
}

void FrameStream::ReindexCachedFrames() {
    for (uint8_t slot = 0U; slot < CACHE_SLOTS; ++slot) {
        const uint16_t index = FindLiveIdentity(m_cacheIdentities[slot]);
        m_indices[slot].store(index, std::memory_order_release);
        if (index == NO_FRAME && !StructuralPayloadPinned(m_cacheIdentities[slot])) {
            m_generations[slot].store(0U, std::memory_order_release);
            m_slotEditSequence[slot].store(0U, std::memory_order_release);
            m_slotPersistedSequence[slot].store(0U, std::memory_order_release);
            m_cacheIdentities[slot] = 0U;
        }
    }
}

uint16_t FrameStream::LiveDurableIndex(const store::LiveSnapshot& snapshot,
                                      const uint16_t index) const {
    LockPublication();
    const uint16_t durable = MatchesLiveSnapshotLocked(snapshot) && index < snapshot.frameCount
        ? DurableIndexLocked(m_liveIdentities[index]) : NO_FRAME;
    UnlockPublication();
    return durable;
}

} // namespace modules::litchcraft_frame_stream
