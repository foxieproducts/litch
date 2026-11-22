#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/current_game_sync_source.hpp"

namespace modules::current_game_sync {

enum class TransferInputResult : uint8_t {
    Accepted = 0U,
    Rejected,
    Dropped,
    IoFailure,
};

enum class ArchiveAdmission : uint8_t { Deferred = 0U,
                                        Rejected,
                                        Accepted };

struct ArchiveOps {
    void* context = nullptr;
    // Runs on the settings-persistence owner. Accepted means the exact pointer
    // mutation was admitted; that owner must later call ArchiveCompleted with
    // this transfer's operation token and the post-save durable pointer.
    ArchiveAdmission (*admit)(void*, const current_game_log::CommitPointer&, const Digest&) = nullptr;
};

// Dormant transport owner for one current-line transaction. The embedding BLE
// owner supplies the current durable pointer on every boundary and owns the
// asynchronous archive settings mutation.
class CurrentGameSyncTransfer final {
  public:
    CurrentGameSyncTransfer(CurrentGameSyncSource& source, ArchiveOps archive = {})
        : m_source(source), m_archive(archive) {}

    TransferInputResult HandleBegin(const uint8_t* bytes, size_t length, uint32_t connectionEpoch, bool ownerReady, bool otaBusy, uint16_t frameCapacity, const current_game_log::CommitPointer& authority);
    TransferInputResult HandleCommit(const uint8_t* bytes, size_t length, uint32_t connectionEpoch, bool ownerReady, bool otaBusy, const current_game_log::CommitPointer& authority);
    TransferInputResult HandleArchiveAck(const uint8_t* bytes, size_t length, uint32_t connectionEpoch, bool ownerReady, bool otaBusy, const current_game_log::CommitPointer& authority);
    // Call on the same settings-persistence owner after the admitted save's
    // callback. A mismatched epoch/token is stale and is deliberately ignored.
    void ArchiveCompleted(uint32_t connectionEpoch, uint32_t operationToken, bool succeeded, const current_game_log::CommitPointer& authority);
    [[nodiscard]] uint32_t PendingArchiveOperationToken() const {
        return m_archiveOperationToken;
    }

    bool NextNotification(uint32_t connectionEpoch,
                          const current_game_log::CommitPointer& authority,
                          uint8_t* out,
                          size_t outCapacity,
                          size_t& outLength);
    bool NotificationAccepted(uint32_t connectionEpoch, uint32_t token);
    [[nodiscard]] uint32_t PendingNotificationToken() const { return m_pendingToken; }
    [[nodiscard]] bool HasNotification() const;
    [[nodiscard]] bool AwaitingCommit() const;
    void Abandon();

  private:
    enum class Phase : uint8_t {
        Idle,
        State,
        Bounds,
        SegmentBegin,
        Data,
        AwaitCommit,
        Committed,
        ArchivePending,
        Archived,
        Reject,
        Done,
    };

    bool SameAuthority(const current_game_log::CommitPointer& authority) const;
    bool SameBegin(const uint8_t* bytes, size_t length) const;
    bool SameCommit(const uint8_t* bytes, size_t length) const;
    bool PlanIsCurrent(const current_game_log::CommitPointer& authority);
    void SetTransactionId(uint16_t id);
    bool SetReject(uint16_t transactionId, uint8_t opcode, game_history::RejectReason reason, uint16_t expected, uint32_t detail = 0U);
    TransferInputResult QueueBusy(uint16_t transactionId,
                                  game_history::RejectReason reason =
                                      game_history::RejectReason::Busy);
    bool IsArchiveAdoption(const current_game_log::CommitPointer& authority) const;
    bool BuildFrame(game_history::Opcode opcode, const uint8_t* payload, uint16_t payloadLength, uint8_t* out, size_t outCapacity, size_t& outLength);
    bool PrepareNotification(uint8_t* out, size_t outCapacity, size_t& outLength);

    CurrentGameSyncSource& m_source;
    ArchiveOps m_archive;
    Phase m_phase = Phase::Idle;
    uint32_t m_connectionEpoch = 0U;
    uint16_t m_transactionId = 0U;
    uint16_t m_nextFrameSequence = 0U;
    uint16_t m_frameCapacity = game_history::MIN_ATT_PAYLOAD_BYTES;
    uint16_t m_segmentOffset = 0U;
    uint16_t m_pendingDataLength = 0U;
    uint16_t m_busyTransactionId = 0U;
    SourceSnapshot m_snapshot{};
    SegmentPlan m_plan{};
    game_history::CommitPayload m_commit{};
    game_history::RejectPayload m_reject{};
    std::array<uint8_t, game_history::MIN_ATT_PAYLOAD_BYTES> m_beginBytes{};
    size_t m_beginLength = 0U;
    std::array<uint8_t, game_history::MIN_ATT_PAYLOAD_BYTES> m_commitBytes{};
    size_t m_commitLength = 0U;
    current_game_log::CommitPointer m_authority{};
    std::array<uint8_t, game_history::MAX_FRAME_BYTES> m_pendingFrame{};
    size_t m_pendingLength = 0U;
    uint32_t m_pendingToken = 0U;
    uint32_t m_lastDeliveryToken = 0U;
    uint16_t m_archiveAckSequence = 0U;
    uint32_t m_archiveOperationToken = 0U;
    uint32_t m_lastArchiveOperationToken = 0U;
    bool m_archiveSucceeded = false;
    std::array<uint8_t, game_history::MIN_ATT_PAYLOAD_BYTES> m_archivedFrame{};
    size_t m_archivedLength = 0U;
};

} // namespace modules::current_game_sync
