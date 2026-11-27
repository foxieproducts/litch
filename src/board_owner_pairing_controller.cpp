#include "modules/board_owner_pairing_controller.hpp"
#include "modules/owner_pairing_session_wire.hpp"

namespace modules {

void BoardOwnerPairingController::Boot(BoardOwnerRecord record,
                                       BondInventory bonds) {
    if (m_receipt != 0U) {
        const Phase pending = m_phase == Phase::AwaitReceiptRelease
            ? m_releasePhase : m_phase;
        if (!bonds.available ||
            (pending == Phase::SavingOwner && bonds.count !=
                (m_adding ? m_addBaseline.saved.count + 1U : 1U)) ||
            (pending == Phase::SavingAbsent && (bonds.count != 0U || bonds.remoteMetadataPresent))) {
            // Release the accepted receipt exactly once, but do not turn its
            // success into live authority after custody evidence was lost.
            m_custodyRecheckRequired = true;
            m_ownership = Ownership::RecoveryRequired;
            m_authenticated = false;
            m_linkPresent = false;
        }
        m_reason = Reason::StorageFailed;
        if (m_phase != Phase::AwaitReceiptRelease) {
            m_releasePhase = m_phase;
            m_releaseOutcome = m_persistence.GetOutcome(m_receipt);
        }
        m_phase = Phase::AwaitReceiptRelease;
        ClearEffect();
        return;
    }
    m_operation = Operation::None;
    m_phase = Phase::Idle;
    m_reason = Reason::None;
    m_pairFailureReason = Reason::None;
    m_effect = {};
    m_receipt = 0;
    m_session = 0;
    m_connection = 0;
    m_epoch = 0;
    m_end = 0;
    m_savedCount = 0U;
    m_adding = false;
    m_addRollback = false;
    m_addBaseline = {};
    m_authenticated = false;
    m_linkPresent = false;
    m_successAwaitingAck = false;
    m_custodyRecheckRequired = false;
    m_failedSavePhase = Phase::Idle;
    m_releasePhase = Phase::Idle;
    m_releaseOutcome = BoardOwnerPersistence::Outcome::Unknown;

    if (!bonds.available) {
        FailRecovery(Reason::StorageFailed);
        return;
    }
    if (record == BoardOwnerRecord::Absent && bonds.count == 0U && !bonds.remoteMetadataPresent) {
        m_ownership = Ownership::Unpaired;
        return;
    }
    if (record == BoardOwnerRecord::Absent) {
        m_ownership = Ownership::RecoveryRequired;
        m_operation = Operation::OrphanCleanup;
        (void)BeginSave(BoardOwnerRecord::Clearing, Phase::SavingTombstone);
        return;
    }
    if (record == BoardOwnerRecord::Active && bonds.count >= 1U && bonds.count <= MAX_SAVED_PHONES) {
        m_ownership = Ownership::Owned;
        m_savedCount = bonds.count;
        return;
    }
    if (record == BoardOwnerRecord::Adding && bonds.count >= 1U && bonds.count <= MAX_SAVED_PHONES) {
        m_addBaseline = m_persistence.AddingBaseline();
        if (!m_addBaseline.Valid()) { FailRecovery(Reason::StorageFailed); return; }
        m_adding = m_addRollback = true;
        m_savedCount = m_addBaseline.saved.count;
        m_ownership = Ownership::RecoveryRequired;
        m_operation = Operation::Pair;
        m_phase = Phase::AwaitQuiesce;
        Emit(EffectType::QuiesceAndRejectWork);
        return;
    }
    if (record == BoardOwnerRecord::Clearing) {
        m_ownership = Ownership::RecoveryRequired;
        m_operation = Operation::Unpair;
        m_phase = Phase::AwaitQuiesce;
        Emit(EffectType::QuiesceAndRejectWork);
        return;
    }
    FailRecovery(Reason::StorageFailed);
}

bool BoardOwnerPairingController::Pairable() const {
    return m_ownership == Ownership::Unpaired &&
           m_operation == Operation::None && m_phase == Phase::Idle;
}

bool BoardOwnerPairingController::ArmPair(uint16_t session,
                                          uint16_t connection,
                                          uint32_t epoch,
                                          uint8_t physicalEnd,
                                          const BoardOwnerAddBaseline& baseline) {
    const bool adding = m_ownership == Ownership::Owned && m_operation == Operation::None &&
        m_phase == Phase::Idle && !m_authenticated && baseline.Valid() &&
        baseline.saved.count == m_savedCount;
    if ((!Pairable() && !adding) || session == 0U || epoch == 0U ||
        physicalEnd > owner_pairing_session_wire::EITHER_END ||
        (m_linkPresent &&
         (connection != m_connection || epoch != m_epoch))) return false;
    m_operation = Operation::Pair;
    m_phase = Phase::Armed;
    m_reason = Reason::None;
    m_pairFailureReason = Reason::None;
    m_session = session;
    m_connection = connection;
    m_epoch = epoch;
    m_end = physicalEnd;
    m_linkPresent = true;
    m_adding = adding;
    m_addRollback = false;
    m_addBaseline = adding ? baseline : BoardOwnerAddBaseline{};
    return true;
}

bool BoardOwnerPairingController::PairAddressMatches(
    uint16_t session, uint16_t connection, uint32_t epoch, uint8_t end) const {
    return session != 0U && session == m_session &&
           connection == m_connection && epoch == m_epoch && end == m_end;
}

bool BoardOwnerPairingController::CancelPair(
    uint16_t session, uint16_t connection, uint32_t epoch, Reason reason) {
    if (m_operation != Operation::Pair || m_phase != Phase::Armed ||
        !PairAddressMatches(session, connection, epoch, m_end) ||
        (reason != Reason::None && reason != Reason::Timeout)) {
        return false;
    }
    m_operation = Operation::None;
    m_phase = Phase::Idle;
    m_reason = reason;
    m_session = 0U;
    m_adding = false;
    m_addBaseline = {};
    ClearEffect();
    return true;
}

bool BoardOwnerPairingController::ConfirmPair(uint16_t session,
                                              uint16_t connection,
                                              uint32_t epoch,
                                              uint8_t physicalEnd) {
    if (m_operation != Operation::Pair || m_phase != Phase::Armed || physicalEnd > 1U ||
        !PairAddressMatches(session, connection, epoch, m_end) ||
        (m_end != owner_pairing_session_wire::EITHER_END && physicalEnd != m_end)) {
        return false;
    }
    m_end = physicalEnd;
    if (m_adding) return BeginAddingJournal(AddJournalPurpose::Consent);
    m_phase = Phase::AwaitSecurity;
    Emit(EffectType::RequestSecurity);
    return true;
}

bool BoardOwnerPairingController::ArmUnpair(uint16_t session,
                                            uint8_t physicalEnd) {
    if (m_ownership != Ownership::Owned || m_operation != Operation::None ||
        m_phase != Phase::Idle || session == 0U || physicalEnd > 1U) {
        return false;
    }
    m_operation = Operation::Unpair;
    m_phase = Phase::Armed;
    m_reason = Reason::None;
    m_session = session;
    m_end = physicalEnd;
    return true;
}

bool BoardOwnerPairingController::CancelUnpair(uint16_t session,
                                               uint8_t physicalEnd) {
    if (m_operation != Operation::Unpair || m_phase != Phase::Armed ||
        session == 0U || session != m_session || physicalEnd != m_end) {
        return false;
    }
    m_operation = Operation::None;
    m_phase = Phase::Idle;
    m_reason = Reason::None;
    m_session = 0U;
    ClearEffect();
    return true;
}

bool BoardOwnerPairingController::ConfirmUnpair(uint16_t session,
                                                uint8_t physicalEnd) {
    if (m_operation != Operation::Unpair || m_phase != Phase::Armed ||
        session == 0U || session != m_session || physicalEnd != m_end) {
        return false;
    }
    return BeginSave(BoardOwnerRecord::Clearing, Phase::SavingTombstone);
}

bool BoardOwnerPairingController::RegisterAuthenticatedOwnerConnection(
    uint16_t connection, uint32_t epoch, ConnectionSecurity security) {
    if (m_ownership != Ownership::Owned || m_operation != Operation::None ||
        m_phase != Phase::Idle || epoch == 0U || !security.IsSavedOwner() ||
        security.bonds.count != m_savedCount ||
        (m_linkPresent &&
         (connection != m_connection || epoch != m_epoch))) {
        return false;
    }
    m_connection = connection;
    m_epoch = epoch;
    m_linkPresent = true;
    m_authenticated = true;
    return true;
}

BoardOwnerPairingController::AckResult
BoardOwnerPairingController::AcknowledgeSecurity(
    uint32_t token, uint16_t session, uint16_t connection, uint32_t epoch, uint8_t physicalEnd,
    ConnectionSecurity security) {
    if (m_operation != Operation::Pair || m_phase != Phase::AwaitSecurity ||
        m_effect.type != EffectType::RequestSecurity ||
        m_effect.token != token ||
        !PairAddressMatches(session, connection, epoch, physicalEnd)) {
        return AckResult::Stale;
    }
    ClearEffect();
    const bool exactCustody = m_adding
        ? security.bonds.count == m_addBaseline.saved.count + 1U &&
          security.bonds.identities.count == security.bonds.count &&
          security.bonds.identities.ContainsAll(m_addBaseline.saved)
        : security.bonds.count == 1U;
    if (!security.IsSavedOwner() || !exactCustody) {
        m_reason = security.bonds.available ? Reason::SecurityFailed : Reason::StorageFailed;
        // A failed native write can have reached NVS before the SDK restores
        // its old RAM count/flags. Zero RAM custody is not durable rollback.
        if (m_adding || !security.nativeSucceeded || !security.bonds.available ||
            security.bonded || security.bonds.count != 0U) {
            (void)BeginSave(BoardOwnerRecord::Clearing, Phase::SavingTombstone);
        } else {
            FinishPairFailure(m_reason);
        }
        return AckResult::AcceptedFailure;
    }
    return BeginSave(BoardOwnerRecord::Active, Phase::SavingOwner)
               ? AckResult::AcceptedSuccess : AckResult::AcceptedFailure;
}

BoardOwnerPairingController::AckResult
BoardOwnerPairingController::AcknowledgeQuiesce(uint32_t token, bool success) {
    if (m_operation == Operation::None || m_phase != Phase::AwaitQuiesce ||
        m_effect.type != EffectType::QuiesceAndRejectWork || m_effect.token != token) {
        return AckResult::Stale;
    }
    ClearEffect();
    if (!success) {
        FailRecovery(Reason::Busy);
        return AckResult::AcceptedFailure;
    }
    m_phase = Phase::AwaitBondDelete;
    Emit(m_adding ? EffectType::RollbackAddedPeers : EffectType::DeleteAllBonds);
    return AckResult::AcceptedSuccess;
}

BoardOwnerPairingController::AckResult
BoardOwnerPairingController::AcknowledgeBondDelete(uint32_t token, bool success) {
    if (m_phase != Phase::AwaitBondDelete ||
        m_effect.type != (m_adding ? EffectType::RollbackAddedPeers : EffectType::DeleteAllBonds) ||
        m_effect.token != token) {
        return AckResult::Stale;
    }
    ClearEffect();
    if (!success) {
        FailRecovery(Reason::BondDeleteFailed);
        return AckResult::AcceptedFailure;
    }
    return BeginSave(m_adding ? BoardOwnerRecord::Active : BoardOwnerRecord::Absent,
                     m_adding ? Phase::SavingAddRestored : Phase::SavingAbsent)
               ? AckResult::AcceptedSuccess : AckResult::AcceptedFailure;
}

BoardOwnerPairingController::AckResult
BoardOwnerPairingController::AcknowledgeSuccess(uint32_t token) {
    if (!m_successAwaitingAck ||
        (m_effect.type != EffectType::PairSucceeded &&
         m_effect.type != EffectType::UnpairSucceeded) || m_effect.token != token) {
        return AckResult::Stale;
    }
    const bool wasUnpair = m_operation == Operation::Unpair;
    m_successAwaitingAck = false;
    ClearEffect();
    if (wasUnpair && m_authenticated) {
        m_phase = Phase::AwaitDisconnect;
        Emit(EffectType::Disconnect);
    } else {
        m_operation = Operation::None;
        m_phase = Phase::Idle;
        m_adding = false;
        m_addBaseline = {};
    }
    return AckResult::AcceptedSuccess;
}

BoardOwnerPairingController::AckResult
BoardOwnerPairingController::AcknowledgeDisconnect(uint32_t token, bool success) {
    if (m_phase != Phase::AwaitDisconnect ||
        m_effect.type != EffectType::Disconnect || m_effect.token != token) {
        return AckResult::Stale;
    }
    ClearEffect();
    if (!success) {
        FailRecovery(Reason::WrongConnection);
        return AckResult::AcceptedFailure;
    }
    m_authenticated = false;
    m_linkPresent = false;
    m_operation = Operation::None;
    m_phase = Phase::Idle;
    return AckResult::AcceptedSuccess;
}

void BoardOwnerPairingController::Disconnect(uint16_t connection,
                                             uint32_t epoch) {
    const bool matchingLiveLink =
        m_linkPresent && connection == m_connection && epoch == m_epoch;
    if (!matchingLiveLink) return;
    m_authenticated = false;
    m_linkPresent = false;
    if (m_operation == Operation::Pair && m_phase == Phase::Armed) {
        // Before consent there are no native writes to roll back. Retire the
        // Add baseline too, so a later explicit Unpair keeps its all-peer path.
        (void)CancelPair(m_session, connection, epoch);
        return;
    }
    if (m_operation == Operation::Pair &&
        m_phase == Phase::AwaitSecurity) {
        // The disconnect races bond creation: absence of a security callback
        // cannot prove that the stack created no bond. Invalidate the security
        // token and delete any possible bond before making the board pairable.
        m_reason = Reason::SecurityFailed;
        (void)BeginSave(BoardOwnerRecord::Clearing, Phase::SavingTombstone);
        return;
    }
    // An armed Unpair belongs to the board's local confirmation context.
    // Retiring a phone link must not cancel that offline-capable gesture.
}

bool BoardOwnerPairingController::RetryBondDelete() {
    if (m_custodyRecheckRequired || m_phase != Phase::Failed ||
        m_reason != Reason::BondDeleteFailed) return false;
    m_reason = Reason::None;
    m_phase = Phase::AwaitBondDelete;
    Emit(m_adding ? EffectType::RollbackAddedPeers : EffectType::DeleteAllBonds);
    return true;
}

bool BoardOwnerPairingController::RetryFinalSave() {
    if (m_custodyRecheckRequired || m_phase != Phase::Failed || m_operation == Operation::None ||
        m_reason != Reason::StorageFailed ||
        m_failedSavePhase != (m_adding ? Phase::SavingAddRestored : Phase::SavingAbsent)) return false;
    m_reason = Reason::None;
    return BeginSave(m_adding ? BoardOwnerRecord::Active : BoardOwnerRecord::Absent,
                     m_adding ? Phase::SavingAddRestored : Phase::SavingAbsent);
}

bool BoardOwnerPairingController::RetryQuiesce() {
    if (m_custodyRecheckRequired || m_phase != Phase::Failed || m_operation == Operation::None ||
        m_reason != Reason::Busy) return false;
    m_reason = Reason::None;
    m_phase = Phase::AwaitQuiesce;
    Emit(EffectType::QuiesceAndRejectWork);
    return true;
}

bool BoardOwnerPairingController::RetryClearingSave() {
    if (m_custodyRecheckRequired || m_phase != Phase::Failed || m_operation == Operation::None ||
        m_reason != Reason::StorageFailed ||
        m_failedSavePhase != Phase::SavingTombstone) return false;
    m_reason = Reason::None;
    return BeginSave(BoardOwnerRecord::Clearing, Phase::SavingTombstone);
}

void BoardOwnerPairingController::Emit(EffectType type) {
    ++m_nextEffectToken;
    if (m_nextEffectToken == 0U) ++m_nextEffectToken;
    m_effect = {type, m_session, m_connection, m_epoch, m_end,
                m_nextEffectToken};
}

void BoardOwnerPairingController::ClearEffect() { m_effect = {}; }

void BoardOwnerPairingController::FinishPairFailure(Reason reason) {
    m_ownership = Ownership::Unpaired;
    m_operation = Operation::None;
    m_phase = Phase::Idle;
    m_reason = reason;
    m_authenticated = false;
    m_linkPresent = false;
    ClearEffect();
}

void BoardOwnerPairingController::FailRecovery(Reason reason) {
    m_ownership = Ownership::RecoveryRequired;
    m_phase = Phase::Failed;
    m_reason = reason;
    ClearEffect();
}

} // namespace modules
