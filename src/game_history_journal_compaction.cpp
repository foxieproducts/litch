#include "modules/game_history_journal.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace modules::game_history {

JournalMutationResult GameHistoryJournal::FindFirstGame(
    uint32_t& outPrefixBytes,
    uint32_t& outNextSequence,
    uint32_t& outSealSequence,
    bool& outSealed) const {
    outPrefixBytes = 0U;
    outNextSequence = 0U;
    outSealSequence = 0U;
    outSealed = false;
    if (m_summary.validPrefixBytes == 0U) {
        return JournalMutationResult::Applied;
    }
    StoredRecord first;
    if (!ReadRecord(0U, m_summary.validPrefixBytes, first)) {
        return JournalMutationResult::IoFailure;
    }
    uint32_t offset = 0U;
    while (offset < m_summary.validPrefixBytes) {
        StoredRecord current;
        if (!ReadRecord(offset, m_summary.validPrefixBytes, current)) {
            return JournalMutationResult::IoFailure;
        }
        if (first.gameId != current.gameId) {
            break;
        }
        offset += current.length;
        if (current.type == RecordType::Seal) {
            outPrefixBytes = offset;
            outSealSequence = current.outboxSequence;
            outNextSequence = current.outboxSequence == UINT32_MAX
                                  ? 0U
                                  : current.outboxSequence + 1U;
            outSealed = true;
            break;
        }
    }
    return JournalMutationResult::Applied;
}

JournalMutationResult GameHistoryJournal::CompactPrefix(
    const uint32_t prefixBytes,
    const uint32_t nextOldestSequence,
    const PendingCompaction kind,
    const bool prepareState) {
    if (m_summary.hasInvalidSuffix) return JournalMutationResult::Corrupt;
    if (prefixBytes == 0U || prefixBytes > m_summary.validPrefixBytes ||
        m_ops.replace == nullptr || m_ops.append == nullptr ||
        m_ops.read == nullptr || m_ops.stat == nullptr ||
        m_ops.promote == nullptr) {
        return JournalMutationResult::Bounds;
    }
    if (prepareState) {
        State pending = m_state;
        pending.pending = kind;
        pending.pendingOldest = m_summary.oldestRetained;
        pending.pendingNext = nextOldestSequence;
        pending.pendingBytes = prefixBytes;
        if (kind == PendingCompaction::EvictLostGame) {
            if (pending.lostGameCount == UINT32_MAX) {
                return JournalMutationResult::Exhausted;
            }
            ++pending.lostGameCount;
        }
        if (!SaveState(pending)) {
            return JournalMutationResult::IoFailure;
        }
    }

    if (!m_ops.replace(m_ops.context, JournalArtifact::Compaction,
                       nullptr, 0U)) {
        return JournalMutationResult::IoFailure;
    }
    std::array<uint8_t, JOURNAL_COPY_BUFFER_BYTES> buffer{};
    uint32_t sourceOffset = prefixBytes;
    while (sourceOffset < m_summary.validPrefixBytes) {
        const size_t count = std::min<size_t>(
            buffer.size(), m_summary.validPrefixBytes - sourceOffset);
        if (!m_ops.read(m_ops.context, JournalArtifact::Journal,
                        sourceOffset, buffer.data(), count) ||
            !m_ops.append(m_ops.context, JournalArtifact::Compaction,
                          buffer.data(), count)) {
            return JournalMutationResult::IoFailure;
        }
        sourceOffset += static_cast<uint32_t>(count);
    }

    uint32_t compactedBytes = 0U;
    if (m_ops.stat(m_ops.context, JournalArtifact::Compaction,
                   compactedBytes) != JournalStorageStatus::Present ||
        compactedBytes != m_summary.validPrefixBytes - prefixBytes) {
        return JournalMutationResult::IoFailure;
    }
    std::array<uint8_t, JOURNAL_COPY_BUFFER_BYTES> original{};
    uint32_t checkedOffset = 0U;
    while (checkedOffset < compactedBytes) {
        const size_t count = std::min<size_t>(
            buffer.size(), compactedBytes - checkedOffset);
        if (!m_ops.read(m_ops.context, JournalArtifact::Compaction,
                        checkedOffset, buffer.data(), count) ||
            !m_ops.read(m_ops.context, JournalArtifact::Journal,
                        prefixBytes + checkedOffset, original.data(), count) ||
            std::memcmp(buffer.data(), original.data(), count) != 0) {
            return JournalMutationResult::IoFailure;
        }
        checkedOffset += static_cast<uint32_t>(count);
    }
    // Complete records include their CRC: an aggregate CRC cannot prove an
    // exact copy. Compare bounded chunks with the original before promotion.
    if (!m_ops.promote(m_ops.context, JournalArtifact::Compaction,
                       JournalArtifact::Journal)) {
        return JournalMutationResult::IoFailure;
    }
    AdvanceLayoutEpoch();

    Scan scan;
    if (ScanJournal(scan) != JournalOpenResult::Ready ||
        scan.invalidSuffix ||
        (scan.oldestSequence != nextOldestSequence &&
         !(scan.oldestSequence == 0U &&
           nextOldestSequence == m_state.nextSequence))) {
        return JournalMutationResult::Corrupt;
    }
    PublishSummary(scan);
    State completed = m_state;
    completed.pending = PendingCompaction::None;
    completed.pendingOldest = 0U;
    completed.pendingNext = 0U;
    completed.pendingBytes = 0U;
    if (completed.compactionCount == UINT32_MAX) {
        return JournalMutationResult::Exhausted;
    }
    ++completed.compactionCount;
    if (!SaveState(completed)) {
        return JournalMutationResult::IoFailure;
    }
    PublishSummary(scan);
    return JournalMutationResult::Applied;
}

