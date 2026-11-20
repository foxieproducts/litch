#include "firmware/current_game_sync_runtime.hpp"

#ifdef ARDUINO

#include <algorithm>

namespace firmware {

using modules::current_game_sync::ArchiveAdmission;
using modules::current_game_sync::Opcode;

CurrentGameSyncRuntime::CurrentGameSyncRuntime(
    SettingsPersistenceWorker& worker,
    CurrentGameLogService& log,
    const modules::current_game_log::StorageOps storage,
    fxe::SettingsStore& settings)
    : m_worker(worker),
      m_log(log),
      m_settings(settings),
      m_source(storage),
      m_transfer(
          m_source,
          {this, &CurrentGameSyncRuntime::AdmitArchiveThunk}) {}

bool CurrentGameSyncRuntime::Configure(
    const JsonDocument& initialDocument) {
    if (IsConfigured() ||
        !m_log.Configure(m_worker, initialDocument) ||
        !m_worker.SetCurrentGameSyncServiceCallback(
            &ServiceThunk, this)) {
        return false;
    }
    PublishAuthority();
    m_configured.store(true, std::memory_order_release);
    return true;
}

bool CurrentGameSyncRuntime::Admit(
    const uint8_t* const bytes, const size_t size,
    const uint32_t connectionEpoch, const bool otaBusy) {
    uint8_t expected = Free;
    // Transport admission stays live after saving fails. The worker returns
    // the existing domain rejection without reading or writing unavailable storage.
    if (!IsConfigured() || !m_worker.IsStarted() || bytes == nullptr ||
        size < modules::game_history::FRAME_HEADER_BYTES ||
        size > m_input.size() || connectionEpoch == 0U ||
        !m_inputState.compare_exchange_strong(
            expected, Writing, std::memory_order_acq_rel)) {
        return false;
    }
    std::copy_n(bytes, size, m_input.begin());
    m_inputSize = static_cast<uint8_t>(size);
    m_inputEpoch = connectionEpoch;
    m_inputOtaBusy = otaBusy;
    m_inputState.store(Ready, std::memory_order_release);
    m_worker.NotifyCurrentGameSync();
    return true;
}

bool CurrentGameSyncRuntime::Peek(
    uint8_t* const out, const size_t capacity, size_t& outSize,
    uint32_t& token, const uint32_t connectionEpoch) {
    outSize = 0U;
    token = 0U;
    if (out == nullptr || connectionEpoch == 0U ||
        m_outputState.load(std::memory_order_acquire) != Ready ||
        connectionEpoch != m_outputEpoch || capacity < m_outputSize) {
        return false;
    }
    std::copy_n(m_output.begin(), m_outputSize, out);
    outSize = m_outputSize;
    token = m_outputToken;
    return token != 0U;
}

void CurrentGameSyncRuntime::Accept(
    const uint32_t token, const uint32_t connectionEpoch) {
    uint8_t expected = Ready;
    if (token == 0U || token != m_outputToken ||
        connectionEpoch != m_outputEpoch ||
        !m_outputState.compare_exchange_strong(
            expected, Accepted, std::memory_order_acq_rel)) {
        return;
    }
    m_worker.NotifyCurrentGameSync();
}

void CurrentGameSyncRuntime::ServiceMain() {
    if (!m_settings.IsDisabled() && IsConfigured() && m_log.ConsumeArchiveSaveRequest()) {
        (void)m_settings.SaveNow();
    }
}

bool CurrentGameSyncRuntime::CopyLinePublication(
    uint32_t& revision, std::array<uint8_t, 2U>& body) const {
    if (!IsConfigured() ||
        m_authorityLock.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    revision = m_lineStateRevision;
    body = {1U, static_cast<uint8_t>(m_authority.present ? 1U : 0U)};
    m_authorityLock.clear(std::memory_order_release);
    return true;
}

void CurrentGameSyncRuntime::PublishAuthority() {
    // Configure runs before worker startup; subsequent publications run only
    // on settings_io, after its exact snapshot completion. The lock covers
    // only this small copy and never storage, scheduling, or callbacks.
    const auto& committed = m_log.CommittedPointer();
    while (m_authorityLock.test_and_set(std::memory_order_acquire)) {}
    if (m_authority.gameId != committed.gameId ||
        m_authority.lineRevision != committed.lineRevision ||
        m_authority.currentPly != committed.currentPly ||
        m_authority.present != committed.present) {
        ++m_lineStateRevision;
        if (m_lineStateRevision == 0U || m_lineStateRevision == UINT32_MAX) {
            m_lineStateRevision = 1U;
        }
    }
    m_authority = {committed.gameId, committed.lineRevision,
                   committed.currentPly, committed.present,
                   committed.archiveRequired};
    m_authorityLock.clear(std::memory_order_release);
}

modules::ControlCoordinatorResult CurrentGameSyncRuntime::CheckReplacementAdmission(
    const modules::GameHistoryPersistenceState& current,
    const modules::GamePersistenceCoordinator::ReplacementKind kind) const {
    using Result = modules::ControlCoordinatorResult;
    using Kind = modules::GamePersistenceCoordinator::ReplacementKind;
    // Explicit New Game is board-first consent to abandon the prior live line.
    // Archival and saving remain best-effort; neither may block this action.
    // Engine ownership and interaction admission remain coordinator-owned.
    if (kind == Kind::NewGame) return Result::Applied;
    // An explicit session-only policy never retires or overwrites disk authority.
    // FEN/restore may replace RAM while the last verified saved line
    // remains untouched and returns after power loss. Healthy archive gates below
    // retain their exact durable acknowledgement requirement.
    if (m_settings.IsDisabled()) return Result::Applied;
    if (!IsConfigured() || !m_worker.IsStarted() || !current.valid ||
        !current.present || !modules::game_history::IsValidGameId(current.gameId) ||
        current.lineRevision == 0U || current.IsCaptureExhausted()) {
        return Result::StorageUnavailable;
    }
    if (m_authorityLock.test_and_set(std::memory_order_acquire)) {
        return Result::Busy;
    }
    const DurableAuthority authority = m_authority;
    m_authorityLock.clear(std::memory_order_release);
    if (!authority.present) {
        // A no-authority external import can save its explicitly partial
        // settings, but cannot be archived/reset as a complete current line.
        if (current.captureIncomplete) return Result::Unsupported;
        if (current.lineRevision == 1U && current.currentPly == 0U) {
            return Result::Applied;
        }
        return m_worker.GetLastError() == fxe::SettingsStoreError::None
            ? Result::Busy : Result::StorageUnavailable;
    }
    if (authority.gameId != current.gameId) {
        return authority.archiveRequired ? Result::ArchiveRequired
                                         : Result::StorageUnavailable;
    }
    if (current.captureIncomplete) return Result::Unsupported;
    if (authority.lineRevision != current.lineRevision ||
        authority.currentPly != current.currentPly) {
        return m_worker.GetLastError() == fxe::SettingsStoreError::None
            ? Result::Busy : Result::StorageUnavailable;
    }
    if (authority.archiveRequired) return Result::ArchiveRequired;
    // Partial-FEN replacement is not representable by the complete-line
    // owner. Reject before mutation even when its prior line was archived.
    if (kind == Kind::ExternalFen) return Result::Unsupported;
    return Result::Applied;
}

void CurrentGameSyncRuntime::ServiceThunk(void* const context) {
    if (context != nullptr) {
        static_cast<CurrentGameSyncRuntime*>(context)->ServiceWorker();
    }
}

ArchiveAdmission CurrentGameSyncRuntime::AdmitArchiveThunk(
    void* const context,
    const modules::current_game_log::CommitPointer& authority,
    const modules::current_game_sync::Digest& digest) {
    return context == nullptr
        ? ArchiveAdmission::Rejected
        : static_cast<CurrentGameSyncRuntime*>(context)
              ->m_log.AdmitArchiveAcknowledgement(authority, digest);
}

void CurrentGameSyncRuntime::ServiceWorker() {
    if (m_outputState.load(std::memory_order_acquire) == Accepted) {
        (void)m_transfer.NotificationAccepted(
            m_outputEpoch, m_outputToken);
        m_outputSize = 0U;
        m_outputToken = 0U;
        m_outputEpoch = 0U;
        m_outputState.store(Free, std::memory_order_release);
    }

    bool archiveSucceeded = false;
    modules::current_game_log::CommitPointer archiveAuthority;
    if (m_log.TakeArchiveCompletion(
            archiveSucceeded, archiveAuthority)) {
        m_transfer.ArchiveCompleted(
            m_activeEpoch,
            m_transfer.PendingArchiveOperationToken(),
            archiveSucceeded,
            archiveAuthority);
    }

    if (m_inputState.load(std::memory_order_acquire) == Ready) {
        const auto bytes = m_input;
        const size_t size = m_inputSize;
        const uint32_t epoch = m_inputEpoch;
        const bool otaBusy = m_inputOtaBusy;
        m_inputState.store(Free, std::memory_order_release);
        if (m_activeEpoch != epoch) {
            m_transfer.Abandon();
            m_activeEpoch = epoch;
        }
        const auto authority = m_log.CommittedPointer();
        const bool ownerReady = !m_settings.IsDisabled();
        const uint8_t opcode = bytes[2U];
        if (opcode == static_cast<uint8_t>(Opcode::Begin)) {
            (void)m_transfer.HandleBegin(
                bytes.data(), size, epoch, ownerReady, otaBusy, FRAME_BYTES,
                authority);
        } else if (opcode == static_cast<uint8_t>(Opcode::SegmentCommit)) {
            (void)m_transfer.HandleCommit(
                bytes.data(), size, epoch, ownerReady, otaBusy, authority);
        } else if (opcode == static_cast<uint8_t>(Opcode::ArchiveAck)) {
            (void)m_transfer.HandleArchiveAck(
                bytes.data(), size, epoch, ownerReady, otaBusy, authority);
        } else {
            // Let the protocol owner produce its canonical unsupported/bad
            // state rejection for an envelope that starts a transaction.
            (void)m_transfer.HandleBegin(
                bytes.data(), size, epoch, ownerReady, otaBusy, FRAME_BYTES,
                authority);
        }
    }

    PublishAuthority();
    PublishNextOutput();
}

void CurrentGameSyncRuntime::PublishNextOutput() {
    if (m_activeEpoch == 0U ||
        m_outputState.load(std::memory_order_acquire) != Free) {
        return;
    }
    size_t size = 0U;
    if (!m_transfer.NextNotification(
            m_activeEpoch, m_log.CommittedPointer(), m_output.data(),
            m_output.size(), size) || size == 0U ||
        size > m_output.size()) {
        return;
    }
    const uint32_t token = m_transfer.PendingNotificationToken();
    if (token == 0U) return;
    m_outputSize = static_cast<uint8_t>(size);
    m_outputToken = token;
    m_outputEpoch = m_activeEpoch;
    m_outputState.store(Ready, std::memory_order_release);
}

} // namespace firmware

#endif
