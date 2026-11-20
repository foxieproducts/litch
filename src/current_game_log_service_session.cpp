#include "firmware/current_game_log_service.hpp"

#ifdef ARDUINO

#include "logger.hpp"
#include "modules/current_game_log_persistence.hpp"

namespace firmware {

using modules::current_game_log::CommitPointer;
using modules::current_game_log::Result;
using modules::game_history::Record;

namespace {

bool SameAuthority(const CommitPointer& left, const CommitPointer& right) {
    return left.present == right.present && left.slot == right.slot &&
        left.generation == right.generation &&
        left.byteLength == right.byteLength &&
        left.prefixCrc32 == right.prefixCrc32 &&
        left.gameId == right.gameId &&
        left.lineRevision == right.lineRevision &&
        left.currentPly == right.currentPly &&
        left.archiveRequired == right.archiveRequired;
}

} // namespace

modules::current_game_sync::ArchiveAdmission
CurrentGameLogService::AdmitArchiveAcknowledgement(
    const CommitPointer& authority,
    const modules::current_game_sync::Digest& digest) {
    using modules::current_game_sync::ArchiveAdmission;
    if (!m_configured || !authority.present || !authority.archiveRequired ||
        !SameAuthority(authority, m_committedPointer)) {
        return ArchiveAdmission::Rejected;
    }
    if (m_archiveRequestPending) {
        return SameAuthority(authority, m_archiveRequestAuthority) &&
            digest.fingerprint == m_archiveRequestDigest.fingerprint
            ? ArchiveAdmission::Accepted
            : ArchiveAdmission::Deferred;
    }
    m_archiveRequestAuthority = authority;
    m_archiveRequestDigest = digest;
    m_archiveRequestPending = true;
    m_archiveSaveRequested.store(true, std::memory_order_release);
    return ArchiveAdmission::Accepted;
}

bool CurrentGameLogService::PrepareArchiveAcknowledgement(
    JsonDocument& document, const CommitPointer& authority) {
    const Result opened = EnsureOpen(authority);
    if (opened != Result::Ready) {
        m_archiveRequestPending = false;
        m_archiveCompletion.store(2U, std::memory_order_release);
        return Fail(opened);
    }
    m_preparedDocument = &document;
    CommitPointer candidate;
    modules::current_game_log::SettingsOps::Receipt receipt = 0U;
    const Result result = m_log.AcknowledgeArchived(authority, candidate, receipt);
    m_preparedDocument = nullptr;
    if (result != Result::Applied) {
        m_archiveRequestPending = false;
        m_archiveCompletion.store(2U, std::memory_order_release);
        return Fail(result);
    }
    m_archiveTransactionPrepared = true;
    m_lastResult = result;
    return true;
}

bool CurrentGameLogService::ConsumeArchiveSaveRequest() {
    return m_archiveSaveRequested.exchange(false, std::memory_order_acq_rel);
}

bool CurrentGameLogService::TakeArchiveCompletion(
    bool& outSucceeded, CommitPointer& outAuthority) {
    const uint8_t completion = m_archiveCompletion.exchange(
        0U, std::memory_order_acq_rel);
    if (completion == 0U) return false;
    outSucceeded = completion == 1U;
    outAuthority = m_committedPointer;
    return true;
}

void CurrentGameLogService::ResetLog() {
    m_log = modules::current_game_log::CurrentGameLog(
        m_storage, m_settingsOps);
}

Result CurrentGameLogService::EnsureOpen(const CommitPointer& authority) {
    Record latest;
    if (m_log.ReadLatest(authority, latest) == Result::Ready) {
        return Result::Ready;
    }
    ResetLog();
    return m_log.Open(authority);
}

bool CurrentGameLogService::Fail(const Result result) {
    if (result == Result::ReadbackMismatch && m_worker != nullptr) {
        m_worker->NoteSnapshotStorageFailure(fxe::SettingsStoreError::VerifyFailed);
    }
    m_lastResult = result;
    ++m_failureCount;
    logger::Warn("current_game_log: transaction failed result=%u",
                 static_cast<unsigned>(result));
    return false;
}

bool CurrentGameLogService::EnqueuePointer(
    const CommitPointer& pointer,
    modules::current_game_log::SettingsOps::Receipt& outReceipt) {
    outReceipt = 0U;
    if (m_preparedDocument == nullptr || m_pendingReceipt != 0U ||
        !modules::current_game_log::WriteCommitPointer(
            *m_preparedDocument, pointer)) {
        return false;
    }
    ++m_nextReceipt;
    if (m_nextReceipt == 0U) ++m_nextReceipt;
    m_pendingPointer = pointer;
    m_pendingReceipt = m_nextReceipt;
    outReceipt = m_pendingReceipt;
    return true;
}

bool CurrentGameLogService::PrepareReplacement(JsonDocument& document) {
    if (!modules::current_game_log::WriteCommitPointer(
            document, CommitPointer{})) {
        return Fail(Result::SettingsRejected);
    }
    m_pendingRemoval = true;
    m_lastResult = Result::Ready;
    return true;
}

void CurrentGameLogService::Complete(
    const SettingsPersistenceWorker::SnapshotTransactionOutcome outcome) {
    const bool saved =
        outcome == SettingsPersistenceWorker::SnapshotTransactionOutcome::Saved;
    if (m_pendingReceipt != 0U) {
        const auto receipt = m_pendingReceipt;
        const bool completed = saved
            ? m_log.ConfirmPromotion(m_pendingPointer, receipt)
            : m_log.RejectPromotion(receipt);
        if (!completed) {
            (void)Fail(Result::BadState);
        } else if (saved) {
            m_committedPointer = m_pendingPointer;
        }
    } else if (saved && m_pendingRemoval) {
        m_committedPointer = {};
        ResetLog();
    }
    m_pendingPointer = {};
    m_pendingReceipt = 0U;
    m_pendingRemoval = false;
    if (m_archiveTransactionPrepared) {
        m_archiveRequestPending = false;
        m_archiveRequestAuthority = {};
        m_archiveRequestDigest = {};
        m_archiveCompletion.store(saved ? 1U : 2U,
                                  std::memory_order_release);
        m_archiveTransactionPrepared = false;
    }
}

} // namespace firmware

#endif