JournalMutationResult GameHistoryJournal::ResumePendingCompaction() {
    if (m_state.pending == PendingCompaction::None) {
        return JournalMutationResult::Applied;
    }
    if (m_summary.oldestRetained == m_state.pendingNext ||
        (m_summary.oldestRetained == 0U &&
         m_state.pendingNext == m_state.nextSequence)) {
        State completed = m_state;
        completed.pending = PendingCompaction::None;
        completed.pendingOldest = 0U;
        completed.pendingNext = 0U;
        completed.pendingBytes = 0U;
        if (completed.compactionCount == UINT32_MAX) {
            return JournalMutationResult::Exhausted;
        }
        ++completed.compactionCount;
        return SaveState(completed) ? JournalMutationResult::Applied
                                    : JournalMutationResult::IoFailure;
    }
    if (m_summary.oldestRetained != m_state.pendingOldest) {
        return JournalMutationResult::Corrupt;
    }
    return CompactPrefix(m_state.pendingBytes, m_state.pendingNext,
                         m_state.pending, false);
}

JournalMutationResult GameHistoryJournal::EnsureCapacity(
    const uint16_t incomingBytes) {
    while (m_summary.journalBytes > JOURNAL_MAX_BYTES - incomingBytes) {
        uint32_t prefixBytes = 0U;
        uint32_t nextSequence = 0U;
        uint32_t sealSequence = 0U;
        bool sealed = false;
        const JournalMutationResult found = FindFirstGame(
            prefixBytes, nextSequence, sealSequence, sealed);
        if (found != JournalMutationResult::Applied) {
            return found;
        }
        if (!sealed) {
            return JournalMutationResult::Capacity;
        }
        const PendingCompaction kind = sealSequence <= m_state.durableAck
                                           ? PendingCompaction::ReclaimAcknowledged
                                           : PendingCompaction::EvictLostGame;
        const JournalMutationResult compacted =
            CompactPrefix(prefixBytes, nextSequence, kind, true);
        if (compacted != JournalMutationResult::Applied) {
            Close();
            return compacted;
        }
    }
    return JournalMutationResult::Applied;
}

} // namespace modules::game_history
