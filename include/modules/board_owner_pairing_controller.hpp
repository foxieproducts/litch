#pragma once

#include <cstdint>

#include "modules/ble_owner_pairing_protocol.hpp"
#include "modules/board_owner_persistence.hpp"

namespace modules {

class BoardOwnerPairingController final {
  public:
    using ProtocolState = ble_owner_pairing::PairingState;
    using Reason = ble_owner_pairing::Reason;

    enum class Ownership : uint8_t { Unpaired, Owned, RecoveryRequired };
    enum class Operation : uint8_t { None, Pair, Unpair, OrphanCleanup };
    enum class Phase : uint8_t {
        Idle,
        Armed,
        AwaitSecurity,
        SavingOwner,
        SavingTombstone,
        AwaitQuiesce,
        AwaitBondDelete,
        SavingAbsent,
        AwaitReceiptRelease,
        AwaitDisconnect,
        Failed,
        SavingAddJournal,
        SavingAddRestored,
    };
    enum class EffectType : uint8_t {
        None,
        RequestSecurity,
        QuiesceAndRejectWork,
        DeleteAllBonds,
        PairSucceeded,
        UnpairSucceeded,
        Disconnect,
        RollbackAddedPeers,
    };
    enum class AckResult : uint8_t { Stale, AcceptedSuccess, AcceptedFailure };

    struct Effect {
        EffectType type = EffectType::None;
        uint16_t session = 0;
        uint16_t connection = 0;
        uint32_t epoch = 0;
        uint8_t physicalEnd = 0;
        uint32_t token = 0;
    };

    // Only a complete error-checked native inventory may supply a count.
    // An unavailable store is never evidence that the board has no owner keys.
    struct BondInventory {
        bool available = false;
        uint8_t count = 0;
        BoardOwnerPeerSet identities{};
    };

    struct ConnectionSecurity {
        BondInventory bonds;
        bool nativeSucceeded = false;
        bool encrypted = false;
        bool bonded = false;
        bool peerMatches = false;

        bool IsSavedOwner() const {
            return nativeSucceeded && encrypted && bonded && peerMatches &&
                   bonds.available && bonds.count >= 1U && bonds.count <= MAX_SAVED_PHONES;
        }
    };

    explicit BoardOwnerPairingController(BoardOwnerPersistence& persistence)
        : m_persistence(persistence) {}

    void Boot(BoardOwnerRecord record, BondInventory bonds);
    // End 2 leaves initial consent open to either bank; ConfirmPair must name
    // one actual bank, which becomes part of every subsequent native effect.
    bool ArmPair(uint16_t session, uint16_t connection, uint32_t epoch,
                 uint8_t physicalEnd, const BoardOwnerAddBaseline& baseline = {});
    // Cancel/expiry retires only an unconfirmed prompt on this exact link.
    // Once native security starts, teardown must use the addressed disconnect
    // and bond rollback path, rather than pretending no keys could exist.
    bool CancelPair(uint16_t session, uint16_t connection, uint32_t epoch,
                    Reason reason = Reason::None);
    bool ConfirmPair(uint16_t session, uint16_t connection, uint32_t epoch,
                     uint8_t physicalEnd);
    bool ArmUnpair(uint16_t session, uint8_t physicalEnd);
    bool CancelUnpair(uint16_t session, uint8_t physicalEnd);
    bool ConfirmUnpair(uint16_t session, uint8_t physicalEnd);
    bool RegisterAuthenticatedOwnerConnection(uint16_t connection,
                                              uint32_t epoch,
                                              ConnectionSecurity security);
    AckResult AcknowledgeSecurity(uint32_t token, uint16_t session,
                                  uint16_t connection, uint32_t epoch,
                                  uint8_t physicalEnd, ConnectionSecurity security);
    AckResult AcknowledgeQuiesce(uint32_t token, bool success);
    AckResult AcknowledgeBondDelete(uint32_t token, bool success);
    AckResult AcknowledgeDisconnect(uint32_t token, bool success);
    AckResult AcknowledgeSuccess(uint32_t token);
    void Disconnect(uint16_t connection, uint32_t epoch);
    void PollPersistence();

    bool RetryBondDelete();
    bool RetryClearingSave();
    bool RetryFinalSave();
    bool RetryQuiesce();
    bool RetryReceiptRelease();

    Ownership GetOwnership() const { return m_ownership; }
    Operation GetOperation() const { return m_operation; }
    Phase GetPhase() const { return m_phase; }
    Reason GetReason() const { return m_reason; }
    bool Pairable() const;
    bool IsAdding() const { return m_adding; }
    uint8_t SavedPhoneCount() const { return m_savedCount; }
    const BoardOwnerAddBaseline& AddBaseline() const { return m_addBaseline; }
    bool HasAuthenticatedOwnerConnection() const { return m_authenticated; }
    Effect PendingEffect() const { return m_effect; }

  private:
    bool PairAddressMatches(uint16_t session, uint16_t connection,
                            uint32_t epoch, uint8_t end) const;
    bool BeginSave(BoardOwnerRecord record, Phase phase);
    enum class AddJournalPurpose : uint8_t { Consent, Rollback };
    bool BeginAddingJournal(AddJournalPurpose purpose);
    void CompleteAddPersistence(Phase completed, BoardOwnerPersistence::Outcome outcome);
    void Emit(EffectType type);
    void ClearEffect();
    void CompletePersistence(Phase completed,
                             BoardOwnerPersistence::Outcome outcome);
    void FinishPairFailure(Reason reason);
    void FailRecovery(Reason reason);

    BoardOwnerPersistence& m_persistence;
    Ownership m_ownership = Ownership::RecoveryRequired;
    Operation m_operation = Operation::None;
    Phase m_phase = Phase::Idle;
    Reason m_reason = Reason::None;
    Reason m_pairFailureReason = Reason::None;
    Effect m_effect{};
    BoardOwnerPersistence::Receipt m_receipt = 0;
    uint16_t m_session = 0;
    uint16_t m_connection = 0;
    uint32_t m_epoch = 0;
    uint32_t m_nextEffectToken = 0;
    uint8_t m_end = 0;
    uint8_t m_savedCount = 0U;
    BoardOwnerAddBaseline m_addBaseline{};
    bool m_adding = false;
    bool m_addRollback = false;
    bool m_authenticated = false;
    bool m_linkPresent = false;
    bool m_successAwaitingAck = false;
    bool m_custodyRecheckRequired = false;
    Phase m_failedSavePhase = Phase::Idle;
    Phase m_releasePhase = Phase::Idle;
    BoardOwnerPersistence::Outcome m_releaseOutcome =
        BoardOwnerPersistence::Outcome::Unknown;
};

} // namespace modules
