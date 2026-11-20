#include "modules/litchcraft_frame_stream.hpp"

namespace modules::litchcraft_frame_stream {

bool FrameStream::PresentationReadyLocked() const {
    return !m_confirmedPresentation ||
        (m_confirmedDocumentEpoch != 0U &&
         m_confirmedDocumentEpoch == m_documentEpoch.load(std::memory_order_relaxed) &&
         m_confirmedPresentationCount == m_presentationCount.load(std::memory_order_relaxed));
}

bool FrameStream::PresentationReady() const {
    LockPublication();
    const bool ready = PresentationReadyLocked() && !m_replacementReserved;
    UnlockPublication();
    return ready;
}

uint32_t FrameStream::PresentationCount() const {
    LockPublication();
    const uint32_t count = m_confirmedPresentation ? m_confirmedPresentationCount
        : m_presentationCount.load(std::memory_order_relaxed);
    UnlockPublication();
    return count;
}

void FrameStream::RetirePresentationLocked() {
    m_presentationSlot.store(NO_SLOT, std::memory_order_release);
    m_presentationContext = 0U;
    m_presentationOriginalRequest = NO_FRAME;
}

void FrameStream::RetirePresentationContext() {
    LockPublication();
    RetirePresentationLocked();
    m_confirmedDocumentEpoch = 0U;
    m_publishedPresentationContext = 0U;
    UnlockPublication();
}

void FrameStream::SetConfirmedPresentation(const bool enabled) {
    LockPublication();
    RetirePresentationLocked();
    m_confirmedPresentation = enabled;
    m_confirmedDocumentEpoch = 0U;
    m_publishedPresentationContext = 0U;
    m_confirmedPresentationCount = 0U;
    UnlockPublication();
}

bool FrameStream::PreparePresentation(
    litchcraft_frame_store::Frame& out, const uint32_t contextEpoch,
    PresentationToken& token) {
    LockPublication();
    const uint16_t requested = token.frameIndex;
    const uint8_t active = m_activeSlot.load(std::memory_order_relaxed);
    const bool holdsConfirmed = active < CACHE_SLOTS && PresentationReadyLocked() &&
        requested == m_indices[active].load(std::memory_order_relaxed);
    if (!m_confirmedPresentation || contextEpoch == 0U ||
        contextEpoch != m_publishedPresentationContext ||
        (HasPresentationCandidate() && !MatchesPresentationLocked(token)) ||
        m_presentationMutationLease != 0U || Generation() == 0U ||
        (m_replacementReserved && !holdsConfirmed) ||
        m_structuralEdit.result == StructuralResult::Pending ||
        m_presentationLease == UINT32_MAX) {
        UnlockPublication();
        return false;
    }
    const uint32_t generation = Generation();
    for (uint8_t slot = 0U; slot < CACHE_SLOTS; ++slot) {
        const uint32_t sequence = m_sequences[slot].load(std::memory_order_relaxed);
        if ((sequence & 1U) != 0U ||
            m_generations[slot].load(std::memory_order_relaxed) != generation ||
            m_indices[slot].load(std::memory_order_relaxed) != requested) continue;
        if (!HasPresentationCandidate())
            m_presentationOriginalRequest = m_requested.load(std::memory_order_relaxed);
        token = {m_documentEpoch.load(std::memory_order_relaxed), contextEpoch,
            m_cacheIdentities[slot], sequence,
            m_slotEditSequence[slot].load(std::memory_order_relaxed),
            ++m_presentationLease, requested, slot};
        out = m_frames[slot];
        m_presentationContext = contextEpoch;
        m_presentationSlot.store(slot, std::memory_order_release);
        UnlockPublication();
        return true;
    }
    UnlockPublication();
    return false;
}

bool FrameStream::MatchesPresentationLocked(const PresentationToken& token) const {
    return token.lease != 0U && token.lease == m_presentationLease &&
        token.contextEpoch == m_presentationContext &&
        token.slot < CACHE_SLOTS && token.slot == m_presentationSlot.load(std::memory_order_relaxed);
}

bool FrameStream::PublishPresentationContext(const uint32_t presentationEpoch) {
    LockPublication();
    const bool valid = presentationEpoch != 0U &&
        (!HasPresentationCandidate() || m_presentationContext == presentationEpoch);
    if (valid) m_publishedPresentationContext = presentationEpoch;
    UnlockPublication();
    return valid;
}

bool FrameStream::IsPresentationContextCurrent(const uint32_t presentationEpoch) const {
    LockPublication();
    const bool valid = m_confirmedPresentation && presentationEpoch != 0U &&
        presentationEpoch == m_publishedPresentationContext && m_presentationMutationLease == 0U;
    UnlockPublication();
    return valid;
}

bool FrameStream::RequestPresentation(const PresentationToken& token,
    const uint16_t frameIndex) {
    LockPublication();
    const bool valid = MatchesPresentationLocked(token) &&
        token.contextEpoch == m_publishedPresentationContext &&
        token.documentEpoch == m_documentEpoch.load(std::memory_order_relaxed) &&
        Generation() != 0U && !m_replacementReserved && frameIndex < FrameCount();
    if (valid) m_requested.store(frameIndex, std::memory_order_release);
    UnlockPublication();
    return valid;
}

uint32_t FrameStream::TryBeginPresentationControl() {
    LockPublication();
    if (HasPresentationCandidate() || m_presentationMutationLease != 0U ||
        m_nextPresentationMutationLease == UINT32_MAX) {
        UnlockPublication();
        return 0U;
    }
    m_presentationMutationLease = ++m_nextPresentationMutationLease;
    const uint32_t lease = m_presentationMutationLease;
    UnlockPublication();
    return lease;
}

uint32_t FrameStream::TryBeginPresentationMutation() {
    LockPublication();
    if (!PresentationReadyLocked() || m_replacementReserved || HasPresentationCandidate() ||
        m_presentationMutationLease != 0U ||
        m_nextPresentationMutationLease == UINT32_MAX) {
        UnlockPublication();
        return 0U;
    }
    m_presentationMutationLease = ++m_nextPresentationMutationLease;
    const uint32_t lease = m_presentationMutationLease;
    UnlockPublication();
    return lease;
}

bool FrameStream::EndPresentationMutation(const uint32_t lease) {
    LockPublication();
    const bool current = lease != 0U && lease == m_presentationMutationLease;
    if (current) m_presentationMutationLease = 0U;
    UnlockPublication();
    return current;
}

bool FrameStream::CancelPresentation(const PresentationToken& token) {
    LockPublication();
    const bool current = MatchesPresentationLocked(token);
    if (current) {
        // Only the exact candidate owns its speculative successor demands.
        // Retry starts from the last confirmed cursor, with its original
        // pending seek/read intact rather than a half-consumed catch-up.
        m_requested.store(m_presentationOriginalRequest, std::memory_order_release);
        RetirePresentationLocked();
    }
    UnlockPublication();
    return current;
}

bool FrameStream::ConfirmPresented(const PresentationToken& token) {
    LockPublication();
    const bool valid = m_confirmedPresentation && MatchesPresentationLocked(token) &&
        token.contextEpoch == m_publishedPresentationContext &&
        Generation() != 0U &&
        (!m_replacementReserved ||
         token.slot == m_activeSlot.load(std::memory_order_relaxed)) &&
        token.documentEpoch == m_documentEpoch.load(std::memory_order_relaxed) &&
        token.frameIndex == m_indices[token.slot].load(std::memory_order_relaxed) &&
        token.frameIdentity != 0U && token.frameIdentity == m_cacheIdentities[token.slot] &&
        token.loadSequence == m_sequences[token.slot].load(std::memory_order_relaxed) &&
        token.editSequence == m_slotEditSequence[token.slot].load(std::memory_order_relaxed) &&
        m_generations[token.slot].load(std::memory_order_relaxed) == Generation();
    if (valid) {
        if (m_activeSlot.load(std::memory_order_relaxed) != token.slot) {
            m_activeSlot.store(token.slot, std::memory_order_release);
            m_presentationCount.fetch_add(1U, std::memory_order_release);
        }
        m_confirmedDocumentEpoch = token.documentEpoch;
        m_confirmedPresentationCount = m_presentationCount.load(std::memory_order_relaxed);
        RetirePresentationLocked();
    }
    UnlockPublication();
    return valid;
}

} // namespace modules::litchcraft_frame_stream
