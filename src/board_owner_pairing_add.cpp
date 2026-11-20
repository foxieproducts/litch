#include "modules/board_owner_pairing_controller.hpp"

namespace modules {

bool BoardOwnerPairingController::BeginAddingJournal(AddJournalPurpose purpose) {
    ClearEffect();
    m_addRollback = purpose == AddJournalPurpose::Rollback;
    if (m_addRollback && m_pairFailureReason == Reason::None) m_pairFailureReason = m_reason;
    BoardOwnerPersistence::Receipt receipt = 0U;
    if (!m_adding || !m_addBaseline.Valid() ||
        !m_persistence.SaveAddingJournal(m_addBaseline, receipt) || receipt == 0U) {
        m_failedSavePhase = Phase::SavingAddJournal;
        FailRecovery(Reason::StorageFailed);
        return false;
    }
    m_receipt = receipt;
    m_phase = Phase::SavingAddJournal;
    return true;
}

void BoardOwnerPairingController::CompleteAddPersistence(
    Phase completed, BoardOwnerPersistence::Outcome outcome) {
    if (outcome != BoardOwnerPersistence::Outcome::Saved) {
        m_reason = Reason::StorageFailed;
        if (completed == Phase::SavingOwner) {
            (void)BeginAddingJournal(AddJournalPurpose::Rollback);
        } else {
            m_failedSavePhase = completed;
            FailRecovery(Reason::StorageFailed);
        }
        return;
    }
    if (completed == Phase::SavingAddJournal) {
        if (m_addRollback || !m_linkPresent) {
            m_ownership = Ownership::RecoveryRequired;
            m_phase = Phase::AwaitQuiesce;
            Emit(EffectType::QuiesceAndRejectWork);
        } else {
            // The exact tracked journal is durable and released before native
            // pairing can write any key or RPA metadata for this connection.
            m_phase = Phase::AwaitSecurity;
            Emit(EffectType::RequestSecurity);
        }
    } else if (completed == Phase::SavingOwner) {
        if (!m_linkPresent) {
            m_reason = Reason::SecurityFailed;
            (void)BeginAddingJournal(AddJournalPurpose::Rollback);
            return;
        }
        m_savedCount = m_addBaseline.saved.count + 1U;
        m_ownership = Ownership::Owned;
        m_authenticated = true;
        m_reason = Reason::None;
        m_successAwaitingAck = true;
        Emit(EffectType::PairSucceeded);
    } else if (completed == Phase::SavingAddRestored) {
        m_savedCount = m_addBaseline.saved.count;
        m_ownership = Ownership::Owned;
        m_operation = Operation::None;
        m_phase = Phase::Idle;
        m_reason = m_pairFailureReason;
        m_authenticated = false;
        m_linkPresent = false;
        m_adding = false;
        m_addRollback = false;
        m_addBaseline = {};
        ClearEffect();
    } else {
        FailRecovery(Reason::StorageFailed);
    }
}

} // namespace modules
