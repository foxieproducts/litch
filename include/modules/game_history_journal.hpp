#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/game_history_protocol.hpp"

namespace modules::game_history {

inline constexpr uint32_t JOURNAL_MAX_BYTES = 512U * 1024U;
inline constexpr size_t JOURNAL_STATE_BYTES = 56U;
inline constexpr size_t JOURNAL_COPY_BUFFER_BYTES = 96U;

enum class JournalArtifact : uint8_t {
    Journal = 0U,
    Compaction,
    StateA,
    StateB,
};

enum class JournalStorageStatus : uint8_t {
    Missing = 0U,
    Present,
    IoFailure,
};

struct GameHistoryJournalStorageOps {
    void* context = nullptr;
    JournalStorageStatus (*stat)(void*, JournalArtifact, uint32_t&) = nullptr;
    bool (*read)(void*, JournalArtifact, uint32_t, uint8_t*, size_t) = nullptr;
    // Replaces one artifact durably. State replacement and promotion must leave
    // either the previous complete artifact or the new complete artifact.
    bool (*replace)(void*, JournalArtifact, const uint8_t*, size_t) = nullptr;
    // Appends and flushes before reporting success.
    bool (*append)(void*, JournalArtifact, const uint8_t*, size_t) = nullptr;
    // Atomically promotes a complete compaction artifact over the journal.
    bool (*promote)(void*, JournalArtifact, JournalArtifact) = nullptr;
};

enum class JournalOpenResult : uint8_t {
    Ready = 0U,
    Initialized,
    Corrupt,
    IoFailure,
};

enum class JournalMutationResult : uint8_t {
    Applied = 0U,
    Bounds,
    Capacity,
    Corrupt,
    IoFailure,
    Exhausted,
};

enum class JournalSegmentRead : uint8_t {
    Empty = 0U,
    Ready,
    GapReady,
    Unavailable,
    Bounds,
    Corrupt,
    IoFailure,
};

struct JournalSummary {
    uint64_t journalId = 0U;
    uint32_t oldestRetained = 0U;
    uint32_t newestAllocated = 0U;
    uint32_t durableAck = 0U;
    uint32_t lostGameCount = 0U;
    uint32_t journalBytes = 0U;
    uint32_t validPrefixBytes = 0U;
    uint32_t compactionCount = 0U;
    bool hasInvalidSuffix = false;
    bool sequenceExhausted = false;
};

struct JournalSegmentPlan {
    SegmentDescriptor descriptor{};
    uint32_t journalOffset = 0U;
    uint32_t layoutEpoch = 0U;
};

class GameHistoryJournal final {
  public:
    explicit GameHistoryJournal(GameHistoryJournalStorageOps ops)
        : m_ops(ops) {}

    JournalOpenResult Open(uint64_t newJournalId);
    // Invalidates cached scan/readiness without changing retained artifacts.
    void Close() {
        m_open = false;
        m_latestOffset = 0U;
        m_latestLength = 0U;
    }
    [[nodiscard]] bool IsOpen() const { return m_open; }
    [[nodiscard]] const JournalSummary& Summary() const { return m_summary; }

    JournalMutationResult Append(const Record& record);
    JournalMutationResult AdvanceDurableAck(uint32_t sequence);
    JournalMutationResult ReclaimAcknowledged();

    // Reads the newest complete record without exposing the storage seam.
    // Empty journals report Empty; invalid retained suffixes fail closed.
    JournalSegmentRead ReadLatestRecord(Record& outRecord) const;

    // Finds a position only in the latest retained line of the current game.
    // A newer base supersedes every older candidate, even at the same ply.
    JournalSegmentRead ReadCurrentLinePosition(
        const GameId& gameId, uint16_t ply, uint64_t beforeRevision,
        Record& outRecord) const;

    JournalSegmentRead PlanSegmentAfter(
        uint32_t durableAppAck,
        JournalSegmentPlan& outPlan) const;
    JournalSegmentRead ReadSegmentChunk(
        const JournalSegmentPlan& plan,
        uint16_t segmentOffset,
        uint8_t* out,
        size_t outCapacity,
        size_t& outLength) const;

  private:
    enum class PendingCompaction : uint8_t {
        None = 0U,
        ReclaimAcknowledged = 1U,
        EvictLostGame = 2U,
    };

    struct State {
        uint32_t generation = 0U;
        uint64_t journalId = 0U;
        uint32_t nextSequence = 1U;
        uint32_t durableAck = 0U;
        uint32_t lostGameCount = 0U;
        uint32_t compactionCount = 0U;
        uint32_t recoveryCount = 0U;
        PendingCompaction pending = PendingCompaction::None;
        uint32_t pendingOldest = 0U;
        uint32_t pendingNext = 0U;
        uint32_t pendingBytes = 0U;
    };

    struct Scan {
        uint32_t fileBytes = 0U;
        uint32_t validBytes = 0U;
        uint32_t oldestSequence = 0U;
        uint32_t lastSequence = 0U;
        uint32_t latestOffset = 0U;
        uint16_t latestLength = 0U;
        bool invalidSuffix = false;
    };

    struct StoredRecord {
        GameId gameId{};
        RecordType type = RecordType::LineBase;
        uint32_t outboxSequence = 0U;
        uint32_t offset = 0U;
        uint16_t length = 0U;
    };

    JournalOpenResult LoadState(bool& outMissing);
    bool SaveState(const State& replacement);
    JournalOpenResult ReadState(JournalArtifact artifact, State& out) const;
    bool SerializeState(const State& state,
                        uint8_t out[JOURNAL_STATE_BYTES]) const;
    bool DeserializeState(const uint8_t* bytes,
                          size_t length,
                          State& out) const;
    JournalOpenResult ScanJournal(Scan& out) const;
    bool ReadRecord(uint32_t offset,
                    uint32_t fileBytes,
                    StoredRecord& out) const;
    JournalMutationResult EnsureCapacity(uint16_t incomingBytes);
    JournalMutationResult FindFirstGame(uint32_t& outPrefixBytes,
                                        uint32_t& outNextSequence,
                                        uint32_t& outSealSequence,
                                        bool& outSealed) const;
    JournalMutationResult CompactPrefix(uint32_t prefixBytes,
                                        uint32_t nextOldestSequence,
                                        PendingCompaction kind,
                                        bool prepareState);
    JournalMutationResult ResumePendingCompaction();
    void PublishSummary(const Scan& scan);
    void AdvanceLayoutEpoch();

    GameHistoryJournalStorageOps m_ops;
    State m_state{};
    JournalArtifact m_stateArtifact = JournalArtifact::StateA;
    JournalSummary m_summary{};
    uint32_t m_layoutEpoch = 1U;
    // Locations established by a full scan or exact appended-tail readback.
    // Positions are still decoded from storage on every read.
    uint32_t m_latestOffset = 0U;
    uint16_t m_latestLength = 0U;
    bool m_open = false;
};

} // namespace modules::game_history
