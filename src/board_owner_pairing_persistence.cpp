#include "modules/board_owner_pairing_controller.hpp"

namespace modules {

bool BoardOwnerPairingController::BeginSave(BoardOwnerRecord record,
                                             Phase phase) {
    if (m_adding && record == BoardOwnerRecord::Clearing)
        return BeginAddingJournal(AddJournalPurpose::Rollback);
    ClearEffect();
    if (m_operation == Operation::Pair && phase == Phase::SavingTombstone &&
        m_pairFailureReason == Reason::None) m_pairFailureReason = m_reason;
    BoardOwnerPersistence::Receipt receipt = 0;
    if (!m_persistence.SaveOwnerRecord(record, receipt) || receipt == 0U) {
        if (m_operation == Operation::Pair && phase == Phase::SavingOwner) {
            m_reason = Reason::StorageFailed;
            (void)BeginSave(BoardOwnerRecord::Clearing, Phase::SavingTombstone);
            return false;
        } else {
            m_failedSavePhase = phase;
            FailRecovery(Reason::StorageFailed);
        }
        return false;
    }
    m_receipt = receipt;
    m_phase = phase;
    return true;
}

void BoardOwnerPairingController::PollPersistence() {
    if (m_receipt == 0U) return;
    if (m_phase == Phase::AwaitReceiptRelease) {
        (void)RetryReceiptRelease();
        return;
    }
    const auto outcome = m_persistence.GetOutcome(m_receipt);
    if (outcome == BoardOwnerPersistence::Outcome::Pending) return;
    const Phase completed = m_phase;
    if (!m_persistence.Release(m_receipt)) {
        m_releasePhase = completed;
        m_releaseOutcome = outcome;
        m_phase = Phase::AwaitReceiptRelease;
        m_reason = Reason::StorageFailed;
        ClearEffect();
        return;
    }
    m_receipt = 0;
    CompletePersistence(completed, outcome);
}

void BoardOwnerPairingController::CompletePersistence(
    Phase completed, BoardOwnerPersistence::Outcome outcome) {
    if (m_custodyRecheckRequired) {
        // A fresh Boot with the durable record and checked inventory must
        // reconcile this result after the retained receipt has been released.
        FailRecovery(Reason::StorageFailed);
        return;
    }
    if (m_adding) { CompleteAddPersistence(completed, outcome); return; }
    if (outcome == BoardOwnerPersistence::Outcome::Unknown ||
        outcome == BoardOwnerPersistence::Outcome::Failed) {
        if (completed == Phase::SavingOwner) {
            m_reason = Reason::StorageFailed;
            (void)BeginSave(BoardOwnerRecord::Clearing, Phase::SavingTombstone);
        } else {
            m_failedSavePhase = completed;
            FailRecovery(Reason::StorageFailed);
        }
        return;
    }
    if (completed == Phase::SavingOwner) {
        if (!m_linkPresent) {
            m_reason = Reason::SecurityFailed;
            (void)BeginSave(BoardOwnerRecord::Clearing, Phase::SavingTombstone);
            return;
        }
        m_ownership = Ownership::Owned;
        m_savedCount = 1U;
        m_reason = Reason::None;
        m_authenticated = m_linkPresent;
        m_successAwaitingAck = true;
        Emit(EffectType::PairSucceeded);
    } else if (completed == Phase::SavingTombstone) {
        m_ownership = Ownership::RecoveryRequired;
        if (m_operation != Operation::Pair) m_reason = Reason::None;
        m_phase = Phase::AwaitQuiesce;
        Emit(EffectType::QuiesceAndRejectWork);
    } else if (completed == Phase::SavingAbsent) {
        if (m_operation == Operation::Pair) {
            FinishPairFailure(m_pairFailureReason == Reason::None
                                  ? m_reason : m_pairFailureReason);
            return;
        }
        m_ownership = Ownership::Unpaired;
        m_savedCount = 0U;
        m_reason = Reason::None;
        if (m_operation == Operation::OrphanCleanup) {
            m_operation = Operation::None;
            m_phase = Phase::Idle;
            return;
        }
        m_successAwaitingAck = true;
        Emit(EffectType::UnpairSucceeded);
    }
}

bool BoardOwnerPairingController::RetryReceiptRelease() {
    if (m_phase != Phase::AwaitReceiptRelease || m_receipt == 0U) return false;
    const auto currentOutcome = m_persistence.GetOutcome(m_receipt);
    if (currentOutcome == BoardOwnerPersistence::Outcome::Pending) return false;
    m_releaseOutcome = currentOutcome;
    if (!m_persistence.Release(m_receipt)) return false;
    m_receipt = 0;
    const Phase completed = m_releasePhase;
    const auto outcome = m_releaseOutcome;
    m_releasePhase = Phase::Idle;
    m_releaseOutcome = BoardOwnerPersistence::Outcome::Unknown;
    CompletePersistence(completed, outcome);
    return true;
}

} // namespace modules
