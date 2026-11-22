#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/game_history_journal.hpp"

namespace modules::game_history {

enum class TransferInputResult : uint8_t {
    Accepted = 0U,
    Rejected,
    Dropped,
    IoFailure,
};

class GameHistoryTransfer final {
  public:
    explicit GameHistoryTransfer(GameHistoryJournal& journal)
        : m_journal(journal) {}

    TransferInputResult HandleBegin(const uint8_t* bytes,
                                    size_t length,
                                    uint32_t connectionEpoch,
                                    bool ownerReady,
                                    bool otaBusy,
                                    uint16_t frameCapacity);
    TransferInputResult HandleCommit(const uint8_t* bytes,
                                     size_t length,
                                     uint32_t connectionEpoch,
                                     bool ownerReady,
                                     bool otaBusy);

    bool NextNotification(uint32_t connectionEpoch,
                          uint8_t* out,
                          size_t outCapacity,
                          size_t& outLength);
    // Preparing output does not consume it. Call only after queue acceptance.
    bool NotificationAccepted(uint32_t connectionEpoch, uint32_t token);
    [[nodiscard]] uint32_t PendingNotificationToken() const { return m_pendingToken; }
    void Abandon();

    [[nodiscard]] bool HasNotification() const;
    [[nodiscard]] bool AwaitingCommit() const;

  private:
    enum class Phase : uint8_t {
        Idle = 0U,
        State,
        Bounds,
        SegmentBegin,
        Data,
        AwaitCommit,
        Committed,
        Reject,
        Done,
    };

    bool SetReject(uint16_t transactionId,
                   uint8_t offendingOpcode,
                   RejectReason reason,
                   uint16_t expectedFrameSequence,
                   uint32_t detail = 0U);
    bool BuildFrame(Opcode opcode,
                    uint8_t flags,
                    const uint8_t* payload,
                    uint16_t payloadLength,
                    uint8_t* out,
                    size_t outCapacity,
                    size_t& outLength);
    bool SameBegin(const uint8_t* bytes, size_t length) const;
    bool SameCommit(const uint8_t* bytes, size_t length) const;
    bool PlanIsCurrent() const;
    bool PendingIsRejection() const;
    TransferInputResult QueueBusy(uint16_t transactionId, RejectReason reason);
    bool PrepareNotification(uint32_t connectionEpoch, uint8_t* out,
                             size_t outCapacity, size_t& outLength);

    GameHistoryJournal& m_journal;
    Phase m_phase = Phase::Idle;
    uint32_t m_connectionEpoch = 0U;
    uint16_t m_transactionId = 0U;
    uint16_t m_nextFrameSequence = 0U;
    uint16_t m_frameCapacity = MIN_ATT_PAYLOAD_BYTES;
    uint16_t m_segmentOffset = 0U;
    uint8_t m_stateFlags = 0U;
    StatePayload m_state{};
    StateBoundsPayload m_bounds{};
    JournalSegmentPlan m_plan{};
    CommitPayload m_commit{};
    RejectPayload m_reject{};
    std::array<uint8_t, MIN_ATT_PAYLOAD_BYTES> m_beginBytes{};
    size_t m_beginLength = 0U;
    std::array<uint8_t, MIN_ATT_PAYLOAD_BYTES> m_commitBytes{};
    size_t m_commitLength = 0U;
    uint16_t m_busyTransactionId = 0U;
    RejectReason m_busyReason = RejectReason::Busy;
    std::array<uint8_t, MAX_FRAME_BYTES> m_pendingFrame{};
    size_t m_pendingLength = 0U;
    uint32_t m_pendingToken = 0U;
    uint32_t m_lastDeliveryToken = 0U;
};

} // namespace modules::game_history
