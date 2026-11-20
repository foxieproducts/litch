#include "modules/game_history_journal.hpp"

#include <array>

namespace modules::game_history {

bool GameHistoryJournal::ReadRecord(const uint32_t offset,
                                    const uint32_t fileBytes,
                                    StoredRecord& out) const {
    if (m_ops.read == nullptr || offset > fileBytes ||
        fileBytes - offset < RECORD_HEADER_BYTES + RECORD_CRC_BYTES) {
        return false;
    }
    uint8_t header[RECORD_HEADER_BYTES] = {};
    if (!m_ops.read(m_ops.context, JournalArtifact::Journal, offset,
                    header, sizeof(header))) {
        return false;
    }
    const uint16_t length = static_cast<uint16_t>(
        RECORD_HEADER_BYTES + header[2U] + RECORD_CRC_BYTES);
    if (length > MAX_RECORD_BYTES || length > fileBytes - offset) {
        return false;
    }
    std::array<uint8_t, MAX_RECORD_BYTES> bytes{};
    Record decoded;
    if (!m_ops.read(m_ops.context, JournalArtifact::Journal, offset,
                    bytes.data(), length) ||
        !DecodeRecord(bytes.data(), length, decoded)) {
        return false;
    }
    out.gameId = decoded.gameId;
    out.type = decoded.type;
    out.outboxSequence = decoded.outboxSequence;
    out.offset = offset;
    out.length = length;
    return true;
}

JournalOpenResult GameHistoryJournal::ScanJournal(Scan& out) const {
    out = {};
    if (m_ops.stat == nullptr || m_ops.read == nullptr) {
        return JournalOpenResult::IoFailure;
    }
    const JournalStorageStatus status =
        m_ops.stat(m_ops.context, JournalArtifact::Journal, out.fileBytes);
    if (status == JournalStorageStatus::IoFailure) {
        return JournalOpenResult::IoFailure;
    }
    if (status == JournalStorageStatus::Missing) {
        out.fileBytes = 0U;
        return JournalOpenResult::Ready;
    }
    if (out.fileBytes > JOURNAL_MAX_BYTES) {
        return JournalOpenResult::Corrupt;
    }
    uint32_t offset = 0U;
    uint32_t priorSequence = 0U;
    while (offset < out.fileBytes) {
        StoredRecord stored;
        if (!ReadRecord(offset, out.fileBytes, stored) ||
            (priorSequence != 0U &&
             (priorSequence == UINT32_MAX ||
              stored.outboxSequence != priorSequence + 1U))) {
            out.invalidSuffix = true;
            break;
        }
        if (out.oldestSequence == 0U) {
            out.oldestSequence = stored.outboxSequence;
        }
        priorSequence = stored.outboxSequence;
        out.lastSequence = priorSequence;
        out.latestOffset = offset;
        out.latestLength = stored.length;
        offset += stored.length;
    }
    out.validBytes = offset;
    return JournalOpenResult::Ready;
}

void GameHistoryJournal::PublishSummary(const Scan& scan) {
    m_latestOffset = scan.latestOffset;
    m_latestLength = scan.latestLength;
    m_summary.journalId = m_state.journalId;
    m_summary.oldestRetained = scan.oldestSequence;
    m_summary.newestAllocated = m_state.nextSequence == 0U
                                    ? UINT32_MAX
                                    : m_state.nextSequence - 1U;
    m_summary.durableAck = m_state.durableAck;
    m_summary.lostGameCount = m_state.lostGameCount;
    m_summary.journalBytes = scan.fileBytes;
    m_summary.validPrefixBytes = scan.validBytes;
    m_summary.compactionCount = m_state.compactionCount;
    m_summary.hasInvalidSuffix = scan.invalidSuffix;
    m_summary.sequenceExhausted = m_state.nextSequence == 0U;
}

void GameHistoryJournal::AdvanceLayoutEpoch() {
    m_latestOffset = 0U;
    m_latestLength = 0U;
    ++m_layoutEpoch;
    if (m_layoutEpoch == 0U) m_layoutEpoch = 1U;
}

JournalOpenResult GameHistoryJournal::Open(const uint64_t newJournalId) {
    AdvanceLayoutEpoch();
    Close();
    bool missing = false;
    const JournalOpenResult loaded = LoadState(missing);
    if (loaded != JournalOpenResult::Ready) {
        return loaded;
    }
    if (missing) {
        if (newJournalId == 0U || m_ops.stat == nullptr) {
            return JournalOpenResult::Corrupt;
        }
        uint32_t journalBytes = 0U;
        const JournalStorageStatus journal =
            m_ops.stat(m_ops.context, JournalArtifact::Journal,
                       journalBytes);
        if (journal == JournalStorageStatus::IoFailure) {
            return JournalOpenResult::IoFailure;
        }
        if (journal == JournalStorageStatus::Present && journalBytes != 0U) {
            return JournalOpenResult::Corrupt;
        }
        m_state = {};
        m_state.nextSequence = 1U;
        m_state.journalId = newJournalId;
        m_stateArtifact = JournalArtifact::StateA;
        if (!SaveState(m_state)) {
            return JournalOpenResult::IoFailure;
        }
        m_open = true;
        Scan empty;
        PublishSummary(empty);
        return JournalOpenResult::Initialized;
    }

    m_open = true;
    Scan scan;
    JournalOpenResult scanned = ScanJournal(scan);
    if (scanned != JournalOpenResult::Ready) {
        Close();
        return scanned;
    }
    PublishSummary(scan);
    if (m_state.pending != PendingCompaction::None) {
        const JournalMutationResult resumed = ResumePendingCompaction();
        if (resumed != JournalMutationResult::Applied) {
            Close();
            return resumed == JournalMutationResult::Corrupt
                       ? JournalOpenResult::Corrupt
                       : JournalOpenResult::IoFailure;
        }
        scanned = ScanJournal(scan);
        if (scanned != JournalOpenResult::Ready) {
            Close();
            return scanned;
        }
    }

    if (scan.lastSequence != 0U &&
        (m_state.nextSequence != 0U &&
         scan.lastSequence >= m_state.nextSequence)) {
        State repaired = m_state;
        repaired.nextSequence = scan.lastSequence == UINT32_MAX
                                    ? 0U
                                    : scan.lastSequence + 1U;
        if (!SaveState(repaired)) {
            Close();
            return JournalOpenResult::IoFailure;
        }
    }
    PublishSummary(scan);
    // A durable allocation cannot be ahead of a nonempty retained tail. A
    // clean record-boundary truncation is still data loss, even with valid CRCs.
    if ((scan.lastSequence != 0U &&
         scan.lastSequence != m_summary.newestAllocated) ||
        m_state.durableAck > m_summary.newestAllocated) {
        Close();
        return JournalOpenResult::Corrupt;
    }
    return JournalOpenResult::Ready;
}

JournalMutationResult GameHistoryJournal::Append(const Record& record) {
    if (!m_open) return JournalMutationResult::IoFailure;
    if (m_summary.hasInvalidSuffix) return JournalMutationResult::Corrupt;
    if (m_state.nextSequence == 0U) return JournalMutationResult::Exhausted;
    if (record.outboxSequence != m_state.nextSequence) {
        return JournalMutationResult::Bounds;
    }
    std::array<uint8_t, MAX_RECORD_BYTES> bytes{};
    size_t length = 0U;
    if (!EncodeRecord(record, bytes.data(), bytes.size(), length)) {
        return JournalMutationResult::Bounds;
    }
    const JournalMutationResult capacity =
        EnsureCapacity(static_cast<uint16_t>(length));
    if (capacity != JournalMutationResult::Applied) {
        return capacity;
    }
    if (m_ops.append == nullptr ||
        !m_ops.append(m_ops.context, JournalArtifact::Journal,
                      bytes.data(), length)) {
        // A failed flush may still have written some or all of the record.
        // Reopen must reconcile those bytes before allocating another sequence.
        Close();
        return JournalMutationResult::IoFailure;
    }
    uint32_t fileBytes = 0U;
    if (m_ops.stat == nullptr ||
        m_ops.stat(m_ops.context, JournalArtifact::Journal, fileBytes) !=
            JournalStorageStatus::Present ||
        fileBytes != m_summary.journalBytes + length) {
        Close();
        return JournalMutationResult::IoFailure;
    }
    std::array<uint8_t, MAX_RECORD_BYTES> checked{};
    if (m_ops.read == nullptr ||
        !m_ops.read(m_ops.context, JournalArtifact::Journal,
                    m_summary.journalBytes, checked.data(), length) ||
        std::memcmp(bytes.data(), checked.data(), length) != 0) {
        Close();
        return JournalMutationResult::IoFailure;
    }
    State advanced = m_state;
    advanced.nextSequence = record.outboxSequence == UINT32_MAX
                                ? 0U
                                : record.outboxSequence + 1U;
    if (!SaveState(advanced)) {
        Close();
        return JournalMutationResult::IoFailure;
    }
    // The retained prefix was validated on open/compaction. This single-owner
    // append has verified its exact new length, tail bytes and durable state;
    // rereading the unchanged prefix adds work proportional to game length.
    Scan scan;
    scan.fileBytes = fileBytes;
    scan.validBytes = fileBytes;
    scan.oldestSequence = m_summary.oldestRetained == 0U
                              ? record.outboxSequence : m_summary.oldestRetained;
    scan.lastSequence = record.outboxSequence;
    scan.latestOffset = m_summary.journalBytes;
    scan.latestLength = static_cast<uint16_t>(length);
    PublishSummary(scan);
    return JournalMutationResult::Applied;
}

JournalMutationResult GameHistoryJournal::AdvanceDurableAck(
    const uint32_t sequence) {
    if (!m_open) return JournalMutationResult::IoFailure;
    if (sequence <= m_state.durableAck) {
        return JournalMutationResult::Applied;
    }
    if (sequence > m_summary.newestAllocated) {
        return JournalMutationResult::Bounds;
    }
    State advanced = m_state;
    advanced.durableAck = sequence;
    if (!SaveState(advanced)) {
        Close();
        return JournalMutationResult::IoFailure;
    }
    m_summary.durableAck = sequence;
    return JournalMutationResult::Applied;
}

JournalMutationResult GameHistoryJournal::ReclaimAcknowledged() {
    if (!m_open) return JournalMutationResult::IoFailure;
    if (m_summary.hasInvalidSuffix) return JournalMutationResult::Corrupt;
    while (m_summary.validPrefixBytes != 0U) {
        uint32_t prefixBytes = 0U;
        uint32_t nextSequence = 0U;
        uint32_t sealSequence = 0U;
        bool sealed = false;
        const JournalMutationResult found = FindFirstGame(
            prefixBytes, nextSequence, sealSequence, sealed);
        if (found != JournalMutationResult::Applied) return found;
        if (!sealed || sealSequence > m_state.durableAck) {
            return JournalMutationResult::Applied;
        }
        // Keep the latest game as the durable capture anchor. A repeated
        // settings snapshot must not republish a just-pruned completed game.
        // Capacity reclamation can still remove it when a real append needs space.
        if (prefixBytes == m_summary.validPrefixBytes) return JournalMutationResult::Applied;
        const JournalMutationResult compacted = CompactPrefix(
            prefixBytes, nextSequence,
            PendingCompaction::ReclaimAcknowledged, true);
        if (compacted != JournalMutationResult::Applied) {
            Close();
            return compacted;
        }
    }
    return JournalMutationResult::Applied;
}

} // namespace modules::game_history
